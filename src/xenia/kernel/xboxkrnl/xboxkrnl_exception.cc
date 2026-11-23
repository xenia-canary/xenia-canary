/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/exception_dispatch.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

constexpr uint32_t kExceptionSetThreadName = 0x406D1388;

void HandleSetThreadName(X_EXCEPTION_RECORD* record) {
  // https://learn.microsoft.com/en-us/visualstudio/debugger/tips-for-debugging-threads
  struct X_THREADNAME_INFO {
    xe::be<uint32_t> type;
    xe::be<uint32_t> name_ptr;
    xe::be<uint32_t> thread_id;
    xe::be<uint32_t> flags;
  };
  static_assert_size(X_THREADNAME_INFO, 0x10);

  // Flags isn't read, so only the words up to it are needed.
  constexpr uint32_t kRequiredParameters =
      offsetof(X_THREADNAME_INFO, flags) / sizeof(uint32_t);

  if (record->number_parameters < kRequiredParameters) {
    XELOGW("SetThreadName called with {} parameters",
           static_cast<uint32_t>(record->number_parameters));
    return;
  }

  auto thread_info =
      reinterpret_cast<X_THREADNAME_INFO*>(&record->exception_information[0]);

  assert_true(thread_info->type == 0x1000);

  if (!thread_info->name_ptr) {
    XELOGD("SetThreadName called with null name_ptr");
    return;
  }

  // 4D5307D6 (and its demo) frees the name before its thread starts, so by
  // SetThreadName it's junk. Ignore names that aren't printable.
  const char* name_ptr =
      kernel_memory()->TranslateVirtual<const char*>(thread_info->name_ptr);
  std::string name(name_ptr, strnlen(name_ptr, 256));

  if (name.empty() ||
      std::ranges::any_of(name, [](uint8_t c) { return c < 32 || c == 127; })) {
    XELOGD("SetThreadName called with an invalid name");
    return;
  }

  // TODO(gibbed): cvar for thread name encoding for conversion, some games use
  // SJIS and there's no way to automatically know this.
  std::ranges::replace_if(name, [](uint8_t c) { return c > 127; }, '?');

  object_ref<XThread> thread;
  if (thread_info->thread_id == -1) {
    // Current thread.
    thread = retain_object(XThread::GetCurrentThread());
  } else {
    // Lookup thread by ID.
    thread = kernel_state()->GetThreadByID(thread_info->thread_id);
  }

  if (thread) {
    XELOGD("SetThreadName({}, {})", thread->thread_id(), name);
    thread->set_name(name);
  }
}

}  // namespace

void RtlRaiseException_entry(pointer_t<X_EXCEPTION_RECORD> record,
                             const ppc_context_t& ctx) {
  const bool set_thread_name = record->code == kExceptionSetThreadName;

  if (set_thread_name) {
    HandleSetThreadName(record);
  }

  const uint32_t frame_address = AllocateKernelFrame(ctx);
  if (!frame_address) {
    return;
  }

  auto frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(frame_address);
  const uint32_t context_address =
      frame_address + offsetof(X_KERNEL_FRAME, context);

  // Search from the call, so the caller's scopes are looked up there.
  std::memcpy(&frame->context, &frame->caller_context, sizeof(X_CONTEXT));
  record->exception_address = frame->caller_context.iar;
  const uint32_t call_pc = frame->caller_context.lr - 4;

  const bool handled = DispatchException(
      ctx, frame_address, record.guest_address(), context_address, call_pc);
  if (handled) {
    RestoreContext(ctx, &frame->context);
  } else if (!set_thread_name) {
    // Carry on as if the raise had been ignored.
    XELOGE("No guest handler took exception {:08X} raised at {:08X}",
           static_cast<uint32_t>(record->code),
           static_cast<uint32_t>(record->exception_address));
  }

  ReleaseKernelFrame(frame_address);
}
DECLARE_XBOXKRNL_EXPORT2(RtlRaiseException, kDebug, kImplemented, kImportant);

void RtlUnwind_entry(dword_t target_frame, dword_t target_ip,
                     dword_t exception_record, dword_t return_value,
                     const ppc_context_t& ctx) {
  const uint32_t frame_address = AllocateKernelFrame(ctx);
  if (!frame_address) {
    return;
  }

  // Returns to the target, or to the caller if it isn't found.
  Unwind(ctx, frame_address, target_frame, target_ip, exception_record,
         return_value, frame_address + offsetof(X_KERNEL_FRAME, context));
  ReleaseKernelFrame(frame_address);
}
DECLARE_XBOXKRNL_EXPORT1(RtlUnwind, kDebug, kImplemented);

void RtlCaptureContext_entry(pointer_t<X_CONTEXT> context_record,
                             const ppc_context_t& ctx) {
  CaptureContext(ctx, context_record);
}
DECLARE_XBOXKRNL_EXPORT1(RtlCaptureContext, kDebug, kImplemented);

// The C __try handler. Its data is a table of scopes.
void __C_specific_handler_entry(dword_t record_address,
                                dword_t establisher_frame,
                                dword_t context_address,
                                dword_t dispatcher_context_address,
                                const ppc_context_t& ctx) {
  struct X_SCOPE_RECORD {
    xe::be<uint32_t> begin_address;
    xe::be<uint32_t> end_address;
    xe::be<uint32_t> handler_address;
    xe::be<uint32_t> jump_target;
  };

  auto record =
      kernel_memory()->TranslateVirtual<X_EXCEPTION_RECORD*>(record_address);
  auto context = kernel_memory()->TranslateVirtual<X_CONTEXT*>(context_address);
  auto dispatcher_context =
      kernel_memory()->TranslateVirtual<X_DISPATCHER_CONTEXT*>(
          dispatcher_context_address);

  const uint32_t control_pc = dispatcher_context->control_pc;
  const uint32_t begin = kernel_memory()
                             ->TranslateVirtual<const X_RUNTIME_FUNCTION*>(
                                 dispatcher_context->function_entry)
                             ->begin_address;
  const uint32_t scope_table = xe::load_and_swap<uint32_t>(
      kernel_memory()->TranslateVirtual<uint8_t*>(begin - 4));
  const uint32_t scope_count = xe::load_and_swap<uint32_t>(
      kernel_memory()->TranslateVirtual<uint8_t*>(scope_table));
  auto scopes =
      kernel_memory()->TranslateVirtual<const X_SCOPE_RECORD*>(scope_table + 4);

  const uint32_t frame_address = AllocateKernelFrame(ctx);
  if (!frame_address) {
    ctx->r[3] = kExceptionContinueSearch;
    return;
  }

  auto frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(frame_address);

  if (!(record->exception_flags & kExceptionUnwind)) {
    // Dispatching: run the filters of the scopes around the exception.
    const uint32_t exception_pointers =
        frame_address + offsetof(X_KERNEL_FRAME, exception_pointers);
    frame->exception_pointers[0] = static_cast<uint32_t>(record_address);
    frame->exception_pointers[1] = static_cast<uint32_t>(context_address);

    for (uint32_t i = 0; i < scope_count; ++i) {
      const X_SCOPE_RECORD& scope = scopes[i];
      if (control_pc < scope.begin_address || control_pc >= scope.end_address ||
          !scope.jump_target) {
        continue;
      }

      GuestCallArgs args;
      args.r3 = exception_pointers;
      args.r4 = scope.handler_address;
      args.r5 = establisher_frame;
      args.r12 = establisher_frame;
      const int32_t value = static_cast<int32_t>(
          CallGuest(ctx, frame_address, kKernelFrameCallout, 0,
                    scope.handler_address, args));

      if (value < 0) {
        ReleaseKernelFrame(frame_address);
        ctx->r[3] = kExceptionContinueExecution;
        return;
      }

      if (value > 0) {
        // Continue at the __except block, with the code in r3.
        if (Unwind(ctx, frame_address, establisher_frame, scope.jump_target,
                   record_address, record->code, context_address)) {
          ReleaseKernelFrame(frame_address);
          return;
        }
      }
    }
  } else {
    // Unwinding: run the termination handlers of the scopes left.
    const uint32_t target_pc = context->iar;
    for (uint32_t i = 0; i < scope_count; ++i) {
      const X_SCOPE_RECORD& scope = scopes[i];
      if (control_pc < scope.begin_address || control_pc >= scope.end_address) {
        continue;
      }

      if (target_pc >= scope.begin_address && target_pc <= scope.end_address) {
        break;
      }

      if (scope.jump_target) {
        if (target_pc == scope.jump_target) {
          break;
        }

        continue;
      }

      // Past the scope, so a colliding unwind doesn't rerun it.
      dispatcher_context->control_pc = scope.end_address + 4;

      GuestCallArgs args;
      args.r3 = 1;  // abnormal termination
      args.r4 = scope.handler_address;
      args.r5 = establisher_frame;
      args.r12 = establisher_frame;
      CallGuest(ctx, frame_address, kKernelFrameCallout, 0,
                scope.handler_address, args);
    }
  }

  ReleaseKernelFrame(frame_address);
  ctx->r[3] = kExceptionContinueSearch;
}
DECLARE_XBOXKRNL_EXPORT1(__C_specific_handler, kDebug, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Exception);
