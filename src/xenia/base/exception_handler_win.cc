/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/exception_handler.h"

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/base/platform_win.h"

namespace xe {

// Handle of the added VectoredExceptionHandler.
void* veh_handle_ = nullptr;
// Handle of the added VectoredContinueHandler.
void* vch_handle_ = nullptr;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

static void CaptureThreadContext(HostThreadContext& thread_context,
                                 PCONTEXT ctx) {
#if XE_ARCH_AMD64
  thread_context.rip = ctx->Rip;
  thread_context.eflags = ctx->EFlags;
  std::memcpy(thread_context.int_registers, &ctx->Rax,
              sizeof(thread_context.int_registers));
  std::memcpy(thread_context.xmm_registers, &ctx->Xmm0,
              sizeof(thread_context.xmm_registers));
#elif XE_ARCH_ARM64
  thread_context.pc = ctx->Pc;
  thread_context.pstate = ctx->Cpsr;
  thread_context.sp = ctx->Sp;
  std::memcpy(thread_context.x, &ctx->X0, sizeof(thread_context.x));
  std::memcpy(thread_context.v, &ctx->V[0], sizeof(thread_context.v));
#endif
}

static void RestoreThreadContext(PCONTEXT ctx,
                                 const HostThreadContext& thread_context,
                                 const Exception& ex) {
#if XE_ARCH_AMD64
  ctx->Rip = thread_context.rip;
  ctx->EFlags = thread_context.eflags;
  uint32_t modified_register_index;
  uint16_t modified_int_registers_remaining = ex.modified_int_registers();
  while (xe::bit_scan_forward(modified_int_registers_remaining,
                              &modified_register_index)) {
    modified_int_registers_remaining &=
        ~(UINT16_C(1) << modified_register_index);
    (&ctx->Rax)[modified_register_index] =
        thread_context.int_registers[modified_register_index];
  }
  uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
  while (xe::bit_scan_forward(modified_xmm_registers_remaining,
                              &modified_register_index)) {
    modified_xmm_registers_remaining &=
        ~(UINT16_C(1) << modified_register_index);
    std::memcpy(&ctx->Xmm0 + modified_register_index,
                &thread_context.xmm_registers[modified_register_index],
                sizeof(vec128_t));
  }
#elif XE_ARCH_ARM64
  ctx->Pc = thread_context.pc;
  ctx->Cpsr = thread_context.pstate;
  ctx->Sp = thread_context.sp;
  std::memcpy(&ctx->X0, thread_context.x, sizeof(thread_context.x));
  std::memcpy(&ctx->V[0], thread_context.v, sizeof(thread_context.v));
#endif
}

LONG CALLBACK ExceptionHandlerCallback(PEXCEPTION_POINTERS ex_info) {
  // Visual Studio SetThreadName.
  if (ex_info->ExceptionRecord->ExceptionCode == 0x406D1388) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HostThreadContext thread_context;
  CaptureThreadContext(thread_context, ex_info->ContextRecord);

  // https://msdn.microsoft.com/en-us/library/ms679331(v=vs.85).aspx
  // https://msdn.microsoft.com/en-us/library/aa363082(v=vs.85).aspx
  Exception ex;
  switch (ex_info->ExceptionRecord->ExceptionCode) {
    case STATUS_ILLEGAL_INSTRUCTION:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case STATUS_ACCESS_VIOLATION: {
      Exception::AccessViolationOperation access_violation_operation;
      switch (ex_info->ExceptionRecord->ExceptionInformation[0]) {
        case 0:
          access_violation_operation =
              Exception::AccessViolationOperation::kRead;
          break;
        case 1:
          access_violation_operation =
              Exception::AccessViolationOperation::kWrite;
          break;
        default:
          access_violation_operation =
              Exception::AccessViolationOperation::kUnknown;
          break;
      }
      ex.InitializeAccessViolation(
          &thread_context, ex_info->ExceptionRecord->ExceptionInformation[1],
          access_violation_operation);
    } break;
    default:
      // Unknown/unhandled type.
      return EXCEPTION_CONTINUE_SEARCH;
  }

  for (size_t i = 0; i < xe::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
      RestoreThreadContext(ex_info->ContextRecord, thread_context, ex);
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!veh_handle_) {
    veh_handle_ = AddVectoredExceptionHandler(1, ExceptionHandlerCallback);

    if (IsDebuggerPresent()) {
      // TODO(benvanik): do we need a continue handler if a debugger is
      // attached?
      // vch_handle_ = AddVectoredContinueHandler(1, ExceptionHandlerCallback);
    }
  }

  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < xe::countof(handlers_) - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }

  bool has_any = false;
  for (size_t i = 0; i < xe::countof(handlers_); ++i) {
    if (handlers_[i].first) {
      has_any = true;
      break;
    }
  }
  if (!has_any) {
    if (veh_handle_) {
      RemoveVectoredExceptionHandler(veh_handle_);
      veh_handle_ = nullptr;
    }
    if (vch_handle_) {
      RemoveVectoredContinueHandler(vch_handle_);
      vch_handle_ = nullptr;
    }
  }
}

}  // namespace xe
