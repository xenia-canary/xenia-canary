/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_backend.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>

#if XE_PLATFORM_MAC
#include <pthread.h>
#endif

#if XE_PLATFORM_MAC || XE_PLATFORM_LINUX || XE_PLATFORM_IOS
#include <dlfcn.h>
#endif

#include "xenia/base/clock.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#if XE_ARCH_ARM64 && XE_COMPILER_MSVC
#include <intrin.h>
#endif
#include "xenia/cpu/backend/a64/a64_assembler.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_emitter.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

DECLARE_bool(record_mmio_access_exceptions);

DEFINE_int64(a64_max_stackpoints, 65536,
             "Max number of host->guest stack mappings we can record.", "a64");

DEFINE_bool(a64_enable_host_guest_stack_synchronization, true,
            "Records entries for guest/host stack mappings at function starts "
            "and checks for reentry at return sites. Has slight performance "
            "impact, but fixes crashes in games that use setjmp/longjmp.",
            "a64");
DEFINE_bool(a64_resolve_function_log, false,
            "Log A64 ResolveFunction failures with module ranges.", "a64");
DEFINE_int32(a64_resolve_function_log_limit, 8,
             "Maximum ResolveFunction failure logs.", "a64");
DEFINE_bool(a64_stack_sync_log, false, "Log A64 stack sync events.", "a64");
DEFINE_int32(a64_stack_sync_log_limit, 16, "Maximum A64 stack sync logs.",
             "a64");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

namespace {

bool ShouldLogResolveFailure() {
  if (!cvars::a64_resolve_function_log) {
    return false;
  }
  const int32_t limit = cvars::a64_resolve_function_log_limit;
  if (limit <= 0) {
    return false;
  }
  static std::atomic<int32_t> log_count{0};
  const int32_t count = log_count.fetch_add(1, std::memory_order_relaxed);
  return count < limit;
}

}  // namespace

// Resolve a guest function at runtime. Called by the resolve thunk when
// a guest address has not yet been compiled.
uint64_t ResolveFunction(void* raw_context, uint64_t target_address,
                         uint64_t host_return_address);

void ForwardMMIOAccessForRecording(void* context, void* hostaddr) {
  reinterpret_cast<A64Backend*>(context)
      ->RecordMMIOExceptionForGuestInstruction(hostaddr);
}

// ==========================================================================
// A64HelperEmitter — generates thunks using xbyak_aarch64.
// ==========================================================================
class A64HelperEmitter : public A64Emitter {
 public:
  A64HelperEmitter(A64Backend* backend, XbyakA64Allocator* allocator);

  HostToGuestThunk EmitHostToGuestThunk(size_t* code_size_out = nullptr);
  GuestToHostThunk EmitGuestToHostThunk(size_t* code_size_out = nullptr);
  ResolveFunctionThunk EmitResolveFunctionThunk(size_t* code_size_out = nullptr);
  void* EmitGuestAndHostSynchronizeStackHelper(
      size_t* code_size_out = nullptr);
};

A64HelperEmitter::A64HelperEmitter(A64Backend* backend,
                                   XbyakA64Allocator* allocator)
    : A64Emitter(backend, allocator) {}

// --------------------------------------------------------------------------
// HostToGuestThunk
// --------------------------------------------------------------------------
// Called from host C++ code to enter JIT'd guest code.
//   x0 = target machine code address
//   x1 = PPCContext* (arg0)
//   x2 = return address value (arg1)
//
// ARM64 AAPCS64 calling convention:
//   Caller-saved: x0-x18, v0-v7, v16-v31
//   Callee-saved: x19-x28, x29(FP), x30(LR), d8-d15
//
// We save all callee-saved regs, set up context (x20) and membase (x21),
// then call the target. On return, restore and return to host.
HostToGuestThunk A64HelperEmitter::EmitHostToGuestThunk(size_t* code_size_out) {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  // Allocate thunk stack frame.
  // Save x29(FP) and x30(LR) first, then callee-saved GPRs and NEON regs.
  const size_t thunk_stack = StackLayout::THUNK_STACK_SIZE;

  // sub sp, sp, #thunk_stack
  sub(sp, sp, static_cast<uint32_t>(thunk_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save callee-saved GPRs: x19-x28, x29, x30
  stp(x19, x20, ptr(sp, 0x00));
  stp(x21, x22, ptr(sp, 0x10));
  stp(x23, x24, ptr(sp, 0x20));
  stp(x25, x26, ptr(sp, 0x30));
  stp(x27, x28, ptr(sp, 0x40));
  stp(x29, x30, ptr(sp, 0x50));

  // Save callee-saved NEON regs: d8-d15
  stp(d8, d9, ptr(sp, 0x60));
  stp(d10, d11, ptr(sp, 0x70));
  stp(d12, d13, ptr(sp, 0x80));
  stp(d14, d15, ptr(sp, 0x90));

  code_offsets.body = getSize();

  // Set up guest execution state.
  // x20 = context (PPCContext*)
  mov(x20, x1);
  // x21 = virtual_membase (loaded from context)
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
  // Restore the guest scalar FPCR on every host->guest entry so host-side
  // work done before the call can't leak a stale rounding / non-IEEE mode.
  sub(x10, x20, static_cast<uint32_t>(sizeof(A64BackendContext)));
  ldr(w11,
      ptr(x10, static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu))));
  msr(3, 3, 4, 4, 0, x11);
  // x0 still holds target, x2 holds return address.
  // The guest function's prolog stores x0 to GUEST_RET_ADDR on its stack
  // frame. Move the target to a scratch reg and put the guest return
  // address into x0.
  mov(x9, x0);  // x9 = target (scratch reg)
  // Pass guest return address in x0 (convention for guest function entry).
  mov(x0, x2);  // x0 = guest return address

  // Call the guest function.
  blr(x9);

  code_offsets.epilog = getSize();

  // Restore callee-saved NEON regs.
  ldp(d14, d15, ptr(sp, 0x90));
  ldp(d12, d13, ptr(sp, 0x80));
  ldp(d10, d11, ptr(sp, 0x70));
  ldp(d8, d9, ptr(sp, 0x60));

  // Restore callee-saved GPRs.
  ldp(x29, x30, ptr(sp, 0x50));
  ldp(x27, x28, ptr(sp, 0x40));
  ldp(x25, x26, ptr(sp, 0x30));
  ldp(x23, x24, ptr(sp, 0x20));
  ldp(x21, x22, ptr(sp, 0x10));
  ldp(x19, x20, ptr(sp, 0x00));

  // Deallocate stack.
  add(sp, sp, static_cast<uint32_t>(thunk_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = thunk_stack;

  if (code_size_out) {
    *code_size_out = func_info.code_size.total;
  }
  void* fn = Emplace(func_info);
  return reinterpret_cast<HostToGuestThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestToHostThunk
// --------------------------------------------------------------------------
// Called from guest JIT code to transition into a host (C++) function.
//   x0 = target host function
//   x1 = arg0
//   x2 = arg1
//
// We save volatile guest registers that we need to preserve across the
// host call, then call the host function with context as the first arg.
GuestToHostThunk A64HelperEmitter::EmitGuestToHostThunk(size_t* code_size_out) {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  // The guest JIT uses v4-v7 and v16-v31 as allocatable VEC regs.
  // These are caller-saved in AAPCS64, so the host C function WILL clobber
  // them. We must save all guest-allocated VEC regs (full 128-bit Q regs).
  // GPRs x19-x28 are callee-saved in AAPCS64, so the C function preserves them.
  //
  // Stack layout:
  //   q4, q5       sp + 0x000  (32 bytes)
  //   q6, q7       sp + 0x020
  //   q16, q17     sp + 0x040
  //   q18, q19     sp + 0x060
  //   q20, q21     sp + 0x080
  //   q22, q23     sp + 0x0A0
  //   q24, q25     sp + 0x0C0
  //   q26, q27     sp + 0x0E0
  //   q28, q29     sp + 0x100
  //   q30, q31     sp + 0x120
  //   x29, x30     sp + 0x140
  //   x20, x21     sp + 0x150
  //   Total: 0x160 = 352 bytes (16-byte aligned)
  const size_t g2h_stack = 352;
  sub(sp, sp, static_cast<uint32_t>(g2h_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save guest-allocated VEC regs (full Q = 128-bit).
  stp(Xbyak_aarch64::QReg(4), Xbyak_aarch64::QReg(5), ptr(sp, 0x000));
  stp(Xbyak_aarch64::QReg(6), Xbyak_aarch64::QReg(7), ptr(sp, 0x020));
  stp(Xbyak_aarch64::QReg(16), Xbyak_aarch64::QReg(17), ptr(sp, 0x040));
  stp(Xbyak_aarch64::QReg(18), Xbyak_aarch64::QReg(19), ptr(sp, 0x060));
  stp(Xbyak_aarch64::QReg(20), Xbyak_aarch64::QReg(21), ptr(sp, 0x080));
  stp(Xbyak_aarch64::QReg(22), Xbyak_aarch64::QReg(23), ptr(sp, 0x0A0));
  stp(Xbyak_aarch64::QReg(24), Xbyak_aarch64::QReg(25), ptr(sp, 0x0C0));
  stp(Xbyak_aarch64::QReg(26), Xbyak_aarch64::QReg(27), ptr(sp, 0x0E0));
  stp(Xbyak_aarch64::QReg(28), Xbyak_aarch64::QReg(29), ptr(sp, 0x100));
  stp(Xbyak_aarch64::QReg(30), Xbyak_aarch64::QReg(31), ptr(sp, 0x120));
  // Save x29/x30 (FP/LR).
  stp(x29, x30, ptr(sp, 0x140));
  // Some host callbacks don't preserve our reserved guest-state registers.
  stp(x20, x21, ptr(sp, 0x150));

  code_offsets.body = getSize();

  // Call host function.
  // AAPCS64: x0=first arg. We set x0=context (from x20).
  mov(x9, x0);   // x9 = target function (scratch)
  mov(x0, x20);  // x0 = PPCContext* (our context reg)
  // x1, x2, x3 already hold args from the caller.
  blr(x9);

  // Restore the reserved guest-state registers explicitly rather than relying
  // on every host callback to honor AAPCS64 callee-save rules.
  ldp(x20, x21, ptr(sp, 0x150));
  // Reload membase in case the host callback clobbered x21.
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));
  // Host callbacks may change FPCR. Restore the guest scalar FPCR before
  // resuming the JIT so later guest ops observe the cached PPC mode.
  sub(x10, x20, static_cast<uint32_t>(sizeof(A64BackendContext)));
  ldr(w11,
      ptr(x10, static_cast<uint32_t>(offsetof(A64BackendContext, fpcr_fpu))));
  msr(3, 3, 4, 4, 0, x11);

  code_offsets.epilog = getSize();

  // Restore.
  ldp(x29, x30, ptr(sp, 0x140));
  ldp(Xbyak_aarch64::QReg(30), Xbyak_aarch64::QReg(31), ptr(sp, 0x120));
  ldp(Xbyak_aarch64::QReg(28), Xbyak_aarch64::QReg(29), ptr(sp, 0x100));
  ldp(Xbyak_aarch64::QReg(26), Xbyak_aarch64::QReg(27), ptr(sp, 0x0E0));
  ldp(Xbyak_aarch64::QReg(24), Xbyak_aarch64::QReg(25), ptr(sp, 0x0C0));
  ldp(Xbyak_aarch64::QReg(22), Xbyak_aarch64::QReg(23), ptr(sp, 0x0A0));
  ldp(Xbyak_aarch64::QReg(20), Xbyak_aarch64::QReg(21), ptr(sp, 0x080));
  ldp(Xbyak_aarch64::QReg(18), Xbyak_aarch64::QReg(19), ptr(sp, 0x060));
  ldp(Xbyak_aarch64::QReg(16), Xbyak_aarch64::QReg(17), ptr(sp, 0x040));
  ldp(Xbyak_aarch64::QReg(6), Xbyak_aarch64::QReg(7), ptr(sp, 0x020));
  ldp(Xbyak_aarch64::QReg(4), Xbyak_aarch64::QReg(5), ptr(sp, 0x000));

  add(sp, sp, static_cast<uint32_t>(g2h_stack));
  ret();

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = g2h_stack;

  if (code_size_out) {
    *code_size_out = func_info.code_size.total;
  }
  void* fn = Emplace(func_info);
  return reinterpret_cast<GuestToHostThunk>(fn);
}

// --------------------------------------------------------------------------
// ResolveFunctionThunk
// --------------------------------------------------------------------------
// Called when guest code calls an unresolved function address.
// The indirection table initially points all entries here.
// We call ResolveFunction to compile/lookup the target, then jump to it.
//
// On entry from the indirection table:
//   w17 = guest PPC address (preserved by the call sequence)
//   x20 = context
//   x30 = return address (from the BLR that got us here)
ResolveFunctionThunk A64HelperEmitter::EmitResolveFunctionThunk(
    size_t* code_size_out) {
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();

  const size_t thunk_stack = StackLayout::THUNK_STACK_SIZE;
  sub(sp, sp, static_cast<uint32_t>(thunk_stack));
  code_offsets.prolog_stack_alloc = getSize();

  // Save x29/x30 and x0 (guest return address, needed by the resolved
  // function's prolog). x19 is callee-saved so it survives the C call.
  stp(x29, x30, ptr(sp, 0x50));
  stp(x0, x19, ptr(sp, 0x00));  // save x0 (guest ret addr) and x19

  code_offsets.body = getSize();

  // Call ResolveFunction(context, target_address, host_return_address).
  stp(x20, x21, ptr(sp, 0x10));
  mov(x0, x20);  // x0 = PPCContext*
  mov(w1, w17);  // x1 = guest address (32-bit in w17)
  mov(x2, x30);  // x2 = host return address after the call site
  // Load address of ResolveFunction.
  mov(x9, reinterpret_cast<uint64_t>(&ResolveFunction));
  blr(x9);
  // x0 now holds the resolved host machine code address.
  mov(x9, x0);
  ldp(x20, x21, ptr(sp, 0x10));
  // Reload membase in case ResolveFunction clobbered x21.
  ldr(x21, ptr(x20, static_cast<int32_t>(
                        offsetof(ppc::PPCContext, virtual_membase))));

  code_offsets.epilog = getSize();

  // Restore x0 (guest return address) and saved regs.
  ldp(x0, x19, ptr(sp, 0x00));
  ldp(x29, x30, ptr(sp, 0x50));
  add(sp, sp, static_cast<uint32_t>(thunk_stack));

  cbz(x9, 8);  // skip br x9 if null, fall through to brk
  br(x9);      // Jump to the resolved function (tail call — preserves LR).
  brk(0xF0);   // Resolution failed — trap for debugging.

  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = thunk_stack;

  if (code_size_out) {
    *code_size_out = func_info.code_size.total;
  }
  void* fn = Emplace(func_info);
  return reinterpret_cast<ResolveFunctionThunk>(fn);
}

// --------------------------------------------------------------------------
// GuestAndHostSynchronizeStackHelper
// --------------------------------------------------------------------------
// Called when longjmp is detected (guest r1 changed after a call returned).
// Walks the stackpoint array backward to find the matching host SP, restores
// it, and jumps back to the caller.
//
// On entry (set by the tail-emitted sync check in the guest function):
//   x8  = return address (where to jump after fixup)
//   x9  = caller's stack size (to subtract from restored SP)
//   x20 = PPCContext*
//   The backend context is at (x20 - sizeof(A64BackendContext)).
void* A64HelperEmitter::EmitGuestAndHostSynchronizeStackHelper(
    size_t* code_size_out) {
  using namespace Xbyak_aarch64;
  struct {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = getSize();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // Load backend context pointer: x17 = x20 - sizeof(A64BackendContext)
  mov(x17, static_cast<uint64_t>(sizeof(A64BackendContext)));
  sub(x17, x20, x17);

  // x10 = stackpoints array pointer
  ldr(x10, ptr(x17, static_cast<uint32_t>(
                        offsetof(A64BackendContext, stackpoints))));
  // w11 = current_stackpoint_depth
  ldr(w11, ptr(x17, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                   current_stackpoint_depth))));

  // w12 = current guest r1
  ldr(w12, ptr(x20, static_cast<int32_t>(offsetof(ppc::PPCContext, r[1]))));

  // Search backward through stackpoints for the first entry where
  // guest_stack_ >= current r1 (guest stack was unwound past that frame).
  // ecx = loop index, starting at depth - 1
  sub(w13, w11, 1);

  auto& loop = NewCachedLabel();
  auto& found = NewCachedLabel();
  auto& underflow = NewCachedLabel();

  L(loop);
  // Bounds check
  tbnz(w13, 31, underflow);  // if index went negative, bail

  // x14 = &stackpoints[w13] = x10 + w13 * sizeof(A64BackendStackpoint)
  mov(w14, static_cast<uint32_t>(sizeof(A64BackendStackpoint)));
  umull(x14, w13, w14);
  add(x14, x10, x14);

  // w15 = stackpoints[index].guest_stack_
  ldr(w15, ptr(x14, static_cast<uint32_t>(
                        offsetof(A64BackendStackpoint, guest_stack_))));

  // If guest_stack_ >= current r1, we found our target frame.
  cmp(w15, w12);
  b(GE, found);

  // Not found yet, go to previous entry.
  sub(w13, w13, 1);
  b(loop);

  L(found);
  // x14 points to the matching stackpoint entry.
  // Restore host SP from stackpoints[index].host_stack_
  ldr(x16, ptr(x14, static_cast<uint32_t>(
                        offsetof(A64BackendStackpoint, host_stack_))));

  // Adjust for the caller's stack frame: SP = host_stack_ - stack_size
  sub(x16, x16, x9);
  mov(sp, x16);

  // Update current_stackpoint_depth = index + 1
  // (the entry we restored to has been consumed)
  add(w13, w13, 1);
  str(w13, ptr(x17, static_cast<uint32_t>(offsetof(A64BackendContext,
                                                   current_stackpoint_depth))));

  // Jump back to the caller.
  br(x8);

  L(underflow);
  // Should be impossible — stackpoint array underflowed.
  brk(0xF1);

  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = 0;

  if (code_size_out) {
    *code_size_out = func_info.code_size.total;
  }
  return Emplace(func_info);
}

// ==========================================================================
// ResolveFunction — runtime function resolution.
// ==========================================================================
uint64_t ResolveFunction(void* raw_context, uint64_t target_address,
                         uint64_t host_return_address) {
  auto guest_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  assert_not_null(guest_context);
  auto thread_state = guest_context->thread_state;
  assert_not_null(thread_state);
  assert_not_zero(target_address);

  uint32_t guest_address = 0;
  if (target_address > 0xFFFFFFFFu) {
    const auto ctx_ptr = reinterpret_cast<uint64_t>(guest_context);
    if (target_address >= ctx_ptr &&
        target_address < ctx_ptr + sizeof(ppc::PPCContext)) {
      XELOGE(
          "ResolveFunction: target_address 0x{:016X} is within PPCContext "
          "[0x{:016X}, 0x{:016X})",
          target_address, ctx_ptr, ctx_ptr + sizeof(ppc::PPCContext));
      XELOGE(
          "ResolveFunction: The target register contains a context pointer "
          "instead of a function address");
      return 0;
    }

    auto* code_cache = static_cast<A64CodeCache*>(
        thread_state->processor()->backend()->code_cache());
    auto* guest_function = code_cache->LookupFunction(target_address);
    if (guest_function) {
      guest_address =
          guest_function->MapMachineCodeToGuestAddress(target_address);
    } else {
      guest_address = static_cast<uint32_t>(target_address);
    }
  } else {
    guest_address = static_cast<uint32_t>(target_address);
  }

  if (!guest_address) {
    XELOGE("ResolveFunction: guest_address is 0");
    return 0;
  }

  if (cvars::a64_enable_host_guest_stack_synchronization &&
      target_address <= 0xFFFFFFFFu) {
    auto* processor = thread_state->processor();
    auto* module_for_address =
        processor->LookupModule(static_cast<uint32_t>(target_address));
    if (module_for_address) {
      auto* xexmod = dynamic_cast<XexModule*>(module_for_address);
      if (xexmod) {
        auto* flags = xexmod->GetInstructionAddressFlags(
            static_cast<uint32_t>(target_address));
        if (flags && flags->is_return_site) {
          auto ones_with_address = processor->FindFunctionsWithAddress(
              static_cast<uint32_t>(target_address));
          if (!ones_with_address.empty()) {
            A64Function* candidate = nullptr;
            uintptr_t host_address = 0;
            for (auto* entry : ones_with_address) {
              auto* afunc = static_cast<A64Function*>(entry);
              host_address = afunc->MapGuestAddressToMachineCode(
                  static_cast<uint32_t>(target_address));
              if (host_address &&
                  afunc->machine_code() !=
                      reinterpret_cast<const uint8_t*>(host_address)) {
                candidate = afunc;
                break;
              }
            }

            if (candidate && host_address) {
              auto* backend = static_cast<A64Backend*>(processor->backend());
              auto* backend_context =
                  backend->BackendContextForGuestContext(guest_context);
              if (backend_context->stackpoints &&
                  backend_context->current_stackpoint_depth > 0) {
                uint32_t current_stackpoint_index =
                    backend_context->current_stackpoint_depth - 1;
                const uint32_t current_guest_stackpointer =
                    static_cast<uint32_t>(guest_context->r[1]);
                uint32_t num_frames_bigger = 0;

                while (current_stackpoint_index != 0xFFFFFFFFu) {
                  if (current_guest_stackpointer >
                      backend_context->stackpoints[current_stackpoint_index]
                          .guest_stack_) {
                    --current_stackpoint_index;
                    ++num_frames_bigger;
                  } else {
                    break;
                  }
                }

                if (num_frames_bigger > 1 &&
                    current_stackpoint_index != 0xFFFFFFFFu) {
                  const uint32_t guest_lr =
                      static_cast<uint32_t>(guest_context->lr);
                  uint32_t scan_index = current_stackpoint_index;
                  while (scan_index != 0xFFFFFFFFu) {
                    const auto& sp_entry =
                        backend_context->stackpoints[scan_index];
                    if (sp_entry.guest_stack_ != current_guest_stackpointer) {
                      break;
                    }
                    if (sp_entry.guest_return_address_ == guest_lr) {
                      current_stackpoint_index = scan_index;
                      break;
                    }
                    if (!scan_index) {
                      break;
                    }
                    --scan_index;
                  }

                  if (cvars::a64_stack_sync_log) {
                    static std::atomic<int32_t> sync_log_count{0};
                    const int32_t limit = cvars::a64_stack_sync_log_limit;
                    const int32_t count = sync_log_count.fetch_add(
                        1, std::memory_order_relaxed);
                    if (limit <= 0 || count < limit) {
                      XELOGI(
                          "A64 stack sync: guest=0x{:08X} host=0x{:016X} "
                          "guest_sp=0x{:08X} depth={} index={}",
                          static_cast<uint32_t>(target_address), host_address,
                          current_guest_stackpointer,
                          backend_context->current_stackpoint_depth,
                          current_stackpoint_index);
                    }
                  }
                  return host_address;
                }
              }
            }
          }
        }
      }
    }
  }

  auto fn = thread_state->processor()->ResolveFunction(guest_address);
  if (!fn) {
    XELOGE("ResolveFunction: Failed to resolve guest address 0x{:08X} "
           "(target=0x{:016X}, lr=0x{:016X}, ctr=0x{:016X}, "
           "host_return=0x{:016X}, thread={})",
           guest_address, target_address, guest_context->lr,
           guest_context->ctr, host_return_address, guest_context->thread_id);
    if (ShouldLogResolveFailure()) {
      auto* processor = thread_state->processor();
      auto* code_cache = static_cast<A64CodeCache*>(processor->backend()->code_cache());
      auto log_modules_for_address = [&](uint32_t address, const char* label) {
        bool found = false;
        for (auto* module : processor->GetModules()) {
          if (!module) {
            continue;
          }
          if (module->ContainsAddress(address)) {
            XELOGI("ResolveFunction: {} module '{}' contains 0x{:08X}", label,
                   module->name(), address);
            found = true;
          }
        }
        if (!found) {
          XELOGI("ResolveFunction: {} no module contains 0x{:08X}", label,
                 address);
        }
      };
      log_modules_for_address(static_cast<uint32_t>(guest_context->lr), "lr");
      log_modules_for_address(guest_address, "guest");

      auto lr_functions = processor->FindFunctionsWithAddress(
          static_cast<uint32_t>(guest_context->lr));
      if (lr_functions.empty()) {
        XELOGI("ResolveFunction: no resolved function covers LR 0x{:08X}",
               static_cast<uint32_t>(guest_context->lr));
      } else {
        const auto* lr_fn = lr_functions.front();
        XELOGI("ResolveFunction: LR function {} [0x{:08X},0x{:08X}) name='{}'",
               lr_functions.size(), lr_fn->address(), lr_fn->end_address(),
               lr_fn->name());
      }

      if (host_return_address) {
        auto* host_fn = code_cache->LookupFunction(host_return_address);
        if (!host_fn || !host_fn->machine_code()) {
          XELOGI("ResolveFunction: no compiled guest function covers "
                 "host_return=0x{:016X}",
                 host_return_address);
        } else {
          const uint32_t host_resume_guest =
              host_fn->MapMachineCodeToGuestAddress(host_return_address);
          const uint32_t host_call_guest =
              host_resume_guest >= 4 ? host_resume_guest - 4 : host_resume_guest;
          XELOGI(
              "ResolveFunction: host_return=0x{:016X} maps to function "
              "[0x{:08X},0x{:08X}) '{}' resume=0x{:08X} callsite=0x{:08X}",
              host_return_address, host_fn->address(), host_fn->end_address(),
              host_fn->name(), host_resume_guest, host_call_guest);
        }
      }

      auto* memory = thread_state->memory();
      if (!memory) {
        XELOGI("ResolveFunction: no Memory available for guest dump");
      } else if (!memory->LookupHeap(guest_address)) {
        XELOGI("ResolveFunction: guest_address 0x{:08X} not in any heap",
               guest_address);
      } else {
        const uint8_t* data =
            memory->TranslateVirtual<const uint8_t*>(guest_address);
        std::array<uint8_t, 16> bytes = {};
        std::memcpy(bytes.data(), data, bytes.size());
        XELOGI(
            "ResolveFunction: guest[0x{:08X}] = {:02X} {:02X} {:02X} {:02X} "
            "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
            "{:02X} {:02X} {:02X} {:02X}",
            guest_address, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
            bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
            bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

        const uint32_t lr = static_cast<uint32_t>(guest_context->lr);
        if (memory->LookupHeap(lr)) {
          XELOGI(
              "ResolveFunction: GPRs "
              "r0=0x{:08X} r1=0x{:08X} r2=0x{:08X} r3=0x{:08X} "
              "r4=0x{:08X} r5=0x{:08X} r6=0x{:08X} r7=0x{:08X}",
              static_cast<uint32_t>(guest_context->r[0]),
              static_cast<uint32_t>(guest_context->r[1]),
              static_cast<uint32_t>(guest_context->r[2]),
              static_cast<uint32_t>(guest_context->r[3]),
              static_cast<uint32_t>(guest_context->r[4]),
              static_cast<uint32_t>(guest_context->r[5]),
              static_cast<uint32_t>(guest_context->r[6]),
              static_cast<uint32_t>(guest_context->r[7]));
          XELOGI(
              "ResolveFunction: GPRs "
              "r8=0x{:08X} r9=0x{:08X} r10=0x{:08X} r11=0x{:08X} "
              "r12=0x{:08X} r13=0x{:08X} r31=0x{:08X}",
              static_cast<uint32_t>(guest_context->r[8]),
              static_cast<uint32_t>(guest_context->r[9]),
              static_cast<uint32_t>(guest_context->r[10]),
              static_cast<uint32_t>(guest_context->r[11]),
              static_cast<uint32_t>(guest_context->r[12]),
              static_cast<uint32_t>(guest_context->r[13]),
              static_cast<uint32_t>(guest_context->r[31]));

          for (int offset = -4; offset <= 4; ++offset) {
            const uint32_t pc = lr + offset * 4;
            if (!memory->LookupHeap(pc)) {
              continue;
            }
            const uint32_t* instruction_ptr =
                memory->TranslateVirtual<const uint32_t*>(pc);
            if (!instruction_ptr) {
              continue;
            }
            const uint32_t instruction = xe::load_and_swap<uint32_t>(
                const_cast<uint32_t*>(instruction_ptr));
            xe::StringBuffer disasm;
            if (cpu::ppc::DisasmPPC(pc, instruction, &disasm)) {
              XELOGI("ResolveFunction: lr_window{} 0x{:08X} {}",
                     offset == 0 ? "*" : " ", instruction,
                     disasm.to_string());
            } else {
              XELOGI("ResolveFunction: lr_window{} 0x{:08X}",
                     offset == 0 ? "*" : " ", instruction);
            }
          }

          if (host_return_address) {
            auto* host_fn = code_cache->LookupFunction(host_return_address);
            if (host_fn && host_fn->machine_code()) {
              const uint32_t host_resume_guest =
                  host_fn->MapMachineCodeToGuestAddress(host_return_address);
              const uint32_t host_call_guest =
                  host_resume_guest >= 4 ? host_resume_guest - 4
                                         : host_resume_guest;
              for (int offset = -4; offset <= 8; ++offset) {
                const uint32_t pc = host_call_guest + offset * 4;
                if (!memory->LookupHeap(pc)) {
                  continue;
                }
                const uint32_t* instruction_ptr =
                    memory->TranslateVirtual<const uint32_t*>(pc);
                if (!instruction_ptr) {
                  continue;
                }
                const uint32_t instruction = xe::load_and_swap<uint32_t>(
                    const_cast<uint32_t*>(instruction_ptr));
                xe::StringBuffer disasm;
                if (cpu::ppc::DisasmPPC(pc, instruction, &disasm)) {
                  XELOGI("ResolveFunction: callsite_window{} 0x{:08X} {}",
                         offset == 0 ? "*" : " ", instruction,
                         disasm.to_string());
                } else {
                  XELOGI("ResolveFunction: callsite_window{} 0x{:08X}",
                         offset == 0 ? "*" : " ", instruction);
                }
              }
            }
          }

          const uint32_t r31 = static_cast<uint32_t>(guest_context->r[31]);
          if (memory->LookupHeap(r31)) {
            XELOGI(
                "ResolveFunction: stack32 "
                "[r31+0x120]=0x{:08X} [r31+0x124]=0x{:08X} "
                "[r31+0x128]=0x{:08X} [r31+0x12C]=0x{:08X}",
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x120)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x124)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x128)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x12C)));
            XELOGI(
                "ResolveFunction: stack32 "
                "[r31+0x130]=0x{:08X} [r31+0x134]=0x{:08X} "
                "[r31+0x138]=0x{:08X} [r31+0x13C]=0x{:08X}",
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x130)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x134)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x138)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x13C)));
            XELOGI(
                "ResolveFunction: stack32 "
                "[r31+0x140]=0x{:08X} [r31+0x144]=0x{:08X} "
                "[r31+0x148]=0x{:08X} [r31+0x14C]=0x{:08X}",
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x140)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x144)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x148)),
                xe::load_and_swap<uint32_t>(
                    memory->TranslateVirtual<uint32_t*>(r31 + 0x14C)));
          }
        } else {
          XELOGI("ResolveFunction: LR 0x{:08X} not in any heap", lr);
        }
      }
    }
    return 0;
  }

  auto* guest_fn = static_cast<A64Function*>(fn);
  auto code = guest_fn->machine_code();
  if (!code) {
    XELOGE(
        "ResolveFunction: Function at guest address 0x{:08X} has no machine "
        "code",
        guest_address);
    return 0;
  }
  return reinterpret_cast<uint64_t>(code);
}

// ==========================================================================
// A64Backend
// ==========================================================================

// ARM64 guest trampoline template.
// Loads proc, userdata1, userdata2 into x0-x2, then jumps to guest_to_host
// thunk via x9.  Each 64-bit immediate uses movz + 3x movk (16 bytes).
// Total: 4 registers × 16 bytes + 4 bytes (br x9) = 68 bytes.
//
// Template layout (offsets where 64-bit immediates are patched):
//   +0x00: movz x0, #imm16; movk x0, ..., lsl 16/32/48  -> proc
//   +0x10: movz x1, #imm16; movk x1, ..., lsl 16/32/48  -> userdata1
//   +0x20: movz x2, #imm16; movk x2, ..., lsl 16/32/48  -> userdata2
//   +0x30: movz x9, #imm16; movk x9, ..., lsl 16/32/48  -> g2h thunk
//   +0x40: br x9
//
// ARM64 encoding helpers:
//   movz xN, #imm16          = 0xD2800000 | (imm16 << 5) | N
//   movk xN, #imm16, lsl #S  = 0xF2800000 | (hw << 21) | (imm16 << 5) | N
//     where hw = S/16 (0,1,2,3)
static void EncodeMovImm64(uint32_t* out, uint32_t reg, uint64_t imm) {
  out[0] = 0xD2800000 | (static_cast<uint32_t>(imm & 0xFFFF) << 5) | reg;
  out[1] =
      0xF2A00000 | (static_cast<uint32_t>((imm >> 16) & 0xFFFF) << 5) | reg;
  out[2] =
      0xF2C00000 | (static_cast<uint32_t>((imm >> 32) & 0xFFFF) << 5) | reg;
  out[3] =
      0xF2E00000 | (static_cast<uint32_t>((imm >> 48) & 0xFFFF) << 5) | reg;
}

static constexpr size_t kGuestTrampolineSize = 68;  // 17 instructions × 4
static constexpr uint32_t kTrampolineOffsetProc = 0x00;
static constexpr uint32_t kTrampolineOffsetArg1 = 0x10;
static constexpr uint32_t kTrampolineOffsetArg2 = 0x20;
static constexpr uint32_t kTrampolineOffsetThunk = 0x30;

static void BuildGuestTrampoline(uint8_t* buf, void* proc, void* userdata1,
                                 void* userdata2, void* g2h_thunk) {
  auto* code = reinterpret_cast<uint32_t*>(buf);
  // x0 = proc (target function for guest-to-host thunk)
  EncodeMovImm64(&code[0], 0, reinterpret_cast<uint64_t>(proc));
  // x1 = userdata1
  EncodeMovImm64(&code[4], 1, reinterpret_cast<uint64_t>(userdata1));
  // x2 = userdata2
  EncodeMovImm64(&code[8], 2, reinterpret_cast<uint64_t>(userdata2));
  // x9 = guest_to_host_thunk
  EncodeMovImm64(&code[12], 9, reinterpret_cast<uint64_t>(g2h_thunk));
  // br x9
  code[16] = 0xD61F0120;  // br x9
}

A64Backend::A64Backend() {
  code_cache_ = A64CodeCache::Create();

  // Allocate executable memory for guest trampolines.
  void* buf =
      memory::AllocFixed(nullptr, kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
                         xe::memory::AllocationType::kReserveCommit,
                         xe::memory::PageAccess::kExecuteReadWrite);
  xenia_assert(buf);
  guest_trampoline_memory_ = reinterpret_cast<uint8_t*>(buf);
  guest_trampoline_address_bitmap_.Resize(MAX_GUEST_TRAMPOLINES);
}

A64Backend::~A64Backend() {
  ExceptionHandler::Uninstall(&ExceptionCallbackThunk, this);
  if (guest_trampoline_memory_) {
    memory::DeallocFixed(guest_trampoline_memory_,
                         kGuestTrampolineSize * MAX_GUEST_TRAMPOLINES,
                         memory::DeallocationType::kRelease);
    guest_trampoline_memory_ = nullptr;
  }
}

bool A64Backend::Initialize(Processor* processor) {
  if (!Backend::Initialize(processor)) {
    return false;
  }

  // Initialize the code cache.
  if (!code_cache_->Initialize()) {
    XELOGE("A64Backend: Failed to initialize code cache");
    return false;
  }

  // Expose the code cache to the base Backend class.
  Backend::code_cache_ = code_cache_.get();

  // Set up machine info for the register allocator.
  machine_info_.supports_extended_load_store = true;
  // GPR set: x19, x22-x28 (8 registers, excluding x20=context, x21=membase)
  auto& gpr_set = machine_info_.register_sets[0];
  gpr_set.id = 0;
  std::strcpy(gpr_set.name, "gpr");
  gpr_set.types = MachineInfo::RegisterSet::INT_TYPES;
  gpr_set.count = A64Emitter::GPR_COUNT;
  // VEC set: v4-v7, v16-v31 (20 registers, v0-v3 scratch)
  auto& vec_set = machine_info_.register_sets[1];
  vec_set.id = 1;
  std::strcpy(vec_set.name, "vec");
  vec_set.types = MachineInfo::RegisterSet::FLOAT_TYPES |
                  MachineInfo::RegisterSet::VEC_TYPES;
  vec_set.count = A64Emitter::VEC_COUNT;

  // Generate thunks using ARM64 assembler.
  XbyakA64Allocator allocator;
  A64HelperEmitter thunk_emitter(this, &allocator);

  size_t host_to_guest_thunk_size = 0;
  size_t guest_to_host_thunk_size = 0;
  size_t resolve_function_thunk_size = 0;
  size_t synchronize_guest_and_host_stack_helper_size = 0;

  host_to_guest_thunk_ =
      thunk_emitter.EmitHostToGuestThunk(&host_to_guest_thunk_size);
  guest_to_host_thunk_ =
      thunk_emitter.EmitGuestToHostThunk(&guest_to_host_thunk_size);
  resolve_function_thunk_ =
      thunk_emitter.EmitResolveFunctionThunk(&resolve_function_thunk_size);

  if (!host_to_guest_thunk_ || !guest_to_host_thunk_ ||
      !resolve_function_thunk_) {
    XELOGE("A64Backend: Failed to generate thunks");
    return false;
  }

  if (cvars::a64_enable_host_guest_stack_synchronization) {
    synchronize_guest_and_host_stack_helper_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackHelper(
            &synchronize_guest_and_host_stack_helper_size);
  }
  host_to_guest_thunk_size_ = static_cast<uint32_t>(host_to_guest_thunk_size);
  guest_to_host_thunk_size_ = static_cast<uint32_t>(guest_to_host_thunk_size);
  resolve_function_thunk_size_ =
      static_cast<uint32_t>(resolve_function_thunk_size);
  synchronize_guest_and_host_stack_helper_size_ =
      static_cast<uint32_t>(synchronize_guest_and_host_stack_helper_size);

  // Set the indirection table default to point at the resolve thunk.
#if XE_A64_INDIRECTION_64BIT
  code_cache_->set_indirection_default_64(
      reinterpret_cast<uint64_t>(resolve_function_thunk_));
#else
  code_cache_->set_indirection_default(
      uint32_t(reinterpret_cast<uint64_t>(resolve_function_thunk_)));
#endif

  // Commit the indirection table range used by guest trampolines so that
  // CreateGuestTrampoline can call AddIndirection without faulting.
  code_cache_->CommitExecutableRange(GUEST_TRAMPOLINE_BASE,
                                     GUEST_TRAMPOLINE_END);

  // Commit special indirection ranges (force return address, etc.).
  code_cache_->CommitExecutableRange(0x9FFF0000, 0x9FFFFFFF);

  // Register exception handler for MMIO access from JIT code.
  ExceptionHandler::Install(ExceptionCallbackThunk, this);
  if (cvars::record_mmio_access_exceptions) {
    processor->memory()->SetMMIOExceptionRecordingCallback(
        ForwardMMIOAccessForRecording, this);
  }

  return true;
}

void A64Backend::CommitExecutableRange(uint32_t guest_low,
                                       uint32_t guest_high) {
  code_cache_->CommitExecutableRange(guest_low, guest_high);
}

std::unique_ptr<Assembler> A64Backend::CreateAssembler() {
  return std::make_unique<A64Assembler>(this);
}

std::unique_ptr<GuestFunction> A64Backend::CreateGuestFunction(
    Module* module, uint32_t address) {
  return std::make_unique<A64Function>(module, address);
}

uint64_t A64Backend::CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                                  uint64_t current_pc) {
  // ARM64 instructions are fixed 4 bytes.
  return current_pc + 4;
}

// ARM64 BRK #0 encoding (4 bytes, fixed-width instruction).
static constexpr uint32_t kArm64Brk0 = 0xD4200000;
static constexpr uint32_t kArm64BrkMask = 0xFFE0001F;
static constexpr uint32_t kArm64BrkFixed = 0xD4200000;

void A64Backend::InstallBreakpoint(Breakpoint* breakpoint) {
  breakpoint->ForEachHostAddress([breakpoint](uint64_t host_address) {
    auto ptr = reinterpret_cast<void*>(host_address);
    auto original_bytes = xe::load<uint32_t>(ptr);
    assert_true(original_bytes != kArm64Brk0);
    xe::store<uint32_t>(ptr, kArm64Brk0);
    breakpoint->backend_data().emplace_back(host_address, original_bytes);
  });
}

void A64Backend::InstallBreakpoint(Breakpoint* breakpoint, Function* fn) {
  assert_true(breakpoint->address_type() == Breakpoint::AddressType::kGuest);
  assert_true(fn->is_guest());
  auto guest_function = reinterpret_cast<cpu::GuestFunction*>(fn);
  auto host_address =
      guest_function->MapGuestAddressToMachineCode(breakpoint->guest_address());
  if (!host_address) {
    assert_always();
    return;
  }

  auto ptr = reinterpret_cast<void*>(host_address);
  auto original_bytes = xe::load<uint32_t>(ptr);
  assert_true(original_bytes != kArm64Brk0);
  xe::store<uint32_t>(ptr, kArm64Brk0);
  breakpoint->backend_data().emplace_back(host_address, original_bytes);
}

void A64Backend::UninstallBreakpoint(Breakpoint* breakpoint) {
  for (auto& pair : breakpoint->backend_data()) {
    auto ptr = reinterpret_cast<uint8_t*>(pair.first);
    auto instruction_bytes = xe::load<uint32_t>(ptr);
    assert_true(instruction_bytes == kArm64Brk0);
    xe::store<uint32_t>(ptr, static_cast<uint32_t>(pair.second));
  }
  breakpoint->backend_data().clear();
}

void A64Backend::InitializeBackendContext(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  std::memset(a64_ctx, 0, sizeof(A64BackendContext));
  a64_ctx->reserve_helper_ = &reserve_helper_;
  a64_ctx->Ox1000 = 0x1000;
  a64_ctx->fpcr_fpu = DEFAULT_FPU_FPCR;
  a64_ctx->fpcr_vmx = DEFAULT_VMX_FPCR;
  a64_ctx->flags = (1U << kA64BackendNJMOn);  // NJM on by default
  a64_ctx->guest_tick_count = Clock::GetGuestTickCountPointer();

  // Allocate stackpoints for longjmp detection.
  if (cvars::a64_enable_host_guest_stack_synchronization) {
    uint64_t max_stackpoints = cvars::a64_max_stackpoints;
    if (max_stackpoints > 0) {
      a64_ctx->stackpoints = new A64BackendStackpoint[max_stackpoints]();
    }
  }

  // Reset the live host FPCR for a fresh PPC context so one test's rounding
  // state does not leak into the next on the shared PPC test runner thread.
  SetGuestRoundingMode(ctx, 0);
}

void A64Backend::DeinitializeBackendContext(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  if (a64_ctx->stackpoints) {
    delete[] a64_ctx->stackpoints;
    a64_ctx->stackpoints = nullptr;
  }
}

void A64Backend::PrepareForReentry(void* ctx) {
  auto* a64_ctx = BackendContextForGuestContext(ctx);
  a64_ctx->current_stackpoint_depth = 0;
}

uint32_t A64Backend::CreateGuestTrampoline(GuestTrampolineProc proc,
                                           void* userdata1, void* userdata2,
                                           bool long_term) {
  size_t new_index;
  if (long_term) {
    new_index = guest_trampoline_address_bitmap_.AcquireFromBack();
  } else {
    new_index = guest_trampoline_address_bitmap_.Acquire();
  }
  xenia_assert(new_index != static_cast<size_t>(-1));

  uint8_t* write_pos =
      &guest_trampoline_memory_[kGuestTrampolineSize * new_index];

#if XE_PLATFORM_MAC
  // MAP_JIT pages are only writable while thread-local JIT write protection is
  // disabled. Guest trampolines are emitted lazily on running guest threads, so
  // we must flip the thread into write mode for this small patch-up window.
  pthread_jit_write_protect_np(0);
#endif
  BuildGuestTrampoline(write_pos, reinterpret_cast<void*>(proc), userdata1,
                       userdata2,
                       reinterpret_cast<void*>(guest_to_host_thunk_));
#if XE_PLATFORM_MAC
  pthread_jit_write_protect_np(1);
#endif

  // Flush instruction cache for the new trampoline code.
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), write_pos, kGuestTrampolineSize);
#else
  __builtin___clear_cache(
      reinterpret_cast<char*>(write_pos),
      reinterpret_cast<char*>(write_pos + kGuestTrampolineSize));
#endif

  uint32_t indirection_guest_addr =
      GUEST_TRAMPOLINE_BASE +
      (static_cast<uint32_t>(new_index) * GUEST_TRAMPOLINE_MIN_LEN);

#if XE_A64_INDIRECTION_64BIT
  code_cache()->AddIndirection64(indirection_guest_addr,
                                 reinterpret_cast<uint64_t>(write_pos));
#else
  code_cache()->AddIndirection(
      indirection_guest_addr,
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(write_pos)));
#endif

  return indirection_guest_addr;
}

void A64Backend::FreeGuestTrampoline(uint32_t trampoline_addr) {
  xenia_assert(trampoline_addr >= GUEST_TRAMPOLINE_BASE &&
               trampoline_addr < GUEST_TRAMPOLINE_END);
  size_t index =
      (trampoline_addr - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;
  guest_trampoline_address_bitmap_.Release(index);
}

// PPC rounding mode (3-bit) to ARM64 FPCR value.
// Same table as in a64_sequences.cc SET_ROUNDING_MODE.
static constexpr uint32_t fpcr_table[8] = {
    (0b00 << 22),              // PPC 0: nearest, IEEE
    (0b11 << 22),              // PPC 1: toward zero, IEEE
    (0b01 << 22),              // PPC 2: toward +inf, IEEE
    (0b10 << 22),              // PPC 3: toward -inf, IEEE
    (0b00 << 22) | (1 << 24),  // PPC 4: nearest, flush-to-zero
    (0b11 << 22) | (1 << 24),  // PPC 5: toward zero, flush-to-zero
    (0b01 << 22) | (1 << 24),  // PPC 6: toward +inf, flush-to-zero
    (0b10 << 22) | (1 << 24),  // PPC 7: toward -inf, flush-to-zero
};

void A64Backend::SetGuestRoundingMode(void* ctx, unsigned int mode) {
  A64BackendContext* bctx = BackendContextForGuestContext(ctx);
  uint32_t control = mode & 7;
  uint32_t fpcr_val = fpcr_table[control];
#if XE_COMPILER_MSVC
  // MSVC ARM64 intrinsic: ARM64_FPCR = register ID 0x5A20.
  _WriteStatusReg(0x5A20, static_cast<uint64_t>(fpcr_val));
#else
  __asm__ volatile("msr fpcr, %0" : : "r"(static_cast<uint64_t>(fpcr_val)));
#endif
  bctx->fpcr_fpu = fpcr_val;
  if (control & 0b100) {
    bctx->flags |= (1u << kA64BackendNonIEEEMode);
  } else {
    bctx->flags &= ~(1u << kA64BackendNonIEEEMode);
  }
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(ctx);
  ppc_context->fpscr.bits.rn = control;
  ppc_context->fpscr.bits.ni = control >> 2;
}

bool A64Backend::PopulatePseudoStacktrace(GuestPseudoStackTrace* st) {
  ThreadState* thrd_state = ThreadState::Get();
  if (!thrd_state) {
    return false;
  }
  ppc::PPCContext* ctx = thrd_state->context();
  A64BackendContext* backend_ctx = BackendContextForGuestContext(ctx);

  uint32_t depth = backend_ctx->current_stackpoint_depth - 1;
  if (static_cast<int32_t>(depth) < 1) {
    return false;
  }
  uint32_t num_entries_to_populate =
      std::min(MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES, depth);

  st->count = num_entries_to_populate;
  st->truncated_flag = num_entries_to_populate < depth ? 1 : 0;

  A64BackendStackpoint* current_stackpoint =
      &backend_ctx->stackpoints[backend_ctx->current_stackpoint_depth - 1];

  for (uint32_t stp_index = 0; stp_index < num_entries_to_populate;
       ++stp_index) {
    st->return_addrs[stp_index] = current_stackpoint->guest_return_address_;
    current_stackpoint--;
  }
  return true;
}

void A64Backend::RecordMMIOExceptionForGuestInstruction(void* host_address) {
  uint64_t host_addr_u64 = reinterpret_cast<uint64_t>(host_address);
  auto fnfor = code_cache()->LookupFunction(host_addr_u64);
  if (fnfor) {
    uint32_t guestaddr = fnfor->MapMachineCodeToGuestAddress(host_addr_u64);
    Module* guest_module = fnfor->module();
    if (guest_module) {
      XexModule* xex_guest_module = dynamic_cast<XexModule*>(guest_module);
      if (xex_guest_module) {
        cpu::InfoCacheFlags* icf =
            xex_guest_module->GetInstructionAddressFlags(guestaddr);
        if (icf) {
          const bool was_mmio = icf->accessed_mmio;
          icf->accessed_mmio = true;
          if (!was_mmio) {
            xex_guest_module->FlushInfoCache();
          }
        }
      }
    }
  }
}

const char* A64Backend::LookupHelperThunk(uint64_t host_pc,
                                          uint32_t* offset_out) const {
  struct HelperRange {
    uint64_t start;
    uint32_t size;
    const char* name;
  };
  const HelperRange ranges[] = {
      {reinterpret_cast<uint64_t>(host_to_guest_thunk_), host_to_guest_thunk_size_,
       "host_to_guest"},
      {reinterpret_cast<uint64_t>(guest_to_host_thunk_), guest_to_host_thunk_size_,
       "guest_to_host"},
      {reinterpret_cast<uint64_t>(resolve_function_thunk_),
       resolve_function_thunk_size_, "resolve_function"},
      {reinterpret_cast<uint64_t>(synchronize_guest_and_host_stack_helper_),
       synchronize_guest_and_host_stack_helper_size_, "stack_sync_helper"},
  };

  for (const auto& range : ranges) {
    if (!range.start || !range.size) {
      continue;
    }
    const uint64_t end = range.start + range.size;
    if (host_pc >= range.start && host_pc < end) {
      if (offset_out) {
        *offset_out = static_cast<uint32_t>(host_pc - range.start);
      }
      return range.name;
    }
  }
  return nullptr;
}

bool A64Backend::ExceptionCallbackThunk(Exception* ex, void* data) {
  auto* backend = reinterpret_cast<A64Backend*>(data);
  return backend->ExceptionCallback(ex);
}

bool A64Backend::ExceptionCallback(Exception* ex) {
  if (ex->code() == Exception::Code::kAccessViolation) {
    const uint64_t host_pc = ex->pc();
    const uint64_t fault_address = ex->fault_address();
    uint64_t guest_pc = 0;
    uint32_t host_offset = 0;
    uint32_t helper_offset = 0;
    bool in_trampoline_stub = false;
    const char* helper_name = LookupHelperThunk(host_pc, &helper_offset);
    if (guest_trampoline_memory_) {
      const uint64_t tramp_base =
          reinterpret_cast<uint64_t>(guest_trampoline_memory_);
      const uint64_t tramp_end =
          tramp_base +
          static_cast<uint64_t>(kGuestTrampolineSize) *
              static_cast<uint64_t>(MAX_GUEST_TRAMPOLINES);
      if (host_pc >= tramp_base && host_pc < tramp_end) {
        const size_t stub_index = static_cast<size_t>((host_pc - tramp_base) /
                                                      kGuestTrampolineSize);
        guest_pc = GUEST_TRAMPOLINE_BASE +
                   static_cast<uint32_t>(stub_index) * GUEST_TRAMPOLINE_MIN_LEN;
        in_trampoline_stub = true;
      }
    }
    auto function = code_cache_->LookupFunction(host_pc);
    if (function && function->machine_code()) {
      const uint64_t function_pc =
          reinterpret_cast<uint64_t>(function->machine_code());
      host_offset = static_cast<uint32_t>(host_pc - function_pc);
      if (const auto* entry = function->LookupMachineCodeOffset(host_offset)) {
        guest_pc = entry->guest_address;
      }
    }
    auto* thread_context = ex->thread_context();
    XELOGE(
        "A64 AV: host_pc=0x{:016X} guest_pc=0x{:08X} host_off=0x{:X} "
        "fault=0x{:016X} op={} helper={} helper_off=0x{:X} trampoline={} "
        "x0=0x{:016X} x1=0x{:016X} x2=0x{:016X} x9=0x{:016X} "
        "x16=0x{:016X} x20=0x{:016X} x21=0x{:016X}",
        host_pc, guest_pc, host_offset, fault_address,
        static_cast<int>(ex->access_violation_operation()),
        helper_name ? helper_name : "-",
        helper_name ? helper_offset : 0, in_trampoline_stub,
        thread_context ? thread_context->x[0] : 0,
        thread_context ? thread_context->x[1] : 0,
        thread_context ? thread_context->x[2] : 0,
        thread_context ? thread_context->x[9] : 0,
        thread_context ? thread_context->x[16] : 0,
        thread_context ? thread_context->x[20] : 0,
        thread_context ? thread_context->x[21] : 0);
#if XE_PLATFORM_MAC || XE_PLATFORM_LINUX || XE_PLATFORM_IOS
    if (!function && !helper_name && !in_trampoline_stub) {
      const uint64_t code_base = code_cache_->execute_base_address();
      const uint64_t code_end = code_base + code_cache_->total_size();
      XELOGE("A64 AV: host_pc in code cache range? {} base=0x{:016X} end=0x{:016X}",
             (host_pc >= code_base && host_pc < code_end), code_base, code_end);
      Dl_info info;
      if (dladdr(reinterpret_cast<void*>(host_pc), &info) && info.dli_fname) {
        XELOGE("A64 AV: dladdr image={} sym={}",
               info.dli_fname, info.dli_sname ? info.dli_sname : "unknown");
      }
    }
#endif
    return false;
  }
  if (ex->code() != Exception::Code::kIllegalInstruction) {
    return false;
  }

  const uint64_t host_pc = ex->pc();
  const auto instruction_bytes =
      xe::load<uint32_t>(reinterpret_cast<void*>(host_pc));
  if ((instruction_bytes & kArm64BrkMask) != kArm64BrkFixed) {
    return false;
  }

  const uint32_t trap_imm = (instruction_bytes >> 5) & 0xFFFF;
  uint32_t host_offset = 0;
  uint32_t helper_offset = 0;
  uint64_t guest_pc = 0;
  bool in_trampoline_stub = false;
  const char* helper_name = LookupHelperThunk(host_pc, &helper_offset);
  if (guest_trampoline_memory_) {
    const uint64_t tramp_base =
        reinterpret_cast<uint64_t>(guest_trampoline_memory_);
    const uint64_t tramp_end =
        tramp_base + static_cast<uint64_t>(kGuestTrampolineSize) *
                         static_cast<uint64_t>(MAX_GUEST_TRAMPOLINES);
    if (host_pc >= tramp_base && host_pc < tramp_end) {
      const size_t stub_index =
          static_cast<size_t>((host_pc - tramp_base) / kGuestTrampolineSize);
      guest_pc = GUEST_TRAMPOLINE_BASE +
                 static_cast<uint32_t>(stub_index) * GUEST_TRAMPOLINE_MIN_LEN;
      in_trampoline_stub = true;
    }
  }
  auto* function = code_cache_->LookupFunction(host_pc);
  if (function && function->machine_code()) {
    const uint64_t function_pc =
        reinterpret_cast<uint64_t>(function->machine_code());
    host_offset = static_cast<uint32_t>(host_pc - function_pc);
    if (const auto* entry = function->LookupMachineCodeOffset(host_offset)) {
      guest_pc = entry->guest_address;
    }
  }

  if (trap_imm != 0) {
    auto* thread_context = ex->thread_context();
    XELOGE(
        "A64 BRK: imm=0x{:X} host_pc=0x{:016X} guest_pc=0x{:08X} "
        "host_off=0x{:X} helper={} helper_off=0x{:X} trampoline={} "
        "x0=0x{:016X} x1=0x{:016X} x2=0x{:016X} x9=0x{:016X} "
        "x16=0x{:016X} x20=0x{:016X} x21=0x{:016X}",
        trap_imm, host_pc, guest_pc, host_offset, helper_name ? helper_name : "-",
        helper_name ? helper_offset : 0, in_trampoline_stub,
        thread_context ? thread_context->x[0] : 0,
        thread_context ? thread_context->x[1] : 0,
        thread_context ? thread_context->x[2] : 0,
        thread_context ? thread_context->x[9] : 0,
        thread_context ? thread_context->x[16] : 0,
        thread_context ? thread_context->x[20] : 0,
        thread_context ? thread_context->x[21] : 0);
    if (function) {
      XELOGE("A64 BRK: function='{}' range=[0x{:08X},0x{:08X})",
             function->name(), function->address(), function->end_address());
    }
#if XE_PLATFORM_MAC || XE_PLATFORM_LINUX || XE_PLATFORM_IOS
    if (!function && !helper_name && !in_trampoline_stub) {
      const uint64_t code_base = code_cache_->execute_base_address();
      const uint64_t code_end = code_base + code_cache_->total_size();
      XELOGE(
          "A64 BRK: host_pc in code cache range? {} base=0x{:016X} end=0x{:016X}",
          (host_pc >= code_base && host_pc < code_end), code_base, code_end);
      Dl_info info;
      if (dladdr(reinterpret_cast<void*>(host_pc), &info) && info.dli_fname) {
        XELOGE("A64 BRK: dladdr image={} sym={}", info.dli_fname,
               info.dli_sname ? info.dli_sname : "unknown");
      }
    }
#endif
    return false;
  }

  return processor()->OnThreadBreakpointHit(ex);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
