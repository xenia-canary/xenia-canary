/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/exception_handler.h"

#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "xenia/base/assert.h"
#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"

namespace xe {

bool signal_handlers_installed_ = false;
struct sigaction original_sigill_handler_;
struct sigaction original_sigsegv_handler_;
struct sigaction original_sigbus_handler_;

// This can be as large as needed, but isn't often needed.
// As we will be sometimes firing many exceptions we want to avoid having to
// scan the table too much or invoke many custom handlers.
constexpr size_t kMaxHandlerCount = 8;

// All custom handlers, left-aligned and null terminated.
// Executed in order.
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

static void ExceptionHandlerCallback(int signal_number, siginfo_t* signal_info,
                                     void* signal_context) {
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  // The Darwin kernel may pass an unaligned ucontext_t pointer to signal
  // handlers; copy into an aligned local before reading. mcontext_t is a
  // pointer on Mac, so writes still reach kernel storage via the pointer.
  alignas(16) ucontext_t ucontext_storage;
  std::memcpy(&ucontext_storage, signal_context, sizeof(ucontext_t));
  mcontext_t& mcontext = ucontext_storage.uc_mcontext;
#else
  mcontext_t& mcontext =
      reinterpret_cast<ucontext_t*>(signal_context)->uc_mcontext;
#endif

  HostThreadContext thread_context;

#if XE_ARCH_AMD64
#if XE_PLATFORM_MAC
  // Darwin: mcontext is a pointer; integer state in __ss, FP/XMM in __fs.
  // __fpu_xmm0..__fpu_xmm15 are laid out contiguously in
  // __darwin_x86_float_state64.
  thread_context.rip = mcontext->__ss.__rip;
  thread_context.eflags = uint32_t(mcontext->__ss.__rflags);
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
  std::memcpy(thread_context.xmm_registers, &mcontext->__fs.__fpu_xmm0,
              sizeof(thread_context.xmm_registers));
#else
  thread_context.rip = uint64_t(mcontext.gregs[REG_RIP]);
  thread_context.eflags = uint32_t(mcontext.gregs[REG_EFL]);
  // The REG_ order may be different than the register indices in the
  // instruction encoding.
  thread_context.rax = uint64_t(mcontext.gregs[REG_RAX]);
  thread_context.rcx = uint64_t(mcontext.gregs[REG_RCX]);
  thread_context.rdx = uint64_t(mcontext.gregs[REG_RDX]);
  thread_context.rbx = uint64_t(mcontext.gregs[REG_RBX]);
  thread_context.rsp = uint64_t(mcontext.gregs[REG_RSP]);
  thread_context.rbp = uint64_t(mcontext.gregs[REG_RBP]);
  thread_context.rsi = uint64_t(mcontext.gregs[REG_RSI]);
  thread_context.rdi = uint64_t(mcontext.gregs[REG_RDI]);
  thread_context.r8 = uint64_t(mcontext.gregs[REG_R8]);
  thread_context.r9 = uint64_t(mcontext.gregs[REG_R9]);
  thread_context.r10 = uint64_t(mcontext.gregs[REG_R10]);
  thread_context.r11 = uint64_t(mcontext.gregs[REG_R11]);
  thread_context.r12 = uint64_t(mcontext.gregs[REG_R12]);
  thread_context.r13 = uint64_t(mcontext.gregs[REG_R13]);
  thread_context.r14 = uint64_t(mcontext.gregs[REG_R14]);
  thread_context.r15 = uint64_t(mcontext.gregs[REG_R15]);
  std::memcpy(thread_context.xmm_registers, mcontext.fpregs->_xmm,
              sizeof(thread_context.xmm_registers));
#endif  // XE_PLATFORM_MAC
#elif XE_ARCH_ARM64
#if XE_PLATFORM_MAC
  // Darwin: mcontext is a pointer, registers in __ss and __ns.
  for (int i = 0; i < 29; ++i) {
    thread_context.x[i] = mcontext->__ss.__x[i];
  }
  thread_context.x[29] = mcontext->__ss.__fp;
  thread_context.x[30] = mcontext->__ss.__lr;
  thread_context.sp = mcontext->__ss.__sp;
  thread_context.pc = mcontext->__ss.__pc;
  thread_context.pstate = mcontext->__ss.__cpsr;
  thread_context.fpsr = mcontext->__ns.__fpsr;
  thread_context.fpcr = mcontext->__ns.__fpcr;
  std::memcpy(thread_context.v, mcontext->__ns.__v, sizeof(thread_context.v));
#else
  // Linux: mcontext is a struct with direct member access.
  std::memcpy(thread_context.x, mcontext.regs, sizeof(thread_context.x));
  thread_context.sp = mcontext.sp;
  thread_context.pc = mcontext.pc;
  thread_context.pstate = mcontext.pstate;
  struct fpsimd_context* mcontext_fpsimd = nullptr;
  struct esr_context* mcontext_esr = nullptr;
  for (struct _aarch64_ctx* mcontext_extension =
           reinterpret_cast<struct _aarch64_ctx*>(mcontext.__reserved);
       mcontext_extension->magic;
       mcontext_extension = reinterpret_cast<struct _aarch64_ctx*>(
           reinterpret_cast<uint8_t*>(mcontext_extension) +
           mcontext_extension->size)) {
    switch (mcontext_extension->magic) {
      case FPSIMD_MAGIC:
        mcontext_fpsimd =
            reinterpret_cast<struct fpsimd_context*>(mcontext_extension);
        break;
      case ESR_MAGIC:
        mcontext_esr =
            reinterpret_cast<struct esr_context*>(mcontext_extension);
        break;
      default:
        break;
    }
  }
  assert_not_null(mcontext_fpsimd);
  if (mcontext_fpsimd) {
    thread_context.fpsr = mcontext_fpsimd->fpsr;
    thread_context.fpcr = mcontext_fpsimd->fpcr;
    std::memcpy(thread_context.v, mcontext_fpsimd->vregs,
                sizeof(thread_context.v));
  }
#endif  // XE_PLATFORM_MAC
#endif  // XE_ARCH

  Exception ex;
  switch (signal_number) {
    case SIGILL:
      ex.InitializeIllegalInstruction(&thread_context);
      break;
    case SIGBUS:
    case SIGSEGV: {
      Exception::AccessViolationOperation access_violation_operation;
#if XE_ARCH_AMD64
      // x86_pf_error_code::X86_PF_WRITE
      constexpr uint64_t kX86PageFaultErrorCodeWrite = UINT64_C(1) << 1;
#if XE_PLATFORM_MAC
      access_violation_operation =
          (uint64_t(mcontext->__es.__err) & kX86PageFaultErrorCodeWrite)
              ? Exception::AccessViolationOperation::kWrite
              : Exception::AccessViolationOperation::kRead;
#else
      access_violation_operation =
          (uint64_t(mcontext.gregs[REG_ERR]) & kX86PageFaultErrorCodeWrite)
              ? Exception::AccessViolationOperation::kWrite
              : Exception::AccessViolationOperation::kRead;
#endif
#elif XE_ARCH_ARM64
#if XE_PLATFORM_MAC
      {
        // On Darwin, determine access direction from the faulting instruction.
        uint64_t fault_pc = mcontext->__ss.__pc;
        uint32_t fault_insn = *reinterpret_cast<const uint32_t*>(fault_pc);
        bool instruction_is_store;
        if (IsArm64LoadPrefetchStore(fault_insn, instruction_is_store)) {
          access_violation_operation =
              instruction_is_store ? Exception::AccessViolationOperation::kWrite
                                   : Exception::AccessViolationOperation::kRead;
        } else {
          // Crash-safe diagnostic: write directly to stderr so the line
          // survives an unhandled fault. Rate-limited to avoid flooding.
          static std::atomic<int> unclassified_log_remaining{16};
          if (unclassified_log_remaining.fetch_sub(
                  1, std::memory_order_relaxed) > 0) {
            char buf[160];
            int n = std::snprintf(
                buf, sizeof(buf),
                "[arm64] unclassified fault: pc=0x%llx insn=0x%08x addr=%p\n",
                (unsigned long long)fault_pc, fault_insn, signal_info->si_addr);
            if (n > 0) {
              size_t to_write =
                  (n < int(sizeof(buf) - 1)) ? size_t(n) : sizeof(buf) - 1;
              (void)::write(STDERR_FILENO, buf, to_write);
            }
          }
          access_violation_operation =
              Exception::AccessViolationOperation::kUnknown;
        }
      }
#else
      // For a Data Abort (EC - ESR_EL1 bits 31:26 - 0b100100 from a lower
      // Exception Level, 0b100101 without a change in the Exception Level),
      // bit 6 is 0 for reading from a memory location, 1 for writing to a
      // memory location.
      if (mcontext_esr && ((mcontext_esr->esr >> 26) & 0b111110) == 0b100100) {
        access_violation_operation =
            (mcontext_esr->esr & (UINT64_C(1) << 6))
                ? Exception::AccessViolationOperation::kWrite
                : Exception::AccessViolationOperation::kRead;
      } else {
        // Determine the memory access direction based on which instruction has
        // requested it.
        // esr_context may be unavailable on certain hosts (for instance, on
        // Android, it was added only in NDK r16 - which is the first NDK
        // version to support the Android API level 27, while NDK r15 doesn't
        // have esr_context in its API 26 sigcontext.h).
        // On AArch64 (unlike on AArch32), the program counter is the address of
        // the currently executing instruction.
        bool instruction_is_store;
        if (IsArm64LoadPrefetchStore(
                *reinterpret_cast<const uint32_t*>(mcontext.pc),
                instruction_is_store)) {
          access_violation_operation =
              instruction_is_store ? Exception::AccessViolationOperation::kWrite
                                   : Exception::AccessViolationOperation::kRead;
        } else {
          assert_always(
              "No ESR in the exception thread context, or it's not a Data "
              "Abort, and the faulting instruction is not a known load, "
              "prefetch or store instruction");
          access_violation_operation =
              Exception::AccessViolationOperation::kUnknown;
        }
      }
#endif  // XE_PLATFORM_MAC
#else
      access_violation_operation =
          Exception::AccessViolationOperation::kUnknown;
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
#if XE_ARCH_AMD64
      uint32_t modified_register_index;
#if XE_PLATFORM_MAC
      mcontext->__ss.__rip = thread_context.rip;
      mcontext->__ss.__rflags = thread_context.eflags;
      // Pointer-to-member map; order must match X64Register.
      using GprPtr = __uint64_t __darwin_x86_thread_state64::*;
      static constexpr GprPtr kIntRegisterMap[] = {
          &__darwin_x86_thread_state64::__rax,
          &__darwin_x86_thread_state64::__rcx,
          &__darwin_x86_thread_state64::__rdx,
          &__darwin_x86_thread_state64::__rbx,
          &__darwin_x86_thread_state64::__rsp,
          &__darwin_x86_thread_state64::__rbp,
          &__darwin_x86_thread_state64::__rsi,
          &__darwin_x86_thread_state64::__rdi,
          &__darwin_x86_thread_state64::__r8,
          &__darwin_x86_thread_state64::__r9,
          &__darwin_x86_thread_state64::__r10,
          &__darwin_x86_thread_state64::__r11,
          &__darwin_x86_thread_state64::__r12,
          &__darwin_x86_thread_state64::__r13,
          &__darwin_x86_thread_state64::__r14,
          &__darwin_x86_thread_state64::__r15,
      };
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (xe::bit_scan_forward(modified_int_registers_remaining,
                                  &modified_register_index)) {
        modified_int_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        mcontext->__ss.*kIntRegisterMap[modified_register_index] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (xe::bit_scan_forward(modified_xmm_registers_remaining,
                                  &modified_register_index)) {
        modified_xmm_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        std::memcpy(reinterpret_cast<uint8_t*>(&mcontext->__fs.__fpu_xmm0) +
                        modified_register_index * sizeof(vec128_t),
                    &thread_context.xmm_registers[modified_register_index],
                    sizeof(vec128_t));
      }
#else
      mcontext.gregs[REG_RIP] = greg_t(thread_context.rip);
      mcontext.gregs[REG_EFL] = greg_t(thread_context.eflags);
      // The order must match the order in X64Register.
      static constexpr size_t kIntRegisterMap[] = {
          REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
          REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
          REG_R12, REG_R13, REG_R14, REG_R15,
      };
      uint16_t modified_int_registers_remaining = ex.modified_int_registers();
      while (xe::bit_scan_forward(modified_int_registers_remaining,
                                  &modified_register_index)) {
        modified_int_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        mcontext.gregs[kIntRegisterMap[modified_register_index]] =
            thread_context.int_registers[modified_register_index];
      }
      uint16_t modified_xmm_registers_remaining = ex.modified_xmm_registers();
      while (xe::bit_scan_forward(modified_xmm_registers_remaining,
                                  &modified_register_index)) {
        modified_xmm_registers_remaining &=
            ~(UINT16_C(1) << modified_register_index);
        std::memcpy(&mcontext.fpregs->_xmm[modified_register_index],
                    &thread_context.xmm_registers[modified_register_index],
                    sizeof(vec128_t));
      }
#endif  // XE_PLATFORM_MAC
#elif XE_ARCH_ARM64
      uint32_t modified_register_index;
#if XE_PLATFORM_MAC
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
      mcontext->__ss.__sp = thread_context.sp;
      mcontext->__ss.__pc = thread_context.pc;
      mcontext->__ss.__cpsr = thread_context.pstate;
      mcontext->__ns.__fpsr = thread_context.fpsr;
      mcontext->__ns.__fpcr = thread_context.fpcr;
      uint32_t modified_v_registers_remaining = ex.modified_v_registers();
      while (xe::bit_scan_forward(modified_v_registers_remaining,
                                  &modified_register_index)) {
        modified_v_registers_remaining &=
            ~(UINT32_C(1) << modified_register_index);
        std::memcpy(&mcontext->__ns.__v[modified_register_index],
                    &thread_context.v[modified_register_index],
                    sizeof(vec128_t));
      }
#else
      uint32_t modified_x_registers_remaining = ex.modified_x_registers();
      while (xe::bit_scan_forward(modified_x_registers_remaining,
                                  &modified_register_index)) {
        modified_x_registers_remaining &=
            ~(UINT32_C(1) << modified_register_index);
        mcontext.regs[modified_register_index] =
            thread_context.x[modified_register_index];
      }
      mcontext.sp = thread_context.sp;
      mcontext.pc = thread_context.pc;
      mcontext.pstate = thread_context.pstate;
      if (mcontext_fpsimd) {
        mcontext_fpsimd->fpsr = thread_context.fpsr;
        mcontext_fpsimd->fpcr = thread_context.fpcr;
        uint32_t modified_v_registers_remaining = ex.modified_v_registers();
        while (xe::bit_scan_forward(modified_v_registers_remaining,
                                    &modified_register_index)) {
          modified_v_registers_remaining &=
              ~(UINT32_C(1) << modified_register_index);
          std::memcpy(&mcontext_fpsimd->vregs[modified_register_index],
                      &thread_context.v[modified_register_index],
                      sizeof(vec128_t));
          mcontext.regs[modified_register_index] =
              thread_context.x[modified_register_index];
        }
      }
#endif  // XE_PLATFORM_MAC
#endif  // XE_ARCH
      return;
    }
  }

  // Unhandled: restore the original disposition so the kernel re-delivers
  // the signal to it on instruction retry, otherwise we loop forever.
  struct sigaction* original_handler = nullptr;
  switch (signal_number) {
    case SIGSEGV:
      original_handler = &original_sigsegv_handler_;
      break;
    case SIGBUS:
      original_handler = &original_sigbus_handler_;
      break;
    case SIGILL:
      original_handler = &original_sigill_handler_;
      break;
  }
  if (original_handler) {
    sigaction(signal_number, original_handler, nullptr);
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
    if (sigaction(SIGSEGV, &signal_handler, &original_sigsegv_handler_) != 0) {
      assert_always("Failed to install new SIGSEGV handler");
    }
    if (sigaction(SIGBUS, &signal_handler, &original_sigbus_handler_) != 0) {
      assert_always("Failed to install new SIGBUS handler");
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
      if (sigaction(SIGSEGV, &original_sigsegv_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGSEGV handler");
      }
      if (sigaction(SIGBUS, &original_sigbus_handler_, NULL) != 0) {
        assert_always("Failed to restore original SIGBUS handler");
      }
      signal_handlers_installed_ = false;
    }
  }
}

}  // namespace xe
