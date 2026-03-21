/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#define _XOPEN_SOURCE 600

#include "xenia/base/exception_handler.h"

#include <signal.h>
#include <ucontext.h>
#include <cstdint>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"

namespace xe {

bool signal_handlers_installed_ = false;
struct sigaction original_sigill_handler_;
struct sigaction original_sigtrap_handler_;
struct sigaction original_sigbus_handler_;
struct sigaction original_sigsegv_handler_;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

static void ExceptionHandlerCallback(int signal_number, siginfo_t* signal_info,
                                     void* signal_context) {
#if XE_ARCH_ARM64
  // On ARM64, ucontext_t requires 16-byte alignment but the kernel may provide
  // an unaligned pointer in the signal handler. Copy to an aligned local to
  // avoid undefined behavior from misaligned access.
  alignas(16) ucontext_t ucontext_storage;
  std::memcpy(&ucontext_storage, signal_context, sizeof(ucontext_t));
  ucontext_t* ucontext = &ucontext_storage;
#else
  ucontext_t* ucontext = reinterpret_cast<ucontext_t*>(signal_context);
#endif
  mcontext_t& mcontext = ucontext->uc_mcontext;

  HostThreadContext thread_context;

#if XE_ARCH_ARM64
  // Extract the general-purpose registers from mcontext
  thread_context.pc = mcontext->__ss.__pc;
  thread_context.sp = mcontext->__ss.__sp;
  thread_context.pstate = mcontext->__ss.__cpsr;

  // x0 - x28
  for (int i = 0; i < 29; ++i) {
    thread_context.x[i] = mcontext->__ss.__x[i];
  }
  // x29 is the frame pointer
  thread_context.x[29] = mcontext->__ss.__fp;
  // x30 is the link register
  thread_context.x[30] = mcontext->__ss.__lr;

  // Copy the vector registers
  for (int i = 0; i < 32; ++i) {
    thread_context.v[i].low =
        static_cast<uint64_t>(mcontext->__ns.__v[i] & 0xFFFFFFFFFFFFFFFF);
    thread_context.v[i].high = static_cast<uint64_t>(
        (mcontext->__ns.__v[i] >> 64) & 0xFFFFFFFFFFFFFFFF);
  }

  thread_context.fpsr = mcontext->__ns.__fpsr;
  thread_context.fpcr = mcontext->__ns.__fpcr;
#elif XE_ARCH_AMD64
  thread_context.rip = mcontext->__ss.__rip;
  thread_context.eflags = static_cast<uint32_t>(mcontext->__ss.__rflags);
  thread_context.rax = mcontext->__ss.__rax;
  thread_context.rcx = mcontext->__ss.__rcx;
  thread_context.rdx = mcontext->__ss.__rdx;
  thread_context.rbx = mcontext->__ss.__rbx;
  thread_context.rsp = mcontext->__ss.__rsp;
  thread_context.rbp = mcontext->__ss.__rbp;
  thread_context.rsi = mcontext->__ss.__rsi;
  thread_context.rdi = mcontext->__ss.__rdi;
  thread_context.r8 = mcontext->__ss.__r8;
  thread_context.r9 = mcontext->__ss.__r9;
  thread_context.r10 = mcontext->__ss.__r10;
  thread_context.r11 = mcontext->__ss.__r11;
  thread_context.r12 = mcontext->__ss.__r12;
  thread_context.r13 = mcontext->__ss.__r13;
  thread_context.r14 = mcontext->__ss.__r14;
  thread_context.r15 = mcontext->__ss.__r15;
#endif  // XE_ARCH

  Exception ex;
  switch (signal_number) {
    case SIGILL:
    case SIGTRAP:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case SIGBUS:
    case SIGSEGV: {
      Exception::AccessViolationOperation access_violation_operation =
          Exception::AccessViolationOperation::kUnknown;
#if XE_ARCH_ARM64
      // For a Data Abort (EC - ESR_EL1 bits 31:26 - 0b100100 from a lower
      // Exception Level, 0b100101 without a change in the Exception Level),
      // bit 6 is 0 for reading from a memory location, 1 for writing to a
      // memory location.
      const uint64_t esr = mcontext->__es.__esr;
      if (((esr >> 26) & 0b111110) == 0b100100) {
        access_violation_operation =
            (esr & (UINT64_C(1) << 6))
                ? Exception::AccessViolationOperation::kWrite
                : Exception::AccessViolationOperation::kRead;
      }
#elif XE_ARCH_AMD64
      // x86 page fault error code: bit 1 set indicates write.
      constexpr uint64_t kX86PageFaultErrorCodeWrite = UINT64_C(1) << 1;
      access_violation_operation =
          (uint64_t(mcontext->__es.__err) & kX86PageFaultErrorCodeWrite)
              ? Exception::AccessViolationOperation::kWrite
              : Exception::AccessViolationOperation::kRead;
#endif  // XE_ARCH

      ex.InitializeAccessViolation(
          &thread_context, reinterpret_cast<uint64_t>(signal_info->si_addr),
          access_violation_operation);
    } break;
    default:
      assert_unhandled_case(signal_number);
  }

  for (size_t i = 0; i < xe::countof(handlers_) && handlers_[i].first; ++i) {
    if (handlers_[i].first(&ex, handlers_[i].second)) {
      // Exception handled.
#if XE_ARCH_ARM64
      // Write back the modified registers to mcontext
      mcontext->__ss.__pc = thread_context.pc;
      mcontext->__ss.__sp = thread_context.sp;
      mcontext->__ss.__cpsr = (__uint32_t)thread_context.pstate;

      uint32_t modified_register_index;
      uint32_t modified_x_registers_remaining = ex.modified_x_registers();
      while (xe::bit_scan_forward(modified_x_registers_remaining,
                                  &modified_register_index)) {
        modified_x_registers_remaining &=
            ~(UINT32_C(1) << modified_register_index);
        if (modified_register_index < 29) {
          mcontext->__ss.__x[modified_register_index] =
              thread_context.x[modified_register_index];
        } else if (modified_register_index == 29) {
          mcontext->__ss.__fp = thread_context.x[29];
        } else if (modified_register_index == 30) {
          mcontext->__ss.__lr = thread_context.x[30];
        }
      }

      if (ex.modified_v_registers()) {
        uint32_t modified_v_register_index;
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (xe::bit_scan_forward(modified_v_registers_remaining,
                                    &modified_v_register_index)) {
          modified_v_registers_remaining &=
              ~(UINT32_C(1) << modified_v_register_index);
          mcontext->__ns.__v[modified_v_register_index] =
              (static_cast<__uint128_t>(
                   thread_context.v[modified_v_register_index].high)
               << 64) |
              static_cast<__uint128_t>(
                  thread_context.v[modified_v_register_index].low);
        }
        mcontext->__ns.__fpsr = thread_context.fpsr;
        mcontext->__ns.__fpcr = thread_context.fpcr;
      }
      // Copy modified context back to the original signal context
      std::memcpy(signal_context, ucontext, sizeof(ucontext_t));
#elif XE_ARCH_AMD64
      mcontext->__ss.__rip = thread_context.rip;
      mcontext->__ss.__rflags = thread_context.eflags;
      uint32_t modified_register_index;
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      uint64_t* kIntRegisterMap[] = {
          &mcontext->__ss.__rax, &mcontext->__ss.__rcx, &mcontext->__ss.__rdx,
          &mcontext->__ss.__rbx, &mcontext->__ss.__rsp, &mcontext->__ss.__rbp,
          &mcontext->__ss.__rsi, &mcontext->__ss.__rdi, &mcontext->__ss.__r8,
          &mcontext->__ss.__r9,  &mcontext->__ss.__r10, &mcontext->__ss.__r11,
          &mcontext->__ss.__r12, &mcontext->__ss.__r13, &mcontext->__ss.__r14,
          &mcontext->__ss.__r15,
      };
      while (xe::bit_scan_forward(modified_int_registers_remaining,
                                  &modified_register_index)) {
        modified_int_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        *kIntRegisterMap[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
#endif  // XE_ARCH
      return;
    }
  }
}

void ExceptionHandler::Install(Handler fn, void* data) {
  if (!signal_handlers_installed_) {
    struct sigaction signal_handler;

    std::memset(&signal_handler, 0, sizeof(signal_handler));
    signal_handler.sa_sigaction = ExceptionHandlerCallback;
    signal_handler.sa_flags = SA_SIGINFO;

    if (sigaction(SIGILL, &signal_handler, &original_sigill_handler_) != 0) {
      assert_always("Failed to install new SIGILL handler");
    }
    if (sigaction(SIGTRAP, &signal_handler, &original_sigtrap_handler_) != 0) {
      assert_always("Failed to install new SIGTRAP handler");
    }
    if (sigaction(SIGBUS, &signal_handler, &original_sigbus_handler_) != 0) {
      assert_always("Failed to install new SIGBUS handler");
    }
    if (sigaction(SIGSEGV, &signal_handler, &original_sigsegv_handler_) != 0) {
      assert_always("Failed to install new SIGSEGV handler");
    }
    signal_handlers_installed_ = true;
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
    if (signal_handlers_installed_) {
      if (sigaction(SIGILL, &original_sigill_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGILL handler");
      }
      if (sigaction(SIGTRAP, &original_sigtrap_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGTRAP handler");
      }
      if (sigaction(SIGBUS, &original_sigbus_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGBUS handler");
      }
      if (sigaction(SIGSEGV, &original_sigsegv_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGSEGV handler");
      }
      signal_handlers_installed_ = false;
    }
  }
}

}  // namespace xe
