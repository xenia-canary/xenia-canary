/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The KeInsertQueueApc export's wake: a user APC queued to a thread in an
// alertable wait ends that wait with STATUS_USER_APC, and the APC is
// delivered there. Real XThreads (XHostThread) with guest alertable waits
// (KeDelayExecutionThread(UserMode, Alertable), which is what SleepEx(..,
// TRUE) calls); the queued APC's routines are small PPC routines run through
// the JIT.
//
// Windows only: POSIX AlertableSleep is a plain sleep (threading_posix.cc),
// so nothing there ends a waiter's sleep early.

#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_order.h"
#include "xenia/base/math.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/testing/kernel_state_test.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {
// The guest export, as the trampoline calls it.
dword_result_t KeInsertQueueApc_entry(pointer_t<XAPC> apc, lpvoid_t arg1,
                                      lpvoid_t arg2, dword_t priority_increment,
                                      const ppc_context_t& context);
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace test {
namespace {

constexpr uint32_t kPage = 0x90040000;
constexpr uint32_t kPageSize = 0x10000;
constexpr uint32_t kKapc = kPage + 0x2000;    // XAPC
constexpr uint32_t kRecord = kPage + 0x5000;  // what the normal routine saw

// Guest code: the kernel routine at kCode, the normal routine at kNormal.
constexpr uint32_t kCode = 0x82000000;
constexpr uint32_t kNormal = kCode + 0x100;

// kernel routine: returns, leaving the normal routine, context and
// arguments as queued.
constexpr uint32_t kKernelRoutineCode[] = {
    0x4E800020,  // blr
};
// normal routine(r3 = context, r4 = arg1, r5 = arg2): record them at
// kRecord+0x00..0x08 and 1 at kRecord+0x0C.
constexpr uint32_t kNormalRoutineCode[] = {
    0x3D609004,  // lis   r11, 0x9004
    0x616B5000,  // ori   r11, r11, 0x5000      (kRecord)
    0x906B0000,  // stw   r3, 0(r11)
    0x908B0004,  // stw   r4, 4(r11)
    0x90AB0008,  // stw   r5, 8(r11)
    0x39800001,  // li    r12, 1
    0x918B000C,  // stw   r12, 0xC(r11)
    0x4E800020,  // blr
};

class UserApcWakeTest : public KernelStateTest {
 public:
  UserApcWakeTest() {
    REQUIRE(memory()->LookupHeap(kPage)->AllocFixed(
        kPage, kPageSize, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    memory()->Zero(kPage, kPageSize);
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

  // Initializes the user APC at kKapc for the guest KTHREAD `target`, then
  // queues it through the KeInsertQueueApc export, from `ctx`'s thread.
  uint32_t GuestInsert(cpu::ppc::PPCContext* ctx, uint32_t target) {
    auto* apc = memory()->TranslateVirtual<XAPC*>(kKapc);
    xboxkrnl::xeKeInitializeApc(apc, target, kCode, 0, kNormal,
                                /*apc_mode=*/1, /*normal_context=*/0x1234);
    ctx->r[3] = kKapc;
    ctx->r[4] = 0xABCD;
    ctx->r[5] = 0xEF01;
    ctx->r[6] = 0;
    shim::Param::Init init = {ctx, 0, 0};
    shim::TypedPointerParam<XAPC> apc_param(init);
    shim::PointerParam arg1(init);
    shim::PointerParam arg2(init);
    shim::ParamBase<uint32_t> priority(init);
    shim::ContextParam context(init);
    return xboxkrnl::KeInsertQueueApc_entry(apc_param, arg1, arg2, priority,
                                            context);
  }

  // 1 once the normal routine ran.
  uint32_t Delivered() { return Load32(kRecord + 0x0C); }
  uint32_t Record(uint32_t offset) { return Load32(kRecord + offset); }

  void Store32(uint32_t address, uint32_t value) {
    xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(address), value);
  }
  uint32_t Load32(uint32_t address) {
    return xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(address));
  }
};

// Polls `pred` for up to 10 s.
template <typename Pred>
bool WaitFor(Pred pred) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= end) {
      return pred();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// A guest thread (XHostThread) running `body` with its guest context.
class GuestThread {
 public:
  GuestThread(KernelState* kernel_state,
              std::function<void(cpu::ppc::PPCContext*)> body)
      : body_(std::move(body)) {
    thread_ = object_ref<XHostThread>(new XHostThread(
        kernel_state, 64 * 1024, 0,
        [this]() {
          XThread* self = XThread::GetCurrentThread();
          kthread_ = self->guest_object();
          started_ = true;
          body_(self->thread_state()->context());
          done_ = true;
          return 0;
        },
        kernel_state->GetSystemProcess()));
    REQUIRE(thread_->Create() == X_STATUS_SUCCESS);
    REQUIRE(WaitFor([this] { return started_.load(); }));
  }
  ~GuestThread() {
    if (!WaitFor([this] { return done_.load(); })) {
      std::fprintf(stderr,
                   "FATAL: user_apc_wake_test: a guest thread is stuck\n");
      std::abort();
    }
    xe::threading::Wait(thread_->thread(), false, std::chrono::seconds(10));
  }
  // The thread's guest KTHREAD.
  uint32_t kthread() const { return kthread_; }

 private:
  std::function<void(cpu::ppc::PPCContext*)> body_;
  std::atomic<uint32_t> kthread_{0};
  std::atomic<bool> started_{false};
  std::atomic<bool> done_{false};
  object_ref<XHostThread> thread_;
};

// A guest alertable wait of `ms`; returns how long it took.
std::chrono::milliseconds GuestAlertableWait(cpu::ppc::PPCContext* ctx,
                                             int ms) {
  uint64_t interval = static_cast<uint64_t>(-int64_t(ms) * 10'000);
  const auto t0 = std::chrono::steady_clock::now();
  xboxkrnl::KeDelayExecutionThread(/*processor_mode=*/1, /*alertable=*/1,
                                   &interval, ctx);
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);
}

}  // namespace

// One thread in a 2 s alertable wait; another queues it a user APC through
// the export 100 ms in. Without the wake the wait runs out (and the APC is
// delivered only at the thread's next alertable wait).
TEST_CASE_METHOD(UserApcWakeTest,
                 "a queued user APC ends the target's alertable wait",
                 "[kernel][apc]") {
  std::atomic<int64_t> waited_ms{-1};
  std::atomic<uint32_t> delivered{0xFFFFFFFF};
  std::atomic<uint32_t> inserted{0};
  {
    GuestThread waiter(kernel_state(), [&](cpu::ppc::PPCContext* ctx) {
      waited_ms = GuestAlertableWait(ctx, 2000).count();
      delivered = Delivered();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint32_t target = waiter.kthread();
    GuestThread inserter(kernel_state(), [&](cpu::ppc::PPCContext* ctx) {
      inserted = GuestInsert(ctx, target);
    });
  }
  REQUIRE(inserted.load() == 1);
  // Woken 100 ms into its 2 s wait, with the APC delivered there.
  CHECK(waited_ms.load() >= 0);
  CHECK(waited_ms.load() < 1000);
  CHECK(delivered.load() == 1);
  CHECK(Record(0x00) == 0x1234);
  CHECK(Record(0x04) == 0xABCD);
  CHECK(Record(0x08) == 0xEF01);
}

// The current thread is not woken: its next alertable wait delivers the APC
// it queued itself. A self-wake would leave a host APC queued, which would
// end the following alertable wait at once.
TEST_CASE_METHOD(UserApcWakeTest,
                 "a thread queuing itself a user APC is not woken",
                 "[kernel][apc]") {
  std::atomic<uint32_t> inserted{0};
  std::atomic<int64_t> second_ms{-1};
  {
    GuestThread self(kernel_state(), [&](cpu::ppc::PPCContext* ctx) {
      inserted = GuestInsert(ctx, XThread::GetCurrentThread()->guest_object());
      GuestAlertableWait(ctx, 300);  // delivers the APC and returns
      second_ms = GuestAlertableWait(ctx, 300).count();
    });
  }
  REQUIRE(inserted.load() == 1);
  CHECK(Delivered() == 1);
  CHECK(second_ms.load() >= 250);
}

}  // namespace test
}  // namespace kernel
}  // namespace xe

#endif  // XE_PLATFORM_WIN32
