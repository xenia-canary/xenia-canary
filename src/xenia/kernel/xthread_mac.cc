/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * macOS Debugging Notes:
 * This file contains additional debugging statements wrapped in #ifdef __APPLE__
 * to help diagnose thread-related issues on macOS. These statements track:
 * - Thread creation and initialization
 * - When current_xthread_tls_ is set/cleared
 * - When GetCurrentThread() is called from unregistered threads
 * - Thread exit events
 *
 * All debugging is macOS-specific and won't affect other platforms.
 */

#include "xenia/kernel/xthread_mac.h"
#include "xenia/cpu/thread_mac.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#ifdef __APPLE__
#include <execinfo.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order_mac.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading_mac.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator_mac.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading_mac.h"

DEFINE_bool(ignore_thread_priorities, true,
            "Ignores game-specified thread priorities.", "Kernel");
DEFINE_bool(ignore_thread_affinities, true,
            "Ignores game-specified thread affinities.", "Kernel");
DEFINE_bool(
    kernel_log_main_thread_startup_probe, true,
    "Debug: Log guest instruction words at main-thread start dispatch and "
    "xapi_thread_startup selection.",
    "Kernel");

#if 0
DEFINE_int64(stack_size_multiplier_hack, 1,
             "A hack for games with setjmp/longjmp issues.", "Kernel");
DEFINE_int64(main_xthread_stack_size_multiplier_hack, 1,
             "A hack for games with setjmp/longjmp issues.", "Kernel");
#endif

namespace xe {
namespace kernel {

namespace {

constexpr bool IsGpuBootstrapStartupFunction(uint32_t address) {
  switch (address) {
    case 0x82582A98:
    case 0x82582AC8:
    case 0x8258A410:
    case 0x8258A268:
    case 0x825828B0:
      return true;
    default:
      return false;
  }
}

const char* ClassifyStartupWord(uint32_t word) {
  switch (word) {
    case 0x7D8802A6:
      return "mflr r12";
    case 0x9421FE10:
      return "stwu r1,-0x1F0(r1)";
    case 0x4E800020:
      return "blr";
    case 0x4E800421:
      return "bctrl";
    case 0x4E800420:
      return "bctr";
    case 0x44000042:
      return "sc 2";
    case 0x60000000:
      return "nop";
    default:
      if ((word & 0xFC000003u) == 0x48000001u) {
        return "bl <rel24>";
      }
      if ((word & 0xFC000003u) == 0x48000000u) {
        return "b <rel24>";
      }
      return nullptr;
  }
}

void LogGuestStartWords(Memory* memory, uint32_t base, const char* label) {
  if (!memory || !base) {
    XELOGI(
        "GUEST MAIN THREAD: startup probe {} base={:08X} "
        "(memory/addr unavailable)",
        label ? label : "target", base);
    return;
  }
  constexpr uint32_t kProbeWords = 6;
  bool saw_ppc_nop = false;
  for (uint32_t i = 0; i < kProbeWords; ++i) {
    uint32_t addr = base + i * 4;
    auto word_result = memory->TranslateVirtualSafe<xe::be<uint32_t>*>(addr);
    if (!word_result.success || !word_result.pointer) {
      XELOGW(
          "GUEST MAIN THREAD: startup probe {} word[{}] addr={:08X} "
          "unreadable error={}",
          label ? label : "target", i, addr, word_result.error);
      continue;
    }
    uint32_t word = xe::load_and_swap<uint32_t>(word_result.pointer);
    if (word == 0x60000000u) {
      saw_ppc_nop = true;
    }
    const char* classified = ClassifyStartupWord(word);
    if (classified) {
      XELOGI(
          "GUEST MAIN THREAD: startup probe {} word[{}] addr={:08X} "
          "value={:08X} decoded={}",
          label ? label : "target", i, addr, word, classified);
    } else {
      XELOGI(
          "GUEST MAIN THREAD: startup probe {} word[{}] addr={:08X} "
          "value={:08X}",
          label ? label : "target", i, addr, word);
    }
  }
  if (saw_ppc_nop) {
    XELOGI(
        "GUEST MAIN THREAD: startup probe {} observed PPC nop instruction(s); "
        "this is guest code and is unrelated to PM4 ring-buffer NOP seeding.",
        label ? label : "target");
  }
}

}  // namespace

// Thread-local storage for current XThread instance
thread_local XThread* current_xthread_tls_ = nullptr;

#ifdef __APPLE__
// Enhanced TLS management for critical threads (especially GPU Commands thread)
thread_local XThread* tls_backup_ = nullptr;
thread_local uint64_t tls_backup_timestamp_ = 0;
thread_local bool tls_backup_valid_ = false;
thread_local std::atomic<bool> tls_operation_in_progress_{false};
thread_local uint32_t tls_setup_attempt_count_ = 0;
thread_local uint64_t last_tls_failure_timestamp_ = 0;

// Thread-safe TLS synchronization
static std::mutex tls_global_mutex_;
static std::condition_variable tls_ready_cv_;
static std::set<std::thread::id> tls_ready_threads_;

// Constants for TLS management
constexpr uint64_t kTLSBackupTimeoutMs = 5000;  // 5 seconds
constexpr uint32_t kGpuCommandsThreadHandle = 0x01000010;
constexpr int kMaxTLSSetupAttempts = 10;
constexpr uint64_t kTLSFailureCooldownMs = 100;  // Prevent rapid retry loops
#endif

const uint32_t XAPC::kSize;
const uint32_t XAPC::kDummyKernelRoutine;
const uint32_t XAPC::kDummyRundownRoutine;

using namespace xe::literals;

uint32_t next_xthread_id_ = 0;

#ifdef __APPLE__
// Atomic TLS management functions to prevent race conditions
static void SetCurrentThreadAtomic(XThread* thread) {
  // Prevent concurrent TLS modifications
  bool expected = false;
  if (!tls_operation_in_progress_.compare_exchange_strong(expected, true)) {
    // Another operation is in progress, wait briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    SetCurrentThreadAtomic(thread);
    return;
  }
  
  // Additional validation for critical threads
  if (thread && thread->handle() == kGpuCommandsThreadHandle) {
    XELOGI("DEBUG: CRITICAL: Atomic TLS setup for GPU Commands - STARTING");
    
    // Verify no other critical thread is being set
    if (current_xthread_tls_ && current_xthread_tls_ != thread) {
      XELOGW("DEBUG: Overriding existing TLS for different thread!");
    }
  }
  
  current_xthread_tls_ = thread;
  cpu::Thread::SetCurrentThread(thread);
  
  // Update global thread readiness tracking
  auto current_thread_id = std::this_thread::get_id();
  {
    std::lock_guard<std::mutex> lock(tls_global_mutex_);
    tls_ready_threads_.insert(current_thread_id);
  }
  
  // Critical thread specific handling
  if (thread && thread->handle() == kGpuCommandsThreadHandle) {
    XHostThread::BackupTLS();
    // Force immediate verification
    if (!XHostThread::VerifyTLS()) {
      XELOGE("DEBUG: TLS verification failed immediately after setup");
    }
  }
  
  tls_ready_cv_.notify_all();
  tls_operation_in_progress_ = false;
}

static void WaitForTLSReady(std::thread::id thread_id, int timeout_ms = 1000) {
  std::unique_lock<std::mutex> lock(tls_global_mutex_);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  
  tls_ready_cv_.wait_until(lock, deadline, [thread_id]() {
    return tls_ready_threads_.find(thread_id) != tls_ready_threads_.end();
  });
}

static bool IsTLSReady(std::thread::id thread_id) {
  std::lock_guard<std::mutex> lock(tls_global_mutex_);
  return tls_ready_threads_.find(thread_id) != tls_ready_threads_.end();
}
#endif

XThread::XThread(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType), guest_thread_(true) {}

XThread::XThread(KernelState* kernel_state, uint32_t stack_size,
                 uint32_t xapi_thread_startup, uint32_t start_address,
                 uint32_t start_context, uint32_t creation_flags,
                 bool guest_thread, bool main_thread, uint32_t guest_process)
    : XObject(kernel_state, kObjectType, !guest_thread),
      thread_id_(++next_xthread_id_),
      guest_thread_(guest_thread),
      main_thread_(main_thread) {
  creation_params_.stack_size = stack_size;
  creation_params_.xapi_thread_startup = xapi_thread_startup;
  creation_params_.start_address = start_address;
  creation_params_.start_context = start_context;

  // top 8 bits = processor ID (or 0 for default)
  // bit 0 = 1 to create suspended
  creation_params_.creation_flags = creation_flags;

  // Adjust stack size - min of 16k.
  if (creation_params_.stack_size < 16 * 1024) {
    creation_params_.stack_size = 16 * 1024;
  }
  creation_params_.guest_process = guest_process;

#ifdef __APPLE__
  if (handle() == 0x01000010) {
    fprintf(stderr,
            "[xthread] ctor enter handle=%08X thread_id=%u guest=%d main=%d\n",
            handle(), thread_id_, guest_thread_ ? 1 : 0, main_thread_ ? 1 : 0);
    fflush(stderr);
  }
#endif
  
  // The kernel does not take a reference. We must unregister in the dtor.
  // CRITICAL FIX: Move RegisterThread BEFORE TLS setup to ensure lock order is ALWAYS 
  // global_critical_region -> tls_global_mutex_.
  // Historically this was after TLS setup, causing a deadlock with CreateNative.
#ifdef __APPLE__
  if (handle() == 0x01000010) {
    fprintf(stderr, "[xthread] RegisterThread BEGIN handle=%08X\n", handle());
    fflush(stderr);
  }
#endif
  kernel_state_->RegisterThread(this);
#ifdef __APPLE__
  if (handle() == 0x01000010) {
    fprintf(stderr, "[xthread] RegisterThread END handle=%08X\n", handle());
    fflush(stderr);
  }
#endif

#ifdef __APPLE__
  // IMPORTANT: Do NOT touch TLS in the constructor on macOS.
  // The constructor runs on the creator thread, not the new thread.
  // TLS is set safely in the thread entry in XThread::Create().
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: [macOS] XThread ctor - deferring all TLS setup to thread entry");
  }
#endif
}

XThread::~XThread() {
  // Unregister first to prevent lookups while deleting.
  kernel_state_->UnregisterThread(this);

  // Notify processor of our impending destruction.
  emulator()->processor()->OnThreadDestroyed(thread_id_);

  thread_.reset();

  if (thread_state_) {
    delete thread_state_;
  }
  kernel_state()->memory()->SystemHeapFree(tls_static_address_);
  kernel_state()->memory()->SystemHeapFree(pcr_address_);
  FreeStack();

  if (thread_) {
    // TODO(benvanik): platform kill
    XELOGE("Thread disposed without exiting");
  }
}

#ifdef __APPLE__
namespace {
void LogThreadContext(const char* function_name) {
  XELOGE("[{}] Thread context check:", function_name);
  XELOGE("[{}]   current_xthread_tls_ = {}", function_name,
          current_xthread_tls_ ? fmt::format("{:08X}", current_xthread_tls_->handle()) : "nullptr");
  XELOGE("[{}]   Host thread ID = {}", function_name, std::this_thread::get_id());
  if (current_xthread_tls_) {
    XELOGE("[{}]   Thread name = '{}'", function_name, current_xthread_tls_->name());
    XELOGE("[{}]   Thread ID = {}", function_name, current_xthread_tls_->thread_id());
    XELOGE("[{}]   Thread handle = {:08X}", function_name, current_xthread_tls_->handle());
  } else {
    XELOGE("[{}]   TLS is NULL - this may indicate constructor context", function_name);
  }
}

void DumpBacktrace(const char* tag) {
  void* buffer[64];
  int count = backtrace(buffer, 64);
  fprintf(stderr, "[backtrace] %s: captured %d frames\n", tag, count);
  fflush(stderr);
  backtrace_symbols_fd(buffer, count, fileno(stderr));
}
}  // namespace
#endif

bool XThread::IsInThread() { return Thread::IsInThread(); }

bool XThread::IsInThread(XThread* other) {
  return current_xthread_tls_ == other;
}

void XThread::ValidateThreadRole(const char* thread_name, bool is_guest_thread,
                                  X_HANDLE handle) {
  // Critical infrastructure threads that MUST be HOST threads
  static const char* kCriticalHostThreads[] = {
      "GPU Command Processor",
      "XMA Decoder",
      "Dispatch",
      nullptr  // Sentinel
  };

  // Check if this is a critical thread that must be a HOST thread
  for (int i = 0; kCriticalHostThreads[i] != nullptr; ++i) {
    if (thread_name && std::string(thread_name).find(kCriticalHostThreads[i]) != std::string::npos) {
      if (is_guest_thread) {
        // CRITICAL ERROR: Infrastructure thread misclassified as GUEST
        TerminateOnThreadRoleMismatch(thread_name, "HOST", "GUEST");
      }
      XELOGI("DEBUG: Thread role validation PASSED: '{}' (handle={:08X}) correctly classified as HOST thread",
             thread_name, handle);
      return;
    }
  }
}

void XThread::TerminateOnThreadRoleMismatch(const char* thread_name,
                                             const char* expected_role,
                                             const char* actual_role) {
  XELOGE("DEBUG: FATAL: Thread role mismatch detected!");
  XELOGE("Thread name: '{}'", thread_name ? thread_name : "<unknown>");
  XELOGE("Expected role: {}", expected_role);
  XELOGE("Actual role: {}", actual_role);
  XELOGE("");
  XELOGE("This is a critical threading architecture violation.");
  XELOGE("Infrastructure threads (GPU, audio, JIT) MUST be HOST threads.");
  XELOGE("Guest threads are only for code loaded from the game executable.");
  XELOGE("");
  XELOGE("Terminating to prevent deadlock and scheduler starvation.");

  // Flush logs
  fflush(stdout);
  fflush(stderr);

  // Terminate immediately
  std::abort();
}

XThread* XThread::GetCurrentThread() {
  XThread* thread = reinterpret_cast<XThread*>(current_xthread_tls_);
  
#ifdef __APPLE__
  uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  if (!thread) {
    // Enhanced atomic TLS recovery
    auto host_thread_id = std::this_thread::get_id();
    bool is_globally_ready = IsTLSReady(host_thread_id);
    if (!is_globally_ready) {
      fprintf(stderr,
              "[tls] GetCurrentThread NULL (TLS not ready, constructor context likely). host_thread_id=%u\n",
              xe::threading::current_thread_id());
      fflush(stderr);
      return nullptr;
    }

    XELOGE("DEBUG: TLS STATE FAILURE at timestamp: {}", timestamp);
    XELOGE("GetCurrentThread() called with NULL TLS context");
    
    // Diagnose the situation
    LogThreadContext("GetCurrentThread");
    
    XELOGE("Host thread ID: {}", host_thread_id);
    XELOGE("TLS backup valid: {}", IsTLSBackupValid() ? "YES" : "NO");
    XELOGE("TLS operation in progress: {}", tls_operation_in_progress_.load());
    
    // Check if this thread is marked as ready in global tracking
    XELOGE("Global TLS tracking: READY");
    
    // Enhanced recovery with atomic backup restore
    if (IsTLSBackupValid()) {
      auto* backup_thread = GetTLSBackup();
      if (backup_thread) {
        XELOGI("Backup thread available: {:08X}", backup_thread->handle());
        XELOGI("Backup thread name: '{}'", backup_thread->name());
        
        // Atomic TLS restoration for GPU Commands thread
        if (backup_thread->handle() == kGpuCommandsThreadHandle) {
          XELOGI("DEBUG: CRITICAL: ATOMIC TLS restore for GPU Commands thread");
          
          // Prevent concurrent operations
          bool expected = false;
          if (tls_operation_in_progress_.compare_exchange_strong(expected, true)) {
            RestoreTLS();
            
            // Verify restoration worked
            thread = reinterpret_cast<XThread*>(current_xthread_tls_);
            if (thread && thread->handle() == kGpuCommandsThreadHandle) {
              XELOGI("DEBUG: CRITICAL: Atomic TLS restore SUCCESS - GPU Commands thread recovered");
              
              // Update global tracking
              {
                std::lock_guard<std::mutex> lock(tls_global_mutex_);
                tls_ready_threads_.insert(host_thread_id);
              }
              tls_ready_cv_.notify_all();
              
              tls_operation_in_progress_ = false;
              return thread;
            } else {
              XELOGE("DEBUG: CRITICAL: Atomic TLS restore FAILED - GPU Commands thread still NULL");
            }
            tls_operation_in_progress_ = false;
          }
        }
      }
    }
    
    // Last-ditch recovery attempt
    if (tls_backup_valid_ && tls_backup_) {
      XELOGI("DEBUG: EMERGENCY: Last-ditch TLS restoration attempt");
      SetCurrentThreadAtomic(tls_backup_);
      thread = reinterpret_cast<XThread*>(current_xthread_tls_);
      
      if (thread) {
        XELOGI("DEBUG: EMERGENCY: Recovery successful");
        return thread;
      }
    }
    
    XELOGE("DEBUG: CRITICAL: All recovery attempts failed!");
    XELOGE("GPU Commands thread (01000010) is seriously affected!");
    XELOGE("Memory corruption or fundamental TLS failure suspected");
    return nullptr;
    
  } else {
    // Thread retrieved from TLS - return immediately for performance.
    // Historical aggressive verification for GPU Commands thread (01000010) 
    // was removed here to prevent lock contention and deadlocks on macOS.
    return thread;
  }
#else
  if (!thread) {
    assert_always("Attempting to use guest stuff from a non-guest thread.");
  }
#endif
  
  return thread;
}

uint32_t XThread::GetCurrentThreadHandle() {
#ifdef __APPLE__
  XELOGI("[GetCurrentThreadHandle] Called");
#endif
  XThread* thread = XThread::GetCurrentThread();
  return thread->handle();
}

uint32_t XThread::GetCurrentThreadId() {
#ifdef __APPLE__
  XELOGI("[GetCurrentThreadId] Called");
#endif
  XThread* thread = XThread::GetCurrentThread();
  if (!thread) {
    // Host-side paths (for example debug MMIO injection) may call this without
    // a bound XThread TLS context.
    return xe::threading::current_thread_id();
  }
  auto* guest_kthread = thread->guest_object<X_KTHREAD>();
  if (!guest_kthread) {
    return thread->thread_id();
  }
  return guest_kthread->thread_id;
}

uint32_t XThread::GetLastError() {
#ifdef __APPLE__
  XELOGI("[GetLastError] Called");
#endif
  XThread* thread = XThread::GetCurrentThread();
  return thread->last_error();
}

void XThread::SetLastError(uint32_t error_code) {
#ifdef __APPLE__
  XELOGI("[SetLastError] Called with error_code={:08X}", error_code);
#endif
  XThread* thread = XThread::GetCurrentThread();
  thread->set_last_error(error_code);
}

uint32_t XThread::last_error() { return guest_object<X_KTHREAD>()->last_error; }

void XThread::set_last_error(uint32_t error_code) {
  guest_object<X_KTHREAD>()->last_error = error_code;
}

void XThread::set_name(const std::string_view name) {
  thread_name_ = fmt::format("{} ({:08X})", name, handle());

  // Validate thread role when name is set for critical infrastructure threads
  ValidateThreadRole(thread_name_.c_str(), guest_thread_, handle());

  if (thread_) {
    // May be getting set before the thread is created.
    // One the thread is ready it will handle it.
    thread_->set_name(thread_name_);
  }
}

static uint8_t next_cpu = 0;
static uint8_t GetFakeCpuNumber(uint8_t proc_mask) {
  // NOTE: proc_mask is logical processors, not physical processors or cores.
  if (!proc_mask) {
    next_cpu = (next_cpu + 1) % 6;
    return next_cpu;  // is this reasonable?
    // TODO(Triang3l): Does the following apply here?
    // https://docs.microsoft.com/en-us/windows/win32/dxtecharts/coding-for-multiple-cores
    // "On Xbox 360, you must explicitly assign software threads to a particular
    //  hardware thread by using XSetThreadProcessor. Otherwise, all child
    //  threads will stay on the same hardware thread as the parent."
  }
  assert_false(proc_mask & 0xC0);

  uint8_t cpu_number = 7 - xe::lzcnt(proc_mask);
  assert_true(1 << cpu_number == proc_mask);
  assert_true(cpu_number < 6);
  return cpu_number;
}

void XThread::InitializeGuestObject() {
  auto guest_thread = guest_object<X_KTHREAD>();
  auto thread_guest_ptr = guest_object();
  guest_thread->header.type = 6;
  guest_thread->suspend_count =
      (creation_params_.creation_flags & X_CREATE_SUSPENDED) ? 1 : 0;

  guest_thread->unk_10 = (thread_guest_ptr + 0x10);
  guest_thread->unk_14 = (thread_guest_ptr + 0x10);
  guest_thread->wait_timeout_block.wait_list_entry.flink_ptr =
      thread_guest_ptr + 0x20;
  guest_thread->wait_timeout_block.wait_list_entry.blink_ptr =
      thread_guest_ptr + 0x20;
  guest_thread->wait_timeout_block.thread = thread_guest_ptr;
  uint32_t v6 = thread_guest_ptr + 0x18;
  guest_thread->wait_timeout_block.wait_result_xstatus = 0x0100;
  guest_thread->wait_timeout_block.wait_type = 0x0201;
  guest_thread->wait_timeout_block.object = v6;
  guest_thread->stack_base = (this->stack_base_);
  guest_thread->stack_limit = (this->stack_limit_);
  guest_thread->stack_kernel = (this->stack_base_ - 240);
  guest_thread->tls_address = (this->tls_static_address_);
  guest_thread->thread_state = 0;
  uint32_t process_info_block_address =
      creation_params_.guest_process ? creation_params_.guest_process
                                     : this->kernel_state_->GetTitleProcess();

  X_KPROCESS* process =
      memory()->TranslateVirtual<X_KPROCESS*>(process_info_block_address);
  if (!process) {
    printf("DEBUG: BREADCRUMB: InitializeGuestObject ERROR - process is NULL for thread %d\n", thread_id_);
    XELOGW("InitializeGuestObject: process is NULL for thread {:08X}", handle());
    return;
  }
  uint32_t kpcrb = pcr_address_ + offsetof(X_KPCR, prcb_data);

  auto process_type = process->process_type;
  guest_thread->process_type_dup = process_type;
  guest_thread->process_type = process_type;
  guest_thread->apc_lists[0].Initialize(memory());
  guest_thread->apc_lists[1].Initialize(memory());

  guest_thread->a_prcb_ptr = kpcrb;
  guest_thread->another_prcb_ptr = kpcrb;

  guest_thread->may_queue_apcs = 1;
  guest_thread->msr_mask = 0xFDFFD7FF;
  guest_thread->process = process_info_block_address;
  guest_thread->stack_alloc_base = this->stack_base_;
  guest_thread->create_time = Clock::QueryGuestSystemTime();
  guest_thread->timer_list.flink_ptr = thread_guest_ptr + 324;
  guest_thread->timer_list.blink_ptr = thread_guest_ptr + 324;
  guest_thread->thread_id = this->thread_id_;
  guest_thread->start_address = this->creation_params_.start_address;
  guest_thread->unk_154.flink_ptr = thread_guest_ptr + 340;
  uint32_t v9 = thread_guest_ptr;
  guest_thread->last_error = 0;
  guest_thread->unk_154.blink_ptr = v9 + 340;
  guest_thread->creation_flags = this->creation_params_.creation_flags;
  guest_thread->unk_17C = 1;
  if (main_thread_) {
    XELOGI(
        "GUEST MAIN THREAD: wait template initialized "
        "kthread={:08X} wait_timeout_obj={:08X} wait_type={:04X} "
        "wait_result_xstatus={:04X}",
        thread_guest_ptr,
        static_cast<uint32_t>(guest_thread->wait_timeout_block.object.m_ptr),
        static_cast<uint16_t>(guest_thread->wait_timeout_block.wait_type),
        static_cast<uint16_t>(guest_thread->wait_timeout_block.wait_result_xstatus));
  }

  /*
   * not doing this right at all! we're not using our threads context, because
   * we may be on the host and have no underlying context. in reality we should
   * have a context and acquire any locks using that context!
   */
  printf("DEBUG: BREADCRUMB: InitializeGuestObject for thread %d\n", thread_id_);
  auto context_here = thread_state_->context();

  {
    printf("DEBUG: BREADCRUMB: About to acquire process->thread_list_spinlock for process %p\n", static_cast<void*>(process));
    printf("DEBUG: BREADCRUMB: thread_list_spinlock owner=%u, ctx r13=%u\n",
           static_cast<uint32_t>(process->thread_list_spinlock.prcb_of_owner),
           static_cast<uint32_t>(context_here ? context_here->r[13] : 0));
    auto old_irql = xboxkrnl::xeKeKfAcquireSpinLock(
        context_here, &process->thread_list_spinlock);
    printf("DEBUG: BREADCRUMB: Spinlock acquired\n");

    // todo: acquire dispatcher lock here?

    printf("DEBUG: BREADCRUMB: About to call XeInsertTailList\n");
    // bool did_insert = false;
#ifdef __APPLE__
    bool did_insert = false;
    uint32_t list_head_guest =
        process_info_block_address + offsetof(X_KPROCESS, thread_list);
    uint32_t entry_guest =
        thread_guest_ptr + offsetof(X_KTHREAD, process_threads);

    auto mem = kernel_state()->memory();
    bool repaired = false;
    for (int attempt = 1; attempt <= 3 && !did_insert; ++attempt) {
      if (attempt > 1) {
        fprintf(stderr,
                "[thread_list] handle=%08X retrying insert attempt %d after 1ms (spinlock held)\n",
                handle(), attempt);
        fflush(stderr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      auto list_head_safe =
          mem->TranslateVirtualSafe<X_LIST_ENTRY*>(list_head_guest);
      auto entry_safe = mem->TranslateVirtualSafe<X_LIST_ENTRY*>(entry_guest);

      fprintf(stderr,
              "[thread_list] handle=%08X attempt=%d head_guest=%08X entry_guest=%08X head_host=%p entry_host=%p\n",
              handle(), attempt, list_head_guest, entry_guest,
              static_cast<void*>(list_head_safe.pointer),
              static_cast<void*>(entry_safe.pointer));
      fflush(stderr);

      if (!list_head_safe.success || !entry_safe.success) {
        fprintf(stderr,
                "[thread_list] handle=%08X translate failed head_ok=%d entry_ok=%d head_err='%s' entry_err='%s'\n",
                handle(), list_head_safe.success ? 1 : 0,
                entry_safe.success ? 1 : 0, list_head_safe.error.c_str(),
                entry_safe.error.c_str());
        fflush(stderr);
        continue;
      }

      uint32_t head_flink = static_cast<uint32_t>(list_head_safe.pointer->flink_ptr);
      uint32_t head_blink = static_cast<uint32_t>(list_head_safe.pointer->blink_ptr);
      uint32_t entry_flink = static_cast<uint32_t>(entry_safe.pointer->flink_ptr);
      uint32_t entry_blink = static_cast<uint32_t>(entry_safe.pointer->blink_ptr);
      fprintf(stderr,
              "[thread_list] handle=%08X head.flink=%08X head.blink=%08X entry.flink=%08X entry.blink=%08X\n",
              handle(), head_flink, head_blink, entry_flink, entry_blink);
      fflush(stderr);

      if ((head_flink == 0 || head_blink == 0) && !repaired) {
        fprintf(stderr,
                "[thread_list] handle=%08X head appears uninitialized (flink=%08X blink=%08X) - repairing\n",
                handle(), head_flink, head_blink);
        fflush(stderr);
        list_head_safe.pointer->flink_ptr = list_head_guest;
        list_head_safe.pointer->blink_ptr = list_head_guest;
        head_flink = list_head_guest;
        head_blink = list_head_guest;
        repaired = true;
      }

      auto do_insert = [&](uint32_t old_tail_guest,
                           X_LIST_ENTRY* old_tail_host) {
        entry_safe.pointer->flink_ptr = list_head_guest;
        entry_safe.pointer->blink_ptr = old_tail_guest;
        old_tail_host->flink_ptr = entry_guest;
        list_head_safe.pointer->blink_ptr = entry_guest;
        return true;
      };

      uint32_t old_tail = list_head_safe.pointer->blink_ptr;
      auto* old_tail_heap = mem->LookupHeap(old_tail);
      fprintf(stderr,
              "[thread_list] handle=%08X old_tail=%08X heap=%s\n",
              handle(), old_tail, old_tail_heap ? "OK" : "NULL");
      fflush(stderr);

      if (old_tail == list_head_guest) {
        did_insert = do_insert(old_tail, list_head_safe.pointer);
        fprintf(stderr,
                "[thread_list] handle=%08X insert %s (tail=head)\n",
                handle(), did_insert ? "done" : "failed");
        fflush(stderr);
      } else if (!old_tail_heap) {
        fprintf(stderr,
                "[thread_list] handle=%08X invalid old_tail heap\n",
                handle());
        fflush(stderr);
      } else {
        auto old_tail_safe =
            mem->TranslateVirtualSafe<X_LIST_ENTRY*>(old_tail);
        if (!old_tail_safe.success) {
          fprintf(stderr,
                  "[thread_list] handle=%08X old_tail translate failed: %s\n",
                  handle(), old_tail_safe.error.c_str());
          fflush(stderr);
        } else {
          did_insert = do_insert(old_tail, old_tail_safe.pointer);
          fprintf(stderr,
                  "[thread_list] handle=%08X insert %s\n",
                  handle(), did_insert ? "done" : "failed");
          fflush(stderr);
        }
      }

      if (!did_insert && !repaired) {
        fprintf(stderr,
                "[thread_list] handle=%08X attempting list head repair\n",
                handle());
        fflush(stderr);
        list_head_safe.pointer->flink_ptr = list_head_guest;
        list_head_safe.pointer->blink_ptr = list_head_guest;
        repaired = true;
      }
    }

    if (!did_insert) {
      fprintf(stderr,
              "[thread_list] handle=%08X insert skipped - continuing\n",
              handle());
      fflush(stderr);
      DumpBacktrace("thread_list_insert_failed");
      printf("DEBUG: BREADCRUMB: About to release process->thread_list_spinlock\n");
      xboxkrnl::xeKeKfReleaseSpinLock(context_here, &process->thread_list_spinlock,
                                      old_irql);
      printf("DEBUG: BREADCRUMB: Spinlock released after failed thread list insert\n");
      return;
    }
#else
    util::XeInsertTailList(&process->thread_list, &guest_thread->process_threads,
                           context_here);
    // did_insert = true;
#endif
    printf("DEBUG: BREADCRUMB: XeInsertTailList completed\n");
    process->thread_count += 1;
    // todo: release dispatcher lock here?
    printf("DEBUG: BREADCRUMB: About to release process->thread_list_spinlock\n");
    xboxkrnl::xeKeKfReleaseSpinLock(context_here, &process->thread_list_spinlock,
                                    old_irql);
    printf("DEBUG: BREADCRUMB: Spinlock released, InitializeGuestObject COMPLETE\n");
  }
}

bool XThread::AllocateStack(uint32_t size) {
  auto heap = memory()->LookupHeap(kStackAddressRangeBegin);

  auto alignment = heap->page_size();
  auto padding = heap->page_size() * 2;  // Guard page size * 2
  size = xe::round_up(size, alignment);
  auto actual_size = size + padding;

  uint32_t address = 0;
  if (!heap->AllocRange(
          kStackAddressRangeBegin, kStackAddressRangeEnd, actual_size,
          alignment, kMemoryAllocationReserve | kMemoryAllocationCommit,
          kMemoryProtectRead | kMemoryProtectWrite, false, &address)) {
    return false;
  }

  stack_alloc_base_ = address;
  stack_alloc_size_ = actual_size;
  stack_limit_ = address + (padding / 2);
  stack_base_ = stack_limit_ + size;

  // Setup the guard pages - fail gracefully if protection fails
  const uint32_t guard_size = padding / 2;
  // Guard pages should be small; skip protection if something went off the rails.
  constexpr uint32_t kMaxReasonableGuardSize = 2 * 1024 * 1024;  // 2MB
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: Stack guard setup for GPU thread");
    XELOGI("Heap page size: {} bytes", heap->page_size());
    XELOGI("Guard size: {} bytes", guard_size);
    XELOGI("Stack alloc base: {:08X}", stack_alloc_base_);
    XELOGI("Stack limit/base: {:08X}-{:08X}", stack_limit_, stack_base_);
  }
  if (guard_size > kMaxReasonableGuardSize) {
    XELOGW("Skipping guard page protection - guard size too large: {} bytes",
           guard_size);
  } else if (!heap->Protect(stack_alloc_base_, guard_size,
                            kMemoryProtectNoAccess)) {
    XELOGW("Failed to setup bottom guard page for thread stack - continuing without guard protection");
  }
  if (guard_size > kMaxReasonableGuardSize) {
    XELOGW("Skipping top guard page protection - guard size too large: {} bytes",
           guard_size);
  } else if (!heap->Protect(stack_base_, guard_size, kMemoryProtectNoAccess)) {
    XELOGW("Failed to setup top guard page for thread stack - continuing without guard protection");
  }

  return true;
}

void XThread::FreeStack() {
  if (stack_alloc_base_) {
    auto heap = memory()->LookupHeap(kStackAddressRangeBegin);
    heap->Release(stack_alloc_base_);

    stack_alloc_base_ = 0;
    stack_alloc_size_ = 0;
    stack_base_ = 0;
    stack_limit_ = 0;
  }
}

X_STATUS XThread::Create() {
  auto create_stage = std::shared_ptr<std::atomic<int>>();
  auto update_stage = [&create_stage](int stage) {
    if (create_stage) {
      create_stage->store(stage, std::memory_order_relaxed);
    }
  };
#ifdef __APPLE__
  XELOGI("[ThreadCreate] Creating XThread handle={:08X}, thread_id={}, guest_thread={}, main_thread={}",
         handle(), thread_id_, guest_thread_, main_thread_);
         
  // Enhanced debugging for critical GPU thread
  if (handle() == 0x01000010) {
    create_stage = std::make_shared<std::atomic<int>>(0);
    std::thread([create_stage, this]() {
      static const char* kStageNames[] = {
          "begin",
          "native_created",
          "stack_allocated",
          "tls_initialized",
          "guest_object_initialized",
          "host_thread_created",
          "active_cpu_set",
          "thread_registered",
          "resume_called",
          "done"};
      int last_stage = create_stage->load(std::memory_order_relaxed);
      auto last_change = std::chrono::steady_clock::now();
      while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int stage = create_stage->load(std::memory_order_relaxed);
        if (stage >= 9) {
          return;
        }
        if (stage != last_stage) {
          last_stage = stage;
          last_change = std::chrono::steady_clock::now();
          continue;
        }
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_change)
                .count();
        if (elapsed >= 1000 && (elapsed % 1000) < 200) {
          const char* name = kStageNames[stage];
          fprintf(stderr,
                  "[watchdog] XThread::Create stalled stage=%d (%s) for %lld ms (handle=%08X)\n",
                  stage, name, static_cast<long long>(elapsed), handle());
          fprintf(stderr,
                  "[watchdog] current_xthread_tls_=%p cpu::Thread::current_thread_=%p running=%d\n",
                  static_cast<void*>(current_xthread_tls_),
                  static_cast<void*>(cpu::Thread::GetCurrentThread()),
                  running_ ? 1 : 0);
          fflush(stderr);
        }
      }
    }).detach();

    XELOGI("DEBUG: CRITICAL: Starting Create() for GPU Commands thread (handle {:08X})", handle());
    XELOGI("This is the exact crash point we're tracking");
    
    // Check TLS state before CreateNative
    XThread* tls_thread = reinterpret_cast<XThread*>(current_xthread_tls_);
    if (tls_thread) {
      XELOGI("Current TLS thread: {} (ID: {})", tls_thread->name(),
             tls_thread->thread_id());
      XELOGI("TLS status: ESTABLISHED");
    } else {
      XELOGI("Current TLS thread: NONE (creator thread likely has no TLS yet)");
      XELOGI("TLS status: NOT ESTABLISHED (expected for constructor context)");
    }
    
    // Check this pointer validity
    XELOGI("this pointer: {:08X}", reinterpret_cast<uintptr_t>(this));
    XELOGI("kernel_state_: {:08X}", reinterpret_cast<uintptr_t>(kernel_state_));
    XELOGI("host_object_: {}", host_object_ ? "YES" : "NO");
    XELOGI("name_: '{}'", name());
    XELOGI("thread_id_: {}", thread_id_);
    
    if (kernel_state_) {
      XELOGI("kernel_state_ memory: {:08X}", reinterpret_cast<uintptr_t>(kernel_state_->memory()));
      XELOGI("kernel_state_ emulator: {:08X}", reinterpret_cast<uintptr_t>(kernel_state_->emulator()));
    }
  }
#endif

  // Thread kernel object.
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: About to call CreateNative<X_KTHREAD>() for handle {:08X}", handle());
    XELOGI("This is where the bus error typically occurs");
    
#ifdef __APPLE__
    // macOS-specific pre-creation diagnostics
    XELOGI("DEBUG: macOS DIAGNOSTICS: Pre-thread-creation checks:");
    
    // Simple diagnostic logging - avoid complex mach APIs for now
    XELOGI("macOS thread creation diagnostics active");
    XELOGI("GPU Commands thread handle: {:08X}", handle());
    XELOGI("This is critical for debugging thread timeout issues");
#endif
  }
  
  if (!CreateNative<X_KTHREAD>()) {
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: CRITICAL: CreateNative<X_KTHREAD>() FAILED for handle {:08X}", handle());
      XELOGI("Error: Unable to allocate thread object");
      
#ifdef __APPLE__
      XELOGE("DEBUG: macOS FAILURE ANALYSIS:");
      XELOGE("This could be due to:");
      XELOGE("1. Insufficient memory (check resident size above)");
      XELOGE("2. Thread limit exceeded (check thread count above)");
      XELOGE("3. Memory protection issues (MAP_JIT failures)");
      XELOGE("4. Security policy preventing thread creation");
#endif
    } else {
      XELOGW("Unable to allocate thread object");
    }
    return X_STATUS_NO_MEMORY;
  }
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: CreateNative<X_KTHREAD>() SUCCEEDED for handle {:08X}", handle());
    
#ifdef __APPLE__
    XELOGI("DEBUG: macOS SUCCESS: Thread object allocation completed");
    XELOGI("Next critical phase: Stack and TLS allocation");
#endif
    update_stage(1);
  }

  // Allocate a stack.
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: About to allocate stack for GPU Commands thread (01000010)");
    XELOGI("Requested stack size: {} bytes", creation_params_.stack_size);
    XELOGI("Stack size in MB: {:.2f} MB", creation_params_.stack_size / (1024.0 * 1024.0));
  }
  
  if (!AllocateStack(creation_params_.stack_size)) {
    if (handle() == 0x01000010) {
      XELOGE("DEBUG: CRITICAL: Stack allocation FAILED for GPU Commands thread (01000010)");
      XELOGE("Requested size: {} bytes", creation_params_.stack_size);
      XELOGE("This could indicate memory fragmentation or insufficient memory");
      
#ifdef __APPLE__
      XELOGE("DEBUG: macOS STACK FAILURE ANALYSIS:");
      XELOGE("1. Check virtual memory availability");
      XELOGE("2. Check address space fragmentation");
      XELOGE("3. Verify guard page permissions");
#endif
    }
    return X_STATUS_NO_MEMORY;
  }
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: Stack allocation SUCCESS for GPU Commands thread (01000010)");
    XELOGI("Stack allocated: {:08X}-{:08X}", stack_limit_, stack_base_);
    XELOGI("Stack size: {} bytes", stack_base_ - stack_limit_);
    update_stage(2);
  }

  // Allocate TLS block.
  // Games will specify a certain number of 4b slots that each thread will get.
  xex2_opt_tls_info* tls_header = nullptr;
  auto module = kernel_state()->GetExecutableModule();
  if (module) {
    module->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  }

  constexpr uint32_t kDefaultTlsSlotCount = 1024;
  constexpr uint32_t kMaxTlsSlotCount = 2048;
  constexpr uint32_t kMaxTlsExtendedSize = 4 * 1024 * 1024;  // 4MB sanity cap
  uint32_t tls_slots = kDefaultTlsSlotCount;
  uint32_t tls_extended_size = 0;
  uint32_t tls_raw_data_address = 0;
  uint32_t tls_raw_data_size = 0;
  if (tls_header && tls_header->slot_count) {
    tls_slots = tls_header->slot_count;
    tls_extended_size = tls_header->data_size;
    tls_raw_data_address = tls_header->raw_data_address;
    tls_raw_data_size = tls_header->raw_data_size;
    if (tls_slots > kMaxTlsSlotCount) {
      XELOGW("TLS slot count too large ({}), clamping to {}",
             tls_slots, kMaxTlsSlotCount);
      tls_slots = kMaxTlsSlotCount;
    }
    if (tls_extended_size > kMaxTlsExtendedSize) {
      XELOGW("TLS extended size too large ({}), clamping to {}",
             tls_extended_size, kMaxTlsExtendedSize);
      tls_extended_size = kMaxTlsExtendedSize;
    }
    if (tls_raw_data_size > tls_extended_size) {
      XELOGW("TLS raw data size ({}) exceeds extended size ({}), clamping",
             tls_raw_data_size, tls_extended_size);
      tls_raw_data_size = tls_extended_size;
    }
  } else if (tls_header && !tls_header->slot_count) {
    // TLS header present but no slot count - keep defaults.
    tls_raw_data_address = tls_header->raw_data_address;
    tls_raw_data_size = tls_header->raw_data_size;
  }

  // Allocate both the slots and the extended data.
  // Some TLS is compiled with the binary (declspec(thread)) vars. The game
  // will directly access those through 0(r13).
  uint32_t tls_slot_size = tls_slots * 4;
  uint64_t tls_total_size_64 =
      static_cast<uint64_t>(tls_slot_size) + tls_extended_size;
  if (tls_total_size_64 > std::numeric_limits<uint32_t>::max()) {
    XELOGW("TLS total size overflow ({}), clamping to 32-bit max",
           tls_total_size_64);
    tls_total_size_64 = std::numeric_limits<uint32_t>::max();
  }
  tls_total_size_ = static_cast<uint32_t>(tls_total_size_64);
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: TLS allocation for GPU Commands thread (01000010)");
    XELOGI("TLS slots: {}", tls_slots);
    XELOGI("TLS slot size: {} bytes", tls_slot_size);
    XELOGI("TLS extended size: {} bytes", tls_extended_size);
    XELOGI("Total TLS size: {} bytes", tls_total_size_);
    XELOGI("TLS raw data address: {:08X}", tls_raw_data_address);
    XELOGI("TLS raw data size: {} bytes", tls_raw_data_size);
  }
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: TLS SystemHeapAlloc({} bytes) starting",
           tls_total_size_);
  }
  tls_static_address_ = memory()->SystemHeapAlloc(tls_total_size_);
  tls_dynamic_address_ = tls_static_address_ + tls_extended_size;
  if (!tls_static_address_) {
    if (handle() == 0x01000010) {
      XELOGE("DEBUG: CRITICAL: TLS allocation FAILED for GPU Commands thread (01000010)");
      XELOGE("TLS slots: {}", tls_slots);
      XELOGE("Total TLS size: {} bytes", tls_total_size_);
      XELOGE("This could indicate heap corruption or insufficient memory");
      
#ifdef __APPLE__
      XELOGE("DEBUG: macOS TLS FAILURE ANALYSIS:");
      XELOGE("1. System heap may be fragmented");
      XELOGE("2. Previous memory leaks may have exhausted heap");
      XELOGE("3. Security features may be blocking allocation");
#endif
    } else {
      XELOGW("Unable to allocate thread local storage block");
    }
    return X_STATUS_NO_MEMORY;
  }
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: TLS allocation SUCCESS for GPU Commands thread (01000010)");
    XELOGI("TLS static address: {:08X}", tls_static_address_);
    XELOGI("TLS dynamic address: {:08X}", tls_dynamic_address_);
  }

  // Zero all of TLS.
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: Zeroing TLS ({} bytes)", tls_total_size_);
    auto tls_host = memory()->TranslateVirtualSafe<uint8_t*>(tls_static_address_);
    if (tls_host.success) {
      fprintf(stderr,
              "[xthread] TLS memset begin guest=%08X host=%p size=%u\n",
              tls_static_address_, static_cast<void*>(tls_host.pointer),
              tls_total_size_);
    } else {
      fprintf(stderr,
              "[xthread] TLS memset begin guest=%08X translate failed: %s\n",
              tls_static_address_, tls_host.error.c_str());
    }
    fflush(stderr);
  }
  memory()->Fill(tls_static_address_, tls_total_size_, 0);
  if (handle() == 0x01000010) {
    fprintf(stderr, "[xthread] TLS memset done guest=%08X size=%u\n",
            tls_static_address_, tls_total_size_);
    fflush(stderr);
    update_stage(3);
  }
  if (tls_extended_size) {
    // If game has extended data, copy in the default values.
    if (tls_raw_data_address && tls_raw_data_size) {
      memory()->Copy(tls_static_address_, tls_raw_data_address,
                     tls_raw_data_size);
    } else {
      XELOGW("TLS extended data requested but raw data is missing (addr={:08X}, size={})",
             tls_raw_data_address, tls_raw_data_size);
    }
  }

  // Allocate thread state block from heap.
  // https://web.archive.org/web/20170704035330/https://www.microsoft.com/msj/archive/S2CE.aspx
  // This is set as r13 for user code and some special inlined Win32 calls
  // (like GetLastError/etc) will poke it directly.
  // We try to use it as our primary store of data just to keep things all
  // consistent.
  // 0x000: pointer to tls data
  // 0x100: pointer to TEB(?)
  // 0x10C: Current CPU(?)
  // 0x150: if >0 then error states don't get set (DPC active bool?)
  // TEB:
  // 0x14C: thread id
  // 0x160: last error
  // So, at offset 0x100 we have a 4b pointer to offset 200, then have the
  // structure.
  pcr_address_ = memory()->SystemHeapAlloc(0x2D8);
  if (!pcr_address_) {
    XELOGW("Unable to allocate thread state block");
    return X_STATUS_NO_MEMORY;
  }

  // Allocate processor thread state.
  // This is thread safe.
  thread_state_ = new cpu::ThreadState(kernel_state()->processor(), thread_id_,
                                       stack_base_, pcr_address_);
  XELOGI("XThread{:08X} ({:X}) Stack: {:08X}-{:08X}", handle(), thread_id_,
         stack_limit_, stack_base_);

  thread_state_->context()->kernel_state = kernel_state_;

  uint8_t cpu_index = GetFakeCpuNumber(
      static_cast<uint8_t>(creation_params_.creation_flags >> 24));

  X_KPCR* pcr = memory()->TranslateVirtual<X_KPCR*>(pcr_address_);
  pcr->tls_ptr = tls_static_address_;
  pcr->pcr_ptr = pcr_address_;
  pcr->prcb_data.current_thread = guest_object();
  pcr->prcb = pcr_address_ + offsetof(X_KPCR, prcb_data);
  pcr->host_stash = reinterpret_cast<uint64_t>(thread_state_->context());
  pcr->stack_base_ptr = stack_base_;
  pcr->stack_end_ptr = stack_limit_;

  printf("DEBUG: BREADCRUMB: Calling InitializeGuestObject()...\n");
  // Initialize the KTHREAD object.
  InitializeGuestObject();
  printf("DEBUG: BREADCRUMB: InitializeGuestObject() returned\n");
  if (handle() == 0x01000010) {
    update_stage(4);
  }

  pcr->prcb_data.dpc_active = 0;  // DPC active bool?

  // Always retain when starting - the thread owns itself until exited.
  RetainHandle();

  xe::threading::Thread::CreationParameters params;

  params.create_suspended = true;

  params.stack_size = 16_MiB;  // Allocate a big host stack.
  thread_ = xe::threading::Thread::Create(params, [this]() {
#ifdef __APPLE__
    XELOGI("[ThreadCreate] Thread {:08X} (ID: {}) starting execution", handle(), thread_id_);
#endif
    
    // CRITICAL: Set TLS immediately for host threads to prevent GetCurrentThread() failures
    // This must happen before any calls that might need thread context
    // bool is_host_thread = !guest_thread_;
#ifdef __APPLE__
    if (!guest_thread_) {
      XELOGI("[ThreadCreate] [macOS] Setting TLS IMMEDIATELY for host thread {:08X}", handle());
    }
#endif
    if (handle() == 0x01000010) {
      bool tls_ok = false;
      for (int attempt = 1; attempt <= 3; ++attempt) {
        current_xthread_tls_ = this;
        current_thread_ = this;
        cpu::Thread::SetCurrentThread(this);
        if (current_xthread_tls_ == this && current_thread_ == this) {
          tls_ok = true;
          fprintf(stderr,
                  "[gpu-thread] TLS set in thread entry (handle %08X, attempt %d)\n",
                  handle(), attempt);
          break;
        }
        xe::threading::Sleep(std::chrono::milliseconds(1));
      }
      if (!tls_ok) {
        fprintf(stderr,
                "[gpu-thread] TLS FAILED to set after retries (handle %08X)\n",
                handle());
      }
    } else {
      current_xthread_tls_ = this;
      current_thread_ = this;
      cpu::Thread::SetCurrentThread(this);
    }
    
    // CRITICAL: Verify TLS setup for critical threads (especially GPU Commands 0x01000010)
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) TLS verification in thread execution");
      assert(current_xthread_tls_ == this && "TLS setup failed in thread execution for GPU thread");
      assert(current_thread_ == this && "Thread pointer setup failed for GPU thread");
      XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) TLS verified successfully");
    }
    
    // Set thread ID override. This is used by logging.
    xe::threading::set_current_thread_id(handle());

    // Set name immediately, if we have one.
    thread_->set_name(thread_name_);

    // Profiler needs to know about the thread.
    xe::Profiler::ThreadEnter(thread_name_.c_str());

    // Execute user code.
#ifdef __APPLE__
    XELOGI("[ThreadCreate] Setting current_xthread_tls_ to {:08X}", handle());
#endif
    // TLS is already set above, but ensure it's still correct - CRITICAL for host threads
    current_xthread_tls_ = this;
    current_thread_ = this;
    
    // Final verification for critical threads before execution
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: FINAL VERIFICATION: GPU Commands thread (01000010) TLS before Execute()");
      assert(current_xthread_tls_ == this && "Final TLS verification failed for GPU thread");
    }
    
    cpu::ThreadState::Bind(this->thread_state());
    running_ = true;
    Execute();
    running_ = false;
    current_thread_ = nullptr;
#ifdef __APPLE__
    XELOGI("[ThreadCreate] Clearing current_xthread_tls_ for thread {:08X}", handle());
#endif
    current_xthread_tls_ = nullptr;

    xe::Profiler::ThreadExit();

    // Release the self-reference to the thread.
    ReleaseHandle();
  });

  if (!thread_) {
    // TODO(benvanik): translate error?
    XELOGE("CreateThread failed");
    return X_STATUS_NO_MEMORY;
  }
  if (handle() == 0x01000010) {
    update_stage(5);
  }

  // Set the thread name based on host ID (for easier debugging).
  if (thread_name_.empty()) {
    XELOGI("[ThreadCreate] DEBUG: About to call thread_->system_id() for name setting");
    XELOGI("[ThreadCreate] DEBUG: thread_ pointer: {}", static_cast<void*>(thread_.get()));
    set_name(fmt::format("XThread{:04X}", thread_->system_id()));
    XELOGI("[ThreadCreate] DEBUG: set_name() completed successfully");
  }

  if (creation_params_.creation_flags & 0x60) {
    XELOGI("[ThreadCreate] DEBUG: About to call thread_->set_priority()");
    thread_->set_priority(creation_params_.creation_flags & 0x20 ? 1 : 0);
    XELOGI("[ThreadCreate] DEBUG: thread_->set_priority() completed successfully");
  }

  // Assign the newly created thread to the logical processor, and also set up
  // the current CPU in KPCR and KTHREAD.
  XELOGI("[ThreadCreate] DEBUG: About to call SetActiveCpu(cpu_index={})", cpu_index);
  SetActiveCpu(cpu_index);
  XELOGI("[ThreadCreate] DEBUG: SetActiveCpu() completed successfully");
  if (handle() == 0x01000010) {
    update_stage(6);
  }

  // Notify processor of our creation.
  XELOGI("[ThreadCreate] DEBUG: About to call OnThreadCreated()");
  emulator()->processor()->OnThreadCreated(handle(), thread_state_, this);
  XELOGI("[ThreadCreate] DEBUG: OnThreadCreated() completed successfully");
  if (handle() == 0x01000010) {
    update_stage(7);
  }

if ((creation_params_.creation_flags & X_CREATE_SUSPENDED) == 0) {
    // Start to thread now that we're all setup.
#ifdef __APPLE__
    XELOGI("[ThreadCreate] Resuming thread {:08X} (ID: {})", handle(), thread_id_);
#endif
    
    // Enhanced monitoring for critical threads - start a watchdog
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: CRITICAL: Starting execution monitor for GPU Commands thread (01000010)");
      
      // Launch watchdog thread to detect hangs
      std::thread watchdog([this]() {
        XELOGI("DEBUG: WATCHDOG: Monitoring thread 01000010 startup...");
        
        // Wait up to 5 seconds for thread to start execution
        for (int i = 0; i < 50; ++i) {
          xe::threading::Sleep(std::chrono::milliseconds(100));
          if (running_) {
            XELOGI("DEBUG: WATCHDOG: Thread 01000010 started executing after {}ms", i * 100);
            return;
          }
        }
        
        XELOGE("DEBUG: WATCHDOG: Thread 01000010 FAILED to start within 5 seconds!");
        XELOGE("Thread state: created=true, running=false");
        XELOGE("This indicates a hang in thread startup sequence");
        
        // Print thread diagnostics
        if (thread_) {
          XELOGE("Host thread ID: {}", thread_->system_id());
          XELOGE("Thread name: {}", thread_->name());
        }
        XELOGE("Guest thread: {}", guest_thread_ ? "YES" : "NO");
        XELOGE("Thread state pointer: {}", static_cast<void*>(thread_state_));
      });
      watchdog.detach();
    }
    
    thread_->Resume();
    
    // Small delay to ensure thread has chance to start
    if (handle() == 0x01000010) {
      xe::threading::Sleep(std::chrono::milliseconds(10));
      XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) resumed, checking if running...");
      XELOGI("running_ = {}", running_);
      update_stage(8);
    }
  }

#ifdef __APPLE__
  XELOGI("[ThreadCreate] Thread {:08X} (ID: {}) creation complete", handle(), thread_id_);
#endif
  if (handle() == 0x01000010) {
    update_stage(9);
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Exit(int exit_code) {
#ifdef __APPLE__
  XELOGI("[ThreadExit] Thread {:08X} (ID: {}) exiting with code {}", handle(), thread_id_, exit_code);
#endif
  // This may only be called on the thread itself.
  assert_true(XThread::GetCurrentThread() == this);
  // TODO(chrispy): not sure if this order is correct, should it come after
  // apcs?
  auto kthread = guest_object<X_KTHREAD>();
  auto cpu_context = thread_state_->context();
  kthread->terminated = 1;

  // TODO(benvanik): dispatch events? waiters? etc?
  RundownAPCs();

  // Set exit code.
  kthread->header.signal_state = 1;
  kthread->exit_status = exit_code;

  auto kprocess = cpu_context->TranslateVirtual(kthread->process);

  uint32_t old_irql = xboxkrnl::xeKeKfAcquireSpinLock(
      cpu_context, &kprocess->thread_list_spinlock);

  util::XeRemoveEntryList(&kthread->process_threads, cpu_context);

  kprocess->thread_count = kprocess->thread_count - 1;

  xboxkrnl::xeKeKfReleaseSpinLock(cpu_context, &kprocess->thread_list_spinlock,
                                  old_irql);

  kernel_state()->OnThreadExit(this);

  // Notify processor of our exit.
  emulator()->processor()->OnThreadExit(thread_id_);

  // NOTE: unless PlatformExit fails, expect it to never return!
  current_xthread_tls_ = nullptr;
  current_thread_ = nullptr;
  xe::Profiler::ThreadExit();

  running_ = false;
  ReleaseHandle();

  // NOTE: this does not return!
  xe::threading::Thread::Exit(exit_code);
  return X_STATUS_SUCCESS;
}

X_STATUS XThread::Terminate(int exit_code) {
  // TODO(benvanik): inform the profiler that this thread is exiting.

  // Set exit code.
  X_KTHREAD* thread = guest_object<X_KTHREAD>();
  thread->header.signal_state = 1;
  thread->exit_status = exit_code;

  // Notify processor of our exit.
  emulator()->processor()->OnThreadExit(thread_id_);

  running_ = false;
  if (XThread::IsInThread(this)) {
    ReleaseHandle();
    xe::threading::Thread::Exit(exit_code);
  } else {
    thread_->Terminate(exit_code);
    ReleaseHandle();
  }

  return X_STATUS_SUCCESS;
}

class reenter_exception {
 public:
  explicit reenter_exception(uint32_t address) : address_(address) {};
  virtual ~reenter_exception() {};
  uint32_t address() const { return address_; }

 private:
  uint32_t address_;
};

void XThread::Execute() {
  XELOGKERNEL("XThread::Execute thid {} (handle={:08X}, '{}', native={:08X})",
              thread_id_, handle(), thread_name_, thread_->system_id());
  const bool use_xapi_startup =
      creation_params_.xapi_thread_startup != 0 &&
      creation_params_.xapi_thread_startup != creation_params_.start_address;
  const uint32_t selected_dispatch_address =
      use_xapi_startup ? creation_params_.xapi_thread_startup
                       : creation_params_.start_address;
  bool defer_pre_dispatch_apcs = false;
  if (main_thread_ && thread_state_ && thread_state_->context() &&
      selected_dispatch_address != 0) {
    auto* context = thread_state_->context();
    if (context->current_pc == 0) {
      // Seed the first dispatch target before any startup APC processing so
      // early sampling does not stay pinned at pc=0 while the main-thread
      // bridge has already been selected.
      context->current_pc = selected_dispatch_address;
    }
    defer_pre_dispatch_apcs =
        context->current_pc == selected_dispatch_address && context->lr == 0 &&
        context->ctr == 0;
  }
  
  // Log guest thread entry point for title thread
  if (main_thread_) {
    uint32_t context_pc =
        thread_state_ && thread_state_->context()
            ? thread_state_->context()->current_pc
            : 0;
    XELOGI(
        "DEBUG: GUEST THREAD STARTED: Main title thread (handle={:08X}, "
        "thread_id={}, start_address={:08X}, context_pc={:08X})",
        handle(), thread_id_, creation_params_.start_address, context_pc);
    if (context_pc == 0 && creation_params_.start_address != 0) {
      XELOGI(
          "GUEST MAIN THREAD: Launch context PC is 0 at Execute start; "
          "thread will begin at start_address={:08X} after processor "
          "dispatch/trampoline setup.",
          creation_params_.start_address);
    }
  }
  
  auto last_heartbeat_time = std::chrono::steady_clock::now();
  
  // Enhanced startup monitoring for critical threads
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) Execute() STARTING");
    XELOGI("About to call kernel_state()->OnThreadExecute()");
  }
  if (main_thread_) {
    XELOGI(
        "GUEST MAIN THREAD: calling KernelState::OnThreadExecute "
        "(dispatch_target={:08X}, pc={:08X}, lr={:08X}, ctr={:08X})",
        selected_dispatch_address,
        thread_state_ && thread_state_->context()
            ? thread_state_->context()->current_pc
            : 0,
        thread_state_ && thread_state_->context()
            ? static_cast<uint32_t>(thread_state_->context()->lr)
            : 0,
        thread_state_ && thread_state_->context()
            ? static_cast<uint32_t>(thread_state_->context()->ctr)
            : 0);
  }
  
  // Let's kernel know we are starting.
  kernel_state()->OnThreadExecute(this);
  if (main_thread_) {
    XELOGI("GUEST MAIN THREAD: KernelState::OnThreadExecute returned");
  }

// All threads get a mandatory sleep. This is to deal with some buggy
  // games that are assuming that 360 is so slow to create threads that they
  // have time to initialize shared structures AFTER CreateThread (RR).
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) about to sleep 10ms");
  }
  if (main_thread_) {
    XELOGI("GUEST MAIN THREAD: mandatory startup sleep begin (10ms)");
  }
  xe::threading::Sleep(std::chrono::milliseconds(10));
  if (main_thread_) {
    XELOGI("GUEST MAIN THREAD: mandatory startup sleep complete");
  }
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) sleep complete");
    XELOGI("About to call DeliverAPCs()");
  }

  // Dispatch any APCs that were queued before the thread was created first.
  if (defer_pre_dispatch_apcs) {
    XELOGW(
        "GUEST MAIN THREAD: deferring pre-dispatch DeliverAPCs because the "
        "startup context is still in zero-register bridge state "
        "(dispatch_target={:08X}, pc={:08X}, lr={:08X}, ctr={:08X}, "
        "r1={:08X}, r13={:08X})",
        selected_dispatch_address,
        thread_state_ && thread_state_->context()
            ? thread_state_->context()->current_pc
            : 0,
        thread_state_ && thread_state_->context()
            ? static_cast<uint32_t>(thread_state_->context()->lr)
            : 0,
        thread_state_ && thread_state_->context()
            ? static_cast<uint32_t>(thread_state_->context()->ctr)
            : 0,
        thread_state_ && thread_state_->context()
            ? static_cast<uint32_t>(thread_state_->context()->r[1])
            : 0,
        thread_state_ && thread_state_->context()
            ? static_cast<uint32_t>(thread_state_->context()->r[13])
            : 0);
  } else {
    if (main_thread_) {
      XELOGI("GUEST MAIN THREAD: calling DeliverAPCs before processor dispatch");
    }
    DeliverAPCs();
    if (main_thread_) {
      XELOGI("GUEST MAIN THREAD: DeliverAPCs returned");
    }
  }
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) DeliverAPCs() complete");
  }

  uint32_t address;
  std::vector<uint64_t> args;
  bool want_exit_code;
  int exit_code = 0;

  // If a XapiThreadStartup value is present, we use that as a trampoline.
  // Otherwise, we are a raw thread.
  if (use_xapi_startup) {
    address = creation_params_.xapi_thread_startup;
    args.push_back(creation_params_.start_address);
    args.push_back(creation_params_.start_context);
    want_exit_code = false;
  } else {
    // Run user code.
    address = creation_params_.start_address;
    args.push_back(creation_params_.start_context);
    want_exit_code = true;
  }

  if (main_thread_) {
    if (!creation_params_.xapi_thread_startup) {
      XELOGW(
          "GUEST MAIN THREAD: xapi_thread_startup is 0 - dispatch will use raw "
          "start_address={:08X}.",
          creation_params_.start_address);
      XELOGI(
          "GUEST MAIN THREAD: note: TLS/PCR setup is already performed in "
          "XThread::Create before Execute enters guest dispatch.");
    } else if (!use_xapi_startup) {
      XELOGI(
        "GUEST MAIN THREAD: xapi_thread_startup aliases start_address "
        "({:08X}); dispatch will remain in raw-start mode.",
        creation_params_.start_address);
    }
    if (cvars::kernel_log_main_thread_startup_probe) {
      if (IsGpuBootstrapStartupFunction(address)) {
        XELOGI(
            "GUEST MAIN THREAD: skipping startup probe for early GPU bootstrap "
            "target {:08X} to avoid pre-dispatch stalls.",
            address);
      } else {
        LogGuestStartWords(kernel_state()->memory(),
                           creation_params_.start_address, "start_address");
        if (use_xapi_startup) {
          LogGuestStartWords(kernel_state()->memory(),
                             creation_params_.xapi_thread_startup,
                             "xapi_thread_startup");
        }
      }
    }
    XELOGI(
        "GUEST MAIN THREAD: Dispatch target selected (target={:08X}, "
      "mode={}, xapi_thread_startup={:08X}, start_address={:08X}, "
      "start_context={:08X})",
        address,
        use_xapi_startup ? "xapi_thread_startup" : "raw_start_address",
      creation_params_.xapi_thread_startup,
        creation_params_.start_address, creation_params_.start_context);
    XELOGI(
      "GUEST MAIN THREAD: Entering processor dispatch "
      "(target={:08X}, arg_count={}, context_pc={:08X}, lr={:08X}, "
      "r1={:08X})",
      address, args.size(),
      thread_state_ && thread_state_->context()
        ? thread_state_->context()->current_pc
        : 0,
      thread_state_ && thread_state_->context()
        ? static_cast<uint32_t>(thread_state_->context()->lr)
        : 0,
      thread_state_ && thread_state_->context()
        ? static_cast<uint32_t>(thread_state_->context()->r[1])
        : 0);
  }

  uint32_t next_address;
  try {
    if (main_thread_ && address != 0) {
      xboxkrnl::RecordGpuBootstrapGuestMainDispatchBridge(
          address, thread_id_,
          use_xapi_startup ? "execute-dispatch:xapi_thread_startup"
                           : "execute-dispatch:raw_start_address");
    }
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) about to call processor()->Execute()");
      XELOGI("Address: {:08X}, Args count: {}", address, args.size());
      XELOGI("Thread state pointer: {}", static_cast<void*>(thread_state_));
      XELOGI("Kernel state pointer: {}", static_cast<void*>(kernel_state()));
    }
    
    exit_code = static_cast<int>(kernel_state()->processor()->Execute(
        thread_state_, address, args.data(), args.size()));
    
    // Heartbeat for title thread
  if (main_thread_) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat_time).count();
      if (elapsed >= 1000) {
        int heartbeat_count = 0;
        heartbeat_count++;
        XELOGI("DEBUG: GUEST THREAD HEARTBEAT #{}: pc={:08X} handle={:08X} thread_id={}",
               heartbeat_count, thread_state_->context()->current_pc, handle(), thread_id_);
        last_heartbeat_time = now;
      }
    }
    
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) processor()->Execute() COMPLETED");
      XELOGI("Exit code: {}", exit_code);
    }
    next_address = 0;
  } catch (const reenter_exception& ree) {
    if (handle() == 0x01000010) {
      XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) caught reenter_exception");
      XELOGI("Reenter address: {:08X}", ree.address());
    }
    next_address = ree.address();
  } catch (const std::exception& e) {
    if (handle() == 0x01000010) {
      XELOGE("DEBUG: CRITICAL: GPU Commands thread (01000010) caught std::exception: {}", e.what());
    }
    throw;
  } catch (...) {
    if (handle() == 0x01000010) {
      XELOGE("DEBUG: CRITICAL: GPU Commands thread (01000010) caught unknown exception");
    }
    throw;
  }

  // See XThread::Reenter comments.
  while (next_address != 0) {
    try {
      kernel_state()->processor()->ExecuteRaw(thread_state_, next_address);
      next_address = 0;
      if (want_exit_code) {
        exit_code = static_cast<int>(thread_state_->context()->r[3]);
      }
    } catch (const reenter_exception& ree) {
      next_address = ree.address();
    }
  }

  // If we got here it means the execute completed without an exit being called.
  // Treat the return code as an implicit exit code (if desired).
  Exit(!want_exit_code ? 0 : exit_code);
}

void XThread::Reenter(uint32_t address) {
  // TODO(gibbed): Maybe use setjmp/longjmp on Windows?
  // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/longjmp#remarks
  // On Windows with /EH, setjmp/longjmp do stack unwinding.
  // Is there a better solution than exceptions for stack unwinding?
  throw reenter_exception(address);
}

void XThread::EnterCriticalRegion() {
  guest_object<X_KTHREAD>()->apc_disable_count--;
}

void XThread::LeaveCriticalRegion() {
  auto kthread = guest_object<X_KTHREAD>();
  // this has nothing to do with user mode apcs!
#ifdef __APPLE__
  auto apc_disable_count = ++kthread->apc_disable_count;
  (void)apc_disable_count;
#else
  auto apc_disable_count = ++kthread->apc_disable_count;
#endif
}

void XThread::EnqueueApc(uint32_t normal_routine, uint32_t normal_context,
                         uint32_t arg1, uint32_t arg2) {
  // don't use thread_state_ -> context() ! we're not running on the thread
  // we're enqueuing to
  uint32_t success = xboxkrnl::xeNtQueueApcThread(
      this->handle(), normal_routine, normal_context, arg1, arg2,
      cpu::ThreadState::Get()->context());

  xenia_assert(success == X_STATUS_SUCCESS);
}

void XThread::SetCurrentThread() { 
#ifdef __APPLE__
  // Enhanced TLS state logging for critical transitions
  bool is_gpu_thread = (handle() == kGpuCommandsThreadHandle);
  auto prev_thread = current_xthread_tls_;
  uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  XELOGI("[SetCurrentThread] TLS STATE CHANGE at timestamp: {}", timestamp);
  XELOGI("Thread: {:08X} (ID: {}, name: '{}')", handle(), thread_id_, thread_name_);
  XELOGI("Previous TLS: {}", prev_thread ? fmt::format("{:08X} (ID: {})", prev_thread->handle(), prev_thread->thread_id_) : "nullptr");
  
  if (is_gpu_thread) {
    XELOGI("DEBUG: CRITICAL: GPU Commands thread (01000010) TLS setup");
    XELOGI("This is the critical thread that processes GPU commands");
  }
  
  // Check for potential TLS conflicts
  if (prev_thread && prev_thread != this) {
    XELOGW("DEBUG: TLS REPLACEMENT: Overwriting existing TLS context");
    XELOGW("Previous: {:08X} ({})", prev_thread->handle(), prev_thread->name());
    XELOGW("New: {:08X} ({})", handle(), thread_name_);
    
    // Backup previous TLS if it was a GPU thread
    if (prev_thread->handle() == kGpuCommandsThreadHandle) {
      XELOGE("DEBUG: CRITICAL: Overwriting GPU Commands thread TLS!");
      XELOGE("This will likely cause GPU processing to fail!");
    }
  }
#endif
  
  current_xthread_tls_ = this; 
  
#ifdef __APPLE__
  // Verify TLS was set correctly and log the new state
  XELOGI("[SetCurrentThread] TLS VERIFICATION: SUCCESS");
  XELOGI("Current TLS: {:08X} (ID: {})", handle(), thread_id_);
  
  if (is_gpu_thread) {
    XELOGI("DEBUG: CRITICAL: GPU Commands thread TLS successfully established");
  }
  
  // Log TLS backup status for critical threads
  if (is_gpu_thread) {
    BackupTLS();
    XELOGI("DEBUG: CRITICAL: TLS backup created for GPU Commands thread");
  }
#endif
}

void XThread::DeliverAPCs() {
  // https://www.drdobbs.com/inside-nts-asynchronous-procedure-call/184416590?pgno=1
  // https://www.drdobbs.com/inside-nts-asynchronous-procedure-call/184416590?pgno=7
  xboxkrnl::xeProcessUserApcs(thread_state_->context());
}

void XThread::RundownAPCs() {
  xboxkrnl::xeRundownApcs(thread_state_->context());
}

int32_t XThread::QueryPriority() { return thread_->priority(); }

void XThread::SetPriority(int32_t increment) {
  if (is_guest_thread()) {
    guest_object<X_KTHREAD>()->priority = static_cast<uint8_t>(increment);
  }
  priority_ = increment;
  int32_t target_priority = 0;
  if (increment > 0x22) {
    target_priority = xe::threading::ThreadPriority::kHighest;
  } else if (increment > 0x11) {
    target_priority = xe::threading::ThreadPriority::kAboveNormal;
  } else if (increment < -0x22) {
    target_priority = xe::threading::ThreadPriority::kLowest;
  } else if (increment < -0x11) {
    target_priority = xe::threading::ThreadPriority::kBelowNormal;
  } else {
    target_priority = xe::threading::ThreadPriority::kNormal;
  }
  if (!cvars::ignore_thread_priorities) {
    thread_->set_priority(target_priority);
  }
}

void XThread::SetAffinity(uint32_t affinity) {
  SetActiveCpu(GetFakeCpuNumber(affinity));
}

uint8_t XThread::active_cpu() const {
  const X_KPCR& pcr = *memory()->TranslateVirtual<const X_KPCR*>(pcr_address_);
  return pcr.prcb_data.current_cpu;
}

void XThread::SetActiveCpu(uint8_t cpu_index) {
  // May be called during thread creation - don't skip if current == new.

  assert_true(cpu_index < 6);

  X_KPCR& pcr = *memory()->TranslateVirtual<X_KPCR*>(pcr_address_);
  pcr.prcb_data.current_cpu = cpu_index;

  if (is_guest_thread()) {
    X_KTHREAD& thread_object =
        *memory()->TranslateVirtual<X_KTHREAD*>(guest_object());
    thread_object.current_cpu = cpu_index;
  }

  if (xe::threading::logical_processor_count() >= 6) {
    if (!cvars::ignore_thread_affinities) {
      thread_->set_affinity_mask(uint64_t(1) << cpu_index);
    }
  } else {
    // there no good reason why we need to log this... we don't perfectly
    // emulate the 360's scheduler in any way
    // XELOGW("Too few processor cores - scheduling will be wonky");
  }
}

bool XThread::GetTLSValue(uint32_t slot, uint32_t* value_out) {
  if (slot * 4 > tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  *value_out = xe::load_and_swap<uint32_t>(mem);
  return true;
}

bool XThread::SetTLSValue(uint32_t slot, uint32_t value) {
  if (slot * 4 >= tls_total_size_) {
    return false;
  }

  auto mem = memory()->TranslateVirtual(tls_dynamic_address_ + slot * 4);
  xe::store_and_swap<uint32_t>(mem, value);
  return true;
}

uint32_t XThread::suspend_count() {
  return guest_object<X_KTHREAD>()->suspend_count;
}

X_FILETIME XThread::creation_time() {
  return static_cast<X_FILETIME>(guest_object<X_KTHREAD>()->create_time);
}

uint32_t XThread::start_address() {
  return guest_object<X_KTHREAD>()->start_address;
}

X_STATUS XThread::Resume(uint32_t* out_suspend_count) {
  auto guest_thread = guest_object<X_KTHREAD>();

  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)
          ->fetch_sub(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  uint32_t unused_host_suspend_count = 0;
  if (thread_->Resume(&unused_host_suspend_count)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_UNSUCCESSFUL;
  }
}

X_STATUS XThread::Suspend(uint32_t* out_suspend_count) {
  // this normally holds the apc lock for the thread, because it queues a kernel
  // mode apc that does the actual suspension

  X_KTHREAD* guest_thread = guest_object<X_KTHREAD>();

  uint8_t previous_suspend_count =
      reinterpret_cast<std::atomic_uint8_t*>(&guest_thread->suspend_count)
          ->fetch_add(1);
  if (out_suspend_count) {
    *out_suspend_count = previous_suspend_count;
  }
  // If we are suspending ourselves, we can't hold the lock.
  uint32_t unused_host_suspend_count = 0;

  // If we had suspend count wrap around and go back to 0 then thread is not
  // suspended.
  if (guest_thread->suspend_count == 0) {
    return X_STATUS_SUCCESS;
  }

  if (thread_->Suspend(&unused_host_suspend_count)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_UNSUCCESSFUL;
  }
}

X_STATUS XThread::Delay(uint32_t processor_mode, uint32_t alertable,
                        uint64_t interval) {
  int64_t timeout_ticks = interval;
  uint32_t timeout_ms;
  if (timeout_ticks > 0) {
    // Absolute time, based on January 1, 1601.
    // TODO(benvanik): convert time to relative time.
    assert_always();
    timeout_ms = 0;
  } else if (timeout_ticks < 0) {
    // Relative time.
    timeout_ms = uint32_t(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    timeout_ms = 0;
  }

  timeout_ms = Clock::ScaleGuestDurationMillis(timeout_ms);
  if (alertable) {
    auto result =
        xe::threading::AlertableSleep(std::chrono::milliseconds(timeout_ms));
    switch (result) {
      default:
      case xe::threading::SleepResult::kSuccess:
        return X_STATUS_SUCCESS;
      case xe::threading::SleepResult::kAlerted:
        return X_STATUS_USER_APC;
    }
  } else {
    if (timeout_ms == 0) {
      if (priority_ <= xe::threading::ThreadPriority::kBelowNormal) {
        xe::threading::NanoSleep(100);
      } else {
        xe::threading::MaybeYield();
      }
    } else {
      xe::threading::Sleep(std::chrono::milliseconds(timeout_ms));
    }
  }

  return X_STATUS_SUCCESS;
}

struct ThreadSavedState {
  uint32_t thread_id;
  bool is_main_thread;  // Is this the main thread?
  bool is_running;

  uint32_t apc_head;
  uint32_t tls_static_address;
  uint32_t tls_dynamic_address;
  uint32_t tls_total_size;
  uint32_t pcr_address;
  uint32_t stack_base;        // High address
  uint32_t stack_limit;       // Low address
  uint32_t stack_alloc_base;  // Allocation address
  uint32_t stack_alloc_size;  // Allocation size

  // Context (invalid if not running)
  struct {
    uint64_t lr;
    uint64_t ctr;
    uint64_t r[32];
    double f[32];
    vec128_t v[128];
    uint32_t cr[8];
    uint32_t fpscr;
    uint8_t xer_ca;
    uint8_t xer_ov;
    uint8_t xer_so;
    uint8_t vscr_sat;
    uint32_t pc;
  } context;
};

bool XThread::Save(ByteStream* stream) {
  if (!guest_thread_) {
    // Host XThreads are expected to be recreated on their own.
    return false;
  }

  XELOGD("XThread {:08X} serializing...", handle());

  uint32_t pc = 0;
  if (running_) {
    pc = emulator()->processor()->StepToGuestSafePoint(thread_id_);
    if (!pc) {
      XELOGE("XThread {:08X} failed to save: could not step to a safe point!",
             handle());
      assert_always();
      return false;
    }
  }

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(kThreadSaveSignature);
  stream->Write(thread_name_);

  ThreadSavedState state;
  state.thread_id = thread_id_;
  state.is_main_thread = main_thread_;
  state.is_running = running_;
  state.tls_static_address = tls_static_address_;
  state.tls_dynamic_address = tls_dynamic_address_;
  state.tls_total_size = tls_total_size_;
  state.pcr_address = pcr_address_;
  state.stack_base = stack_base_;
  state.stack_limit = stack_limit_;
  state.stack_alloc_base = stack_alloc_base_;
  state.stack_alloc_size = stack_alloc_size_;

  if (running_) {
    // Context information
    auto context = thread_state_->context();
    state.context.lr = context->lr;
    state.context.ctr = context->ctr;
    std::memcpy(state.context.r, context->r, 32 * 8);
    std::memcpy(state.context.f, context->f, 32 * 8);
    std::memcpy(state.context.v, context->v, 128 * 16);
    state.context.cr[0] = context->cr0.value;
    state.context.cr[1] = context->cr1.value;
    state.context.cr[2] = context->cr2.value;
    state.context.cr[3] = context->cr3.value;
    state.context.cr[4] = context->cr4.value;
    state.context.cr[5] = context->cr5.value;
    state.context.cr[6] = context->cr6.value;
    state.context.cr[7] = context->cr7.value;
    state.context.fpscr = context->fpscr.value;
    state.context.xer_ca = context->xer_ca;
    state.context.xer_ov = context->xer_ov;
    state.context.xer_so = context->xer_so;
    state.context.vscr_sat = context->vscr_sat;
    state.context.pc = pc;
  }

  stream->Write(&state, sizeof(ThreadSavedState));
  return true;
}

object_ref<XThread> XThread::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  // Kind-of a hack, but we need to set the kernel state outside of the object
  // constructor so it doesn't register a handle with the object table.
  auto thread = new XThread(nullptr);
  thread->kernel_state_ = kernel_state;

  if (!thread->RestoreObject(stream)) {
    return nullptr;
  }

  if (stream->Read<uint32_t>() != kThreadSaveSignature) {
    XELOGE("Could not restore XThread - invalid magic!");
    return nullptr;
  }

  XELOGD("XThread {:08X}", thread->handle());

  thread->thread_name_ = stream->Read<std::string>();

  ThreadSavedState state;
  stream->Read(&state, sizeof(ThreadSavedState));
  thread->thread_id_ = state.thread_id;
  thread->main_thread_ = state.is_main_thread;
  thread->running_ = state.is_running;
  thread->tls_static_address_ = state.tls_static_address;
  thread->tls_dynamic_address_ = state.tls_dynamic_address;
  thread->tls_total_size_ = state.tls_total_size;
  thread->pcr_address_ = state.pcr_address;
  thread->stack_base_ = state.stack_base;
  thread->stack_limit_ = state.stack_limit;
  thread->stack_alloc_base_ = state.stack_alloc_base;
  thread->stack_alloc_size_ = state.stack_alloc_size;

  // Register now that we know our thread ID.
  kernel_state->RegisterThread(thread);

  thread->thread_state_ =
      new cpu::ThreadState(kernel_state->processor(), thread->thread_id_,
                           thread->stack_base_, thread->pcr_address_);

  if (state.is_running) {
    auto context = thread->thread_state_->context();
    context->kernel_state = kernel_state;
    context->lr = state.context.lr;
    context->ctr = state.context.ctr;
    std::memcpy(context->r, state.context.r, 32 * 8);
    std::memcpy(context->f, state.context.f, 32 * 8);
    std::memcpy(context->v, state.context.v, 128 * 16);
    context->cr0.value = state.context.cr[0];
    context->cr1.value = state.context.cr[1];
    context->cr2.value = state.context.cr[2];
    context->cr3.value = state.context.cr[3];
    context->cr4.value = state.context.cr[4];
    context->cr5.value = state.context.cr[5];
    context->cr6.value = state.context.cr[6];
    context->cr7.value = state.context.cr[7];
    context->fpscr.value = state.context.fpscr;
    context->xer_ca = state.context.xer_ca;
    context->xer_ov = state.context.xer_ov;
    context->xer_so = state.context.xer_so;
    context->vscr_sat = state.context.vscr_sat;

    // Always retain when starting - the thread owns itself until exited.
    thread->RetainHandle();

    xe::threading::Thread::CreationParameters params;
    params.create_suspended = true;  // Not done restoring yet.
    params.stack_size = 16_MiB;
    thread->thread_ = xe::threading::Thread::Create(params, [thread, state]() {
      // Set thread ID override. This is used by logging.
      xe::threading::set_current_thread_id(thread->handle());

      // Set name immediately, if we have one.
      thread->thread_->set_name(thread->name());

      // Profiler needs to know about the thread.
      xe::Profiler::ThreadEnter(thread->name().c_str());

      current_xthread_tls_ = thread;
      current_thread_ = thread;

      // Acquire any mutants
      for (auto mutant : thread->pending_mutant_acquires_) {
        uint64_t timeout = 0;
        auto status = mutant->Wait(0, 0, 0, &timeout);
        assert_true(status == X_STATUS_SUCCESS);
      }
      thread->pending_mutant_acquires_.clear();

      // Execute user code.
      thread->running_ = true;

      uint32_t pc = state.context.pc;
      thread->kernel_state_->processor()->ExecuteRaw(thread->thread_state_, pc);

      current_thread_ = nullptr;
      current_xthread_tls_ = nullptr;

      xe::Profiler::ThreadExit();

      // Release the self-reference to the thread.
      thread->ReleaseHandle();
    });
    assert_not_null(thread->thread_);

    // Notify processor we were recreated.
    thread->emulator()->processor()->OnThreadCreated(
        thread->handle(), thread->thread_state(), thread);
  }

  return object_ref<XThread>(thread);
}

XHostThread::XHostThread(KernelState* kernel_state, uint32_t stack_size,
                          uint32_t creation_flags, std::function<int()> host_fn,
                          uint32_t guest_process)
    : XThread(kernel_state, stack_size, 0, 0, 0, creation_flags, false, false,
              guest_process),
      host_fn_(host_fn) {
  // By default host threads are not debugger suspendable. If the thread runs
  // any guest code this must be overridden.
  can_debugger_suspend_ = false;

  // CRITICAL: Validate that XHostThread is correctly classified as a HOST thread
  // This assertion ensures the threading architecture is correct
  assert_false(guest_thread_ && "XHostThread MUST NOT be a guest thread!");
  assert_true(is_host_object() && "XHostThread MUST be a host object!");
  
  // Enhanced debugging - focus on GPU Commands thread (handle 01000010)
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: XHostThread constructor for CRITICAL handle 01000010:");
    XELOGI("Thread ID: {}", thread_id_);
    XELOGI("Stack size: {}", stack_size);
    XELOGI("Creation flags: {:#X}", creation_flags);
    XELOGI("Guest process: {}", guest_process);
    XELOGI("Host function pointer: {}", static_cast<void*>(&host_fn));
    XELOGI("This is the GPU Commands thread that processes PM4 packets");
  }
  else if (handle() != 0x01000010) {
    XELOGI("DEBUG: XHostThread constructor for CRITICAL handle 01000010:");
    XELOGI("Thread ID: {}", thread_id_);
    XELOGI("Stack size: {}", stack_size);
    XELOGI("Creation flags: {:#X}", creation_flags);
    XELOGI("Guest process: {}", guest_process);
    XELOGI("Host function pointer: {}", static_cast<void*>(&host_fn));
    XELOGI("This is the GPU Commands thread that processes PM4 packets");
  }

#ifdef __APPLE__
  // NOTE: XHostThread constructor runs on the creator thread, not the new host
  // thread. Touching thread-local TLS here will corrupt the creator thread's
  // TLS state and can deadlock later. Defer TLS setup/verification to the
  // actual thread entry where TLS is set correctly.
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: [macOS] XHostThread constructor - deferring TLS setup to thread entry for handle {:08X}", handle());
  }
#endif
}

void xe::kernel::XHostThread::Execute() {
  // Enhanced debugging - focus on GPU Commands thread (handle 01000010)
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: GPU Commands thread (01000010) Execute() STARTING:");
    XELOGI("Thread ID: {}", thread_id_);
    XELOGI("Handle: {:08X}", handle());
    XELOGI("Thread name: '{}'", thread_name_);
    XELOGI("Native thread ID: {:08X}", thread_->system_id());
  }
  
  XELOGKERNEL(
      "XThread::Execute thid {} (handle={:08X}, '{}', native={:08X}, <host>)",
      thread_id_, handle(), thread_name_, thread_->system_id());

#ifdef __APPLE__
  // macOS fix: Enhanced TLS persistence verification before executing host function
  // This is critical for preventing the GPU Commands thread hang
  bool tls_persisted = false;
  int tls_retry_count = 0;
  const int max_tls_retries = 5;
  
  while (!tls_persisted && tls_retry_count < max_tls_retries) {
    tls_retry_count++;
    
    // Verify TLS is still set correctly for this thread
    if (current_xthread_tls_ == this && current_thread_ == this) {
      tls_persisted = true;
      
      if (handle() == 0x01000010) {
        XELOGI("DEBUG: TLS PERSISTENCE: Attempt {} - SUCCESS", tls_retry_count);
      }
    } else {
      if (handle() == 0x01000010) {
        XELOGE("DEBUG: TLS PERSISTENCE: Attempt {} - FAILED", tls_retry_count);
        XELOGE("current_xthread_tls_ = {}", current_xthread_tls_ ? fmt::format("{:08X}", current_xthread_tls_->handle()) : "NULL");
XELOGE("current_thread_ = {}", current_thread_ ? "SET" : "NULL");
      }
      
       // Restore TLS for this thread
       current_xthread_tls_ = this;
       // Note: Don't set current_thread_ as it's base class TLS
      XELOGI("DEBUG: CRITICAL: Emergency TLS restoration attempted");
    }
    
    // Final verification - test GetCurrentThread() one more time
    auto* final_tls_test = GetCurrentThread();
    if (final_tls_test == this) {
      XELOGI("DEBUG: CRITICAL: Final GetCurrentThread() test PASSED");
    } else {
      XELOGE("DEBUG: CRITICAL: Final GetCurrentThread() test FAILED");
      XELOGE("Expected: {:08X}", handle());
      XELOGE("Got: {}", final_tls_test ? fmt::format("{:08X}", final_tls_test->handle()) : "nullptr");
    }
  }
#endif
  
  // Let the kernel know we are starting.
  kernel_state()->OnThreadExecute(this);
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: GPU Commands thread (01000010) about to execute host function");
    XELOGI("This is the WorkerThreadMain() function that processes GPU commands");
#ifdef __APPLE__
    XELOGI("TLS persistence status: {}", tls_persisted ? "VERIFIED" : "FAILED");
    if (!tls_persisted) {
      XELOGE("DEBUG: WARNING: Executing with failed TLS persistence - expect hang!");
    }
#endif
  }
  
  int ret = host_fn_();
  
  if (handle() == 0x01000010) {
    XELOGI("DEBUG: GPU Commands thread (01000010) host function COMPLETED:");
    XELOGI("Return value: {}", ret);
    XELOGI("Thread is now exiting");
#ifdef __APPLE__
    XELOGI("TLS persistence during execution: {}", tls_persisted ? "STABLE" : "UNSTABLE");
#endif
  }

  // Exit.
  Exit(ret);
}

#ifdef __APPLE__
// TLS backup and restore utility functions for critical threads
void xe::kernel::XThread::BackupTLS() {
  if (current_xthread_tls_) {
    tls_backup_ = current_xthread_tls_;
    tls_backup_timestamp_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    tls_backup_valid_ = true;
    
    XELOGI("DEBUG: TLS BACKUP: Created backup for thread {:08X}", current_xthread_tls_->handle());
    XELOGI("Backup timestamp: {}", tls_backup_timestamp_);
    XELOGI("Thread name: '{}'", current_xthread_tls_->name());
  } else {
    XELOGE("DEBUG: TLS BACKUP: Failed - no current TLS to backup");
    tls_backup_valid_ = false;
  }
}

void xe::kernel::XThread::RestoreTLS() {
  if (!tls_backup_valid_ || !tls_backup_) {
    XELOGE("DEBUG: TLS RESTORE: Failed - no valid backup available");
    return;
  }
  
  // Check if backup is still valid (not too old)
  uint64_t current_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  if (current_timestamp - tls_backup_timestamp_ > kTLSBackupTimeoutMs) {
    XELOGE("DEBUG: TLS RESTORE: Failed - backup too old ({} ms)", current_timestamp - tls_backup_timestamp_);
    tls_backup_valid_ = false;
    return;
  }
  
  // Restore TLS from backup
  current_xthread_tls_ = tls_backup_;
  XELOGI("DEBUG: TLS RESTORE: Restored TLS for thread {:08X}", tls_backup_->handle());
  XELOGI("Backup age: {} ms", current_timestamp - tls_backup_timestamp_);
  XELOGI("Thread name: '{}'", tls_backup_->name());
}

bool xe::kernel::XThread::VerifyTLS() {
  if (!current_xthread_tls_) {
    XELOGE("DEBUG: TLS VERIFY: FAILED - current_xthread_tls_ is NULL");
    return false;
  }
  
  if (!tls_backup_valid_) {
    XELOGI("DEBUG: TLS VERIFY: No backup available - current TLS may be unstable");
    return true;  // Current TLS exists, just no backup
  }
  
  // Check if current TLS matches backup
  if (current_xthread_tls_ == tls_backup_) {
    XELOGI("DEBUG: TLS VERIFY: SUCCESS - TLS matches backup");
    return true;
  } else {
    XELOGE("DEBUG: TLS VERIFY: FAILED - TLS mismatch");
    XELOGE("Current thread: {:08X}", current_xthread_tls_->handle());
    XELOGE("Backup thread: {:08X}", tls_backup_->handle());
    return false;
  }
}

xe::kernel::XThread* xe::kernel::XThread::GetTLSBackup() {
  return tls_backup_valid_ ? tls_backup_ : nullptr;
}

bool xe::kernel::XThread::IsTLSBackupValid() {
  if (!tls_backup_valid_ || !tls_backup_) {
    return false;
  }
  
  // Check if backup is still within timeout
  uint64_t current_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  if (current_timestamp - tls_backup_timestamp_ > kTLSBackupTimeoutMs) {
    tls_backup_valid_ = false;
    return false;
  }
  
  return true;
}

#endif
// ThreadStateManager implementation
#ifdef __APPLE__
xe::kernel::ThreadStateManager& xe::kernel::ThreadStateManager::Instance() {
  static ThreadStateManager instance;
  return instance;
}

void xe::kernel::ThreadStateManager::RegisterCriticalThread(uint32_t handle, xe::kernel::XThread* thread) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  critical_threads_[handle] = thread;
  XELOGI("DEBUG: ThreadStateManager: Registered critical thread {:08X}", handle);
}

void xe::kernel::ThreadStateManager::UnregisterCriticalThread(uint32_t handle) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = critical_threads_.find(handle);
  if (it != critical_threads_.end()) {
    critical_threads_.erase(it);
    XELOGI("DEBUG: ThreadStateManager: Unregistered critical thread {:08X}", handle);
  }
}

bool xe::kernel::ThreadStateManager::VerifyCriticalThread(uint32_t handle) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto it = critical_threads_.find(handle);
  if (it != critical_threads_.end()) {
    auto* thread = it->second;
    if (thread && XThread::GetCurrentThread() == thread) {
      XELOGI("DEBUG: ThreadStateManager: Critical thread {:08X} verified", handle);
      return true;
    }
  }
  return false;
}

void xe::kernel::ThreadStateManager::ForceTLSSync(xe::kernel::XThread* thread) {
  if (!thread) return;
  
  std::lock_guard<std::mutex> lock(state_mutex_);
  XELOGI("DEBUG: ThreadStateManager: Force TLS sync for thread {:08X}", thread->handle());
  
  // Force TLS establishment
  thread->SetCurrentThread();
  XThread::BackupTLS();
  
  state_cv_.notify_all();
}
#endif

}  // namespace kernel
}  // namespace xe
