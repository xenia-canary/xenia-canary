/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_EXCEPTION_DISPATCH_H_
#define XENIA_KERNEL_XBOXKRNL_EXCEPTION_DISPATCH_H_

#include <cstddef>
#include <cstdint>

#include "xenia/base/byte_order.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

inline constexpr uint32_t kExceptionNoncontinuable = 0x01;
inline constexpr uint32_t kExceptionUnwinding = 0x02;
inline constexpr uint32_t kExceptionExitUnwind = 0x04;
inline constexpr uint32_t kExceptionStackInvalid = 0x08;
inline constexpr uint32_t kExceptionNestedCall = 0x10;
inline constexpr uint32_t kExceptionTargetUnwind = 0x20;
inline constexpr uint32_t kExceptionCollidedUnwind = 0x40;
inline constexpr uint32_t kExceptionUnwind =
    kExceptionUnwinding | kExceptionExitUnwind | kExceptionTargetUnwind |
    kExceptionCollidedUnwind;

inline constexpr uint32_t kExceptionContinueExecution = 0;
inline constexpr uint32_t kExceptionContinueSearch = 1;
inline constexpr uint32_t kExceptionNestedException = 2;
inline constexpr uint32_t kExceptionCollidedUnwindDisposition = 3;

// Exceptions a dispatch can raise in turn for bad handler results.
inline constexpr uint32_t kMaxRaisedExceptions = 4;

inline constexpr uint32_t kStatusNoncontinuableException = 0xC0000025;
inline constexpr uint32_t kStatusInvalidDisposition = 0xC0000026;
inline constexpr uint32_t kStatusUnwind = 0xC0000027;

inline constexpr uint32_t kContextControl = 0x1;
inline constexpr uint32_t kContextFloatingPoint = 0x2;
inline constexpr uint32_t kContextInteger = 0x4;
inline constexpr uint32_t kContextVector = 0x10;
inline constexpr uint32_t kContextFull =
    kContextControl | kContextFloatingPoint | kContextInteger | kContextVector;

// Context record, as passed to exception handlers.
struct X_CONTEXT {
  xe::be<uint32_t> context_flags;
  xe::be<uint32_t> msr;
  xe::be<uint32_t> iar;
  xe::be<uint32_t> lr;
  xe::be<uint64_t> ctr;
  xe::be<uint64_t> gpr[32];
  xe::be<uint32_t> cr;
  xe::be<uint32_t> xer;
  xe::be<uint64_t> fpscr;
  xe::be<uint64_t> fpr[32];
  xe::be<uint32_t> user_mode_control;
  xe::be<uint32_t> fill;
  xe::be<uint32_t> vscr[4];
  xe::be<uint32_t> vr[128][4];
};
static_assert_size(X_CONTEXT, 0xA40);
static_assert(offsetof(X_CONTEXT, gpr) == 0x18);
static_assert(offsetof(X_CONTEXT, cr) == 0x118);
static_assert(offsetof(X_CONTEXT, fpr) == 0x128);
static_assert(offsetof(X_CONTEXT, vr) == 0x240);

// .pdata entry kinds. A handler and its data are the two words before the
// function.
inline constexpr uint32_t kEntryKindSaveHelper = 0;
inline constexpr uint32_t kEntryKindRestoreHelper = 2;
inline constexpr uint32_t kEntryKindHandler = 3;

inline uint32_t PrologLength(uint32_t flags) { return flags & 0xFF; }
inline uint32_t FunctionLength(uint32_t flags) {
  return (flags >> 8) & 0x3FFFFF;
}
inline uint32_t EntryKind(uint32_t flags) { return flags >> 30; }

// .pdata entry: prolog length (bits 0-7), length (8-29), kind (30-31).
struct X_RUNTIME_FUNCTION {
  xe::be<uint32_t> begin_address;
  xe::be<uint32_t> flags;
};
static_assert_size(X_RUNTIME_FUNCTION, 8);

struct X_DISPATCHER_CONTEXT {
  xe::be<uint32_t> control_pc;
  xe::be<uint32_t> function_entry;
  xe::be<uint32_t> establisher_frame;
  xe::be<uint32_t> context_record;
};
static_assert_size(X_DISPATCHER_CONTEXT, 0x10);

// What guest code called from a kernel frame is.
enum KernelFrameKind : uint32_t {
  kKernelFrameCallout = 0,           // a filter or termination handler
  kKernelFrameExceptionHandler = 1,  // a handler, dispatching
  kKernelFrameUnwindHandler = 2,     // a handler, unwinding
};

// Left below the guest stack pointer by a routine that calls guest code, which
// gets the frame's address as its stack pointer.
struct X_KERNEL_FRAME {
  xe::be<uint32_t> back_chain;
  uint8_t callee_area[0x7C];
  xe::be<fourcc_t> magic;
  xe::be<uint32_t> kind;                    // KernelFrameKind
  xe::be<uint32_t> dispatcher_context_ptr;  // for handler calls
  xe::be<uint32_t> self;                    // the frame's address
  X_CONTEXT caller_context;
  X_DISPATCHER_CONTEXT dispatcher_context;
  X_EXCEPTION_RECORD exception_record;
  xe::be<uint32_t> exception_pointers[2];
  uint8_t pad_B38[0x8];
  X_CONTEXT context;
  X_EXCEPTION_RECORD raised_records[kMaxRaisedExceptions];
};
static_assert_size(X_KERNEL_FRAME, 0x16C0);
static_assert(offsetof(X_KERNEL_FRAME, caller_context) == 0x90);
static_assert(offsetof(X_KERNEL_FRAME, context) == 0xB40);

// Arguments for guest code called by CallGuest.
struct GuestCallArgs {
  uint64_t r3 = 0;
  uint64_t r4 = 0;
  uint64_t r5 = 0;
  uint64_t r6 = 0;
  uint64_t r7 = 0;
  uint64_t r12 = 0;
};

// As RtlCaptureContext.
void CaptureContext(const cpu::ppc::PPCContext* ppc, X_CONTEXT* context);

// Loads a context to continue at its Iar when the export returns.
void RestoreContext(cpu::ppc::PPCContext* ppc, const X_CONTEXT* context);

// A frame below the caller's stack pointer, or 0 if there's no room.
uint32_t AllocateKernelFrame(cpu::ppc::PPCContext* ppc);

void ReleaseKernelFrame(uint32_t address);

// Calls guest code from a routine with its frame as the stack. Returns r3.
uint32_t CallGuest(cpu::ppc::PPCContext* ppc, uint32_t frame_address,
                   KernelFrameKind kind, uint32_t dispatcher_context,
                   uint32_t function_address, const GuestCallArgs& args);

// As RtlDispatchException, from start_pc. Returns true if a handler continues
// execution at the context.
bool DispatchException(cpu::ppc::PPCContext* ppc, uint32_t frame_address,
                       uint32_t record_address, uint32_t context_address,
                       uint32_t start_pc);

// As RtlUnwind, with the context record at context_address. Returns true once
// the target's registers are loaded, and false if the target frame wasn't
// found.
bool Unwind(cpu::ppc::PPCContext* ppc, uint32_t frame_address,
            uint32_t target_frame, uint32_t target_ip, uint32_t record_address,
            uint32_t return_value, uint32_t context_address);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_EXCEPTION_DISPATCH_H_
