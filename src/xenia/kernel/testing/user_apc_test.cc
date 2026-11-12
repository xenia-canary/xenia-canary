/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// User-APC delivery (xeProcessUserApcs). A kernel routine is called as
// KernelRoutine(Apc, &NormalRoutine, &NormalContext, &SystemArgument1,
// &SystemArgument2), with the KAPC itself; Xenia used to pass the APC's list
// entry, KAPC + offsetof(XAPC, list_entry) = KAPC + 8, and the dummy kernel
// routine's branch freed that address too. A title library that frees its
// request contexts from its kernel routine (reading the KAPC's fields at
// +0x04 and +0x1C) then leaked one context per request.
//
// A user APC is queued on a guest KTHREAD laid out in guest memory and
// delivered by the real xeProcessUserApcs, which runs real guest routines
// (PPC code below) through the processor: the kernel routine records its
// arguments and the scratch slots it is handed, then rewrites the slots;
// the normal routine records the arguments it is called with.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/testing/kernel_state_test.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace test {
namespace {

// The list entry sits 8 bytes into the KAPC.
static_assert(offsetof(XAPC, list_entry) == 8, "XAPC::list_entry offset");

constexpr uint32_t kPage = 0x90040000;
constexpr uint32_t kPageSize = 0x10000;
constexpr uint32_t kKpcr = kPage;              // X_KPCR
constexpr uint32_t kKthread = kPage + 0x1000;  // X_KTHREAD
constexpr uint32_t kKapc = kPage + 0x2000;     // XAPC
constexpr uint32_t kRecord = kPage + 0x5000;   // what the routines saw
constexpr uint32_t kStackTop = kPage + 0x8000;

// Guest code: the kernel routine at kCode, the normal routine at kNormal.
constexpr uint32_t kCode = 0x82000000;
constexpr uint32_t kNormal = kCode + 0x100;
// The normal routine the APC is queued with. The kernel routine replaces
// it (through its scratch slot) with kNormal, so this one must never run.
constexpr uint32_t kQueuedNormal = kCode + 0x200;

// kernel routine(r3 = apc, r4..r7 = &normal_routine, &normal_context,
// &arg1, &arg2):
//   record r3..r7 at kRecord+0x00..0x10 and *r4..*r7 at kRecord+0x40..0x4C,
//   then *r4 = kNormal, *r5 = 0x5555, *r6 = 0x6666, *r7 = 0x7777.
constexpr uint32_t kKernelRoutineCode[] = {
    0x3D609004,  // lis   r11, 0x9004
    0x616B5000,  // ori   r11, r11, 0x5000      (kRecord)
    0x906B0000,  // stw   r3, 0(r11)
    0x908B0004,  // stw   r4, 4(r11)
    0x90AB0008,  // stw   r5, 8(r11)
    0x90CB000C,  // stw   r6, 0xC(r11)
    0x90EB0010,  // stw   r7, 0x10(r11)
    0x81840000,  // lwz   r12, 0(r4)
    0x918B0040,  // stw   r12, 0x40(r11)
    0x81850000,  // lwz   r12, 0(r5)
    0x918B0044,  // stw   r12, 0x44(r11)
    0x81860000,  // lwz   r12, 0(r6)
    0x918B0048,  // stw   r12, 0x48(r11)
    0x81870000,  // lwz   r12, 0(r7)
    0x918B004C,  // stw   r12, 0x4C(r11)
    0x3D808200,  // lis   r12, 0x8200
    0x618C0100,  // ori   r12, r12, 0x100       (kNormal)
    0x91840000,  // stw   r12, 0(r4)
    0x39805555,  // li    r12, 0x5555
    0x91850000,  // stw   r12, 0(r5)
    0x39806666,  // li    r12, 0x6666
    0x91860000,  // stw   r12, 0(r6)
    0x39807777,  // li    r12, 0x7777
    0x91870000,  // stw   r12, 0(r7)
    0x4E800020,  // blr
};
// normal routine(r3 = context, r4 = arg1, r5 = arg2): record them at
// kRecord+0x20..0x28 and 1 at kRecord+0x2C.
constexpr uint32_t kNormalRoutineCode[] = {
    0x3D609004,  // lis   r11, 0x9004
    0x616B5000,  // ori   r11, r11, 0x5000
    0x906B0020,  // stw   r3, 0x20(r11)
    0x908B0024,  // stw   r4, 0x24(r11)
    0x90AB0028,  // stw   r5, 0x28(r11)
    0x39800001,  // li    r12, 1
    0x918B002C,  // stw   r12, 0x2C(r11)
    0x4E800020,  // blr
};

class UserApcTest : public KernelStateTest {
 public:
  UserApcTest() : thread_(processor(), 0x101) {
    REQUIRE(memory()->LookupHeap(kPage)->AllocFixed(
        kPage, kPageSize, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    memory()->Zero(kPage, kPageSize);
    ctx_ = thread_.context();
    ctx_->kernel_state = kernel_state();  // the dummy routine's free
    ctx_->r[13] = kKpcr;
    ctx_->r[1] = kStackTop;

    auto* kpcr = memory()->TranslateVirtual<X_KPCR*>(kKpcr);
    kpcr->prcb_data.current_thread = kKthread;
    auto* kthread = memory()->TranslateVirtual<X_KTHREAD*>(kKthread);
    kthread->may_queue_apcs = 1;
    for (auto& list : kthread->apc_lists) {
      const uint32_t head = memory()->HostToGuestVirtual(&list);
      list.flink_ptr = head;
      list.blink_ptr = head;
    }
    InstallGuestRoutines();
  }

  void InstallGuestRoutines() {
    REQUIRE(memory()->LookupHeap(kCode)->AllocFixed(
        kCode, 0x10000, 0x10000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    memory()->Zero(kCode, 0x10000);
    for (size_t i = 0; i < xe::countof(kKernelRoutineCode); ++i) {
      Store32(kCode + uint32_t(i) * 4, kKernelRoutineCode[i]);
    }
    for (size_t i = 0; i < xe::countof(kNormalRoutineCode); ++i) {
      Store32(kNormal + uint32_t(i) * 4, kNormalRoutineCode[i]);
    }
    auto module = std::make_unique<cpu::RawModule>(processor());
    module->SetAddressRange(kCode, 0x10000);
    module->set_executable(true);
    processor()->AddModule(std::move(module));
    processor()->backend()->CommitExecutableRange(kCode, kCode + 0x10000);
  }

  // Queues a user-mode APC with a kernel routine, as a title does with
  // KeInitializeApc + KeInsertQueueApc, then delivers it.
  void QueueAndDeliver(uint32_t kapc = kKapc, uint32_t kernel_routine = kCode,
                       uint32_t normal_routine = kQueuedNormal) {
    memory()->Zero(kRecord, 0x100);
    auto* apc = memory()->TranslateVirtual<XAPC*>(kapc);
    xboxkrnl::xeKeInitializeApc(apc, kKthread, kernel_routine, 0,
                                normal_routine,
                                /*apc_mode=*/1, /*normal_context=*/0x1234);
    REQUIRE(xboxkrnl::xeKeInsertQueueApc(apc, 0xABCD, 0xEF01, 0, ctx_) == 1);
    REQUIRE(xboxkrnl::xeProcessUserApcs(ctx_) == X_STATUS_USER_APC);
  }

  uint32_t Record(uint32_t offset) { return Load32(kRecord + offset); }

  // True once the allocation at `base` is released.
  bool RegionFreed(uint32_t base) {
    HeapAllocationInfo info = {};
    REQUIRE(memory()->LookupHeap(base)->QueryRegionInfo(base, &info));
    return !(info.state & kMemoryAllocationCommit);
  }
  void Store32(uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(address), value);
  }
  uint32_t Load32(uint32_t address) {
    return xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(address));
  }

  // What every delivery must show besides the first argument.
  void CheckScratchAndNormalCall() {
    // r4..r7: the four scratch slots on the guest stack.
    const uint32_t scratch = kStackTop - 16;
    CHECK(Record(0x04) == scratch + 0);
    CHECK(Record(0x08) == scratch + 4);
    CHECK(Record(0x0C) == scratch + 8);
    CHECK(Record(0x10) == scratch + 12);
    // ... holding the queued routine, context and arguments, big-endian.
    CHECK(Record(0x40) == kQueuedNormal);
    CHECK(Record(0x44) == 0x1234);
    CHECK(Record(0x48) == 0xABCD);
    CHECK(Record(0x4C) == 0xEF01);
    // The normal routine the kernel routine wrote back ran, once, with the
    // context and arguments it wrote back.
    CHECK(Record(0x2C) == 1);
    CHECK(Record(0x20) == 0x5555);
    CHECK(Record(0x24) == 0x6666);
    CHECK(Record(0x28) == 0x7777);
  }

 protected:
  cpu::ThreadState thread_;
  cpu::ppc::PPCContext* ctx_ = nullptr;
};

}  // namespace

TEST_CASE_METHOD(UserApcTest, "a user APC kernel routine gets the KAPC",
                 "[kernel][apc]") {
  QueueAndDeliver();
  // The KAPC, not its list entry (KAPC + offsetof(XAPC, list_entry)).
  CHECK(Record(0x00) == kKapc);
  CheckScratchAndNormalCall();
}

// An APC queued with no kernel routine of its own (NtQueueApcThread, and so
// every APC the host queues: I/O completions, timers) is freed after
// delivery. Placed so that its list entry starts the next page, freeing the
// list entry (KAPC + 8) instead of the KAPC misses the allocation.
TEST_CASE_METHOD(UserApcTest, "the dummy kernel routine frees the KAPC",
                 "[kernel][apc]") {
  const uint32_t region = memory()->SystemHeapAlloc(0x2000);
  REQUIRE(region);
  REQUIRE((region & 0xFFF) == 0);
  const uint32_t kapc = region + 0x1000 - offsetof(XAPC, list_entry);
  QueueAndDeliver(kapc, XAPC::kDummyKernelRoutine, kNormal);
  CHECK(RegionFreed(region));
  // The normal routine still ran with what was queued.
  CHECK(Record(0x2C) == 1);
  CHECK(Record(0x20) == 0x1234);
  CHECK(Record(0x24) == 0xABCD);
  CHECK(Record(0x28) == 0xEF01);
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
