/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <ranges>
#ifdef __APPLE__
#include <chrono>
#include <thread>
#endif

#include "xenia/kernel/kernel_state_mac.h"

#ifdef __APPLE__
#include "xenia/base/byte_order_mac.h"
#endif
#include "xenia/base/byte_stream.h"
#ifdef __APPLE__
#include "xenia/base/clock.h"
#endif
#include "xenia/base/logging.h"
#ifdef __APPLE__
#include "xenia/cpu/thread_mac.h"
#include "xenia/cpu/thread_state.h"
#endif
#include "xenia/emulator_mac.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading_mac.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject_mac.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/ui/imgui_host_notification.h"

#include "third_party/crypto/TinySHA1.hpp"

DEFINE_bool(apply_title_update, true, "Apply title updates.", "Kernel");
DEFINE_bool(allow_incompatible_title_update, true,
            "Allow title updates with mismatched signatures to be applied.",
            "Kernel");

DEFINE_uint32(kernel_build_version, 1888, "Define current kernel version",
              "Kernel");
#ifdef __APPLE__
DEFINE_uint32(
    kernel_user_module_load_event_min_interval_ms, 16,
    "For the sake of solving this problem, I handle this by a "
    "minimum interval between user-module load progress wake events in "
    "milliseconds. 0 disables throttling.",
    "Kernel");
DEFINE_bool(
    kernel_skip_dispatch_thread_start_in_set_executable, true,
    "Debug safety: skip starting the kernel deferred dispatch worker inside "
    "SetExecutableModule to avoid launch-time stalls while diagnosing module "
    "wiring hangs.",
    "Kernel");
DEFINE_bool(
    kernel_validate_interrupt_callback_before_execute, true,
    "Validate/resolve interrupt callback target before executing emulated CP "
    "interrupt DPC callback. Prevents executing invalid callback addresses.",
    "Kernel");
#endif

DECLARE_string(cl);

namespace xe {
namespace kernel {

constexpr std::chrono::milliseconds kDeferredOverlappedDelayMillis(25);
static void SecureZeroMemory(void* buffer, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(buffer);
  while (size) {
    *bytes++ = 0;
    --size;
  }
}

#ifdef __APPLE__
namespace {

void EnsureExecutingThreadBindings(XThread* thread, const char* phase) {
  if (!thread) {
    return;
  }

  XThread* current_xthread = XThread::GetCurrentThread();
  if (current_xthread != thread) {
    XELOGW("KernelState::{} repairing XThread TLS current={} expected={:08X}",
           phase ? phase : "<unknown>",
           current_xthread ? fmt::format("{:08X}", current_xthread->handle())
                           : "NULL",
           thread->handle());
    thread->SetCurrentThread();
    cpu::Thread::SetCurrentThread(thread);
  }

  auto* expected_thread_state = thread->thread_state();
  if (cpu::ThreadState::Get() != expected_thread_state) {
    XELOGW(
        "KernelState::{} rebinding ThreadState current_tid={} expected_tid={}",
        phase ? phase : "<unknown>",
        cpu::ThreadState::Get() ? cpu::ThreadState::Get()->thread_id() : -1,
        expected_thread_state ? expected_thread_state->thread_id() : -1);
    cpu::ThreadState::Bind(expected_thread_state);
  }
}

}  // namespace
#endif

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() { return shared_kernel_state_; }

KernelState::KernelState(Emulator* emulator)
    : emulator_(emulator),
      memory_(emulator->memory()),
      dispatch_thread_running_(false),
      dpc_list_(emulator->memory()),
      kernel_trampoline_group_(emulator->processor()->backend()) {
  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;
  processor_ = emulator->processor();
  file_system_ = emulator->file_system();
  xam_state_ = std::make_unique<xam::XamState>(emulator, this);
  smc_ = std::make_unique<SystemManagementController>();
#ifdef __APPLE__
  user_module_load_event_ = xe::threading::Event::CreateAutoResetEvent(false);
#endif

  InitializeKernelGuestGlobals();
  kernel_version_ = KernelVersion(cvars::kernel_build_version);

  // Hardcoded maximum of 2048 TLS slots.
  tls_bitmap_.Resize(2048);

  auto hc_loc_heap = memory_->LookupHeap(strange_hardcoded_page_);
  bool fixed_alloc_worked = hc_loc_heap->AllocFixed(
      strange_hardcoded_page_, 65536, 0,
      kMemoryAllocationCommit | kMemoryAllocationReserve,
      kMemoryProtectRead | kMemoryProtectWrite);

  xenia_assert(fixed_alloc_worked);
}

KernelState::~KernelState() {
  SetExecutableModule(nullptr);

  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_cond_.notify_all();
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }

  executable_module_.reset();
  user_modules_.clear();
  kernel_modules_.clear();
#ifdef __APPLE__
  loaded_user_modules_count_.store(0, std::memory_order_relaxed);
  loaded_executable_user_modules_count_.store(0, std::memory_order_relaxed);
#endif

  // Delete all objects.
  object_table_.Reset();

  xam_state_.reset();

  assert_true(shared_kernel_state_ == this);
  shared_kernel_state_ = nullptr;
}

KernelState* KernelState::shared() { return shared_kernel_state_; }

uint32_t KernelState::title_id() const {
  if (!executable_module_) {
    return 0;
  }

  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

const std::unique_ptr<xam::SpaInfo> KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

const std::unique_ptr<xam::SpaInfo> KernelState::module_xdbf(
    object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(
          fmt::format("{:08X}", exec_module->title_id()).c_str(),
          &resource_data, &resource_size))) {
    return std::make_unique<xam::SpaInfo>(std::span<uint8_t>(
        memory()->TranslateVirtual(resource_data), resource_size));
  }

  return nullptr;
}

uint32_t KernelState::AllocateTLS() { return uint32_t(tls_bitmap_.Acquire()); }

void KernelState::FreeTLS(uint32_t slot) {
  const std::vector<object_ref<XThread>> threads =
      object_table()->GetObjectsByType<XThread>();

  for (const object_ref<XThread>& thread : threads) {
    if (thread->is_guest_thread()) {
      thread->SetTLSValue(slot, 0);
    }
  }
  tls_bitmap_.Release(slot);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine,
                                                     uint32_t priority) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin();
       it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
#ifdef __APPLE__
  loaded_user_modules_count_.fetch_add(1, std::memory_order_relaxed);
  if (module && module->is_executable()) {
    loaded_executable_user_modules_count_.fetch_add(1,
                                                    std::memory_order_relaxed);
  }
#endif
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
#ifdef __APPLE__
      loaded_user_modules_count_.fetch_sub(1, std::memory_order_relaxed);
      if (*it && (*it)->is_executable()) {
        loaded_executable_user_modules_count_.fetch_sub(
            1, std::memory_order_relaxed);
      }
#endif
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

bool KernelState::IsModuleLoaded(const std::string_view name) {
  if (name.empty()) {
    return true;
  }

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }

  auto global_lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->Matches(name)) {
      return true;
    }
  }

  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(
    const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (xe::utf8::equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  if (!user_only) {
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  auto path(name);

  // Resolve the path to an absolute path.
  auto entry = file_system_->ResolvePath(name);
  if (entry) {
    path = entry->absolute_path();
  }

  for (auto user_module : user_modules_) {
    if (user_module->Matches(path)) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }

  SetExecutableModule(module);
#ifdef __APPLE__
  const uint32_t main_thread_xapi_startup = module->entry_point();
#endif
  XELOGI("KernelState: Launching module...");
#ifdef __APPLE__
  XELOGI(
      "GUEST MAIN THREAD: LaunchModule config module='{}' path='{}' "
      "entry={:08X} stack_size={:08X} xapi_thread_startup={:08X} "
      "(LaunchModule raw main-thread policy)",
      module->name(), module->path(), module->entry_point(),
      module->stack_size(), main_thread_xapi_startup);
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t addr = module->entry_point() + i * 4;
    auto word_result = memory_->TranslateVirtualSafe<xe::be<uint32_t>*>(addr);
    if (!word_result.success || !word_result.pointer) {
      XELOGW(
          "GUEST MAIN THREAD: LaunchModule entry probe word[{}] "
          "addr={:08X} unreadable error={}",
          i, addr, word_result.error);
      continue;
    }
    XELOGI(
        "GUEST MAIN THREAD: LaunchModule entry probe word[{}] addr={:08X} "
        "value={:08X}",
        i, addr, xe::load_and_swap<uint32_t>(word_result.pointer));
  }
#endif

  // Create a thread to run in.
  // We start suspended so we can run the debugger prep.
  auto thread = object_ref<XThread>(
#ifdef XE_PLATFORM_WIN32
      new XThread(kernel_state(), module->stack_size(), 0,
#endif
#ifdef __APPLE__
      new XThread(kernel_state(), module->stack_size(), main_thread_xapi_startup,
#endif
                  module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");
#ifdef __APPLE__
  // Validate that the main game thread IS a GUEST thread
  if (!thread->is_guest_thread()) {
    XELOGE("FATAL: Main game thread misclassified as HOST thread!");
    XELOGE("Game executable threads MUST be GUEST threads.");
    std::abort();
  }
  if (!thread->main_thread()) {
    XELOGE("FATAL: Main game thread not marked as main_thread!");
    std::abort();
  }
  XELOGI("   Main game thread role validation PASSED (GUEST thread, main=true)");
#endif

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  // Waits for a debugger client, if desired.
  emulator()->processor()->PreLaunch();

  // Resume the thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  thread->Resume();

  return thread;
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

#ifdef __APPLE__
KernelState::UserModuleLoadStats KernelState::GetUserModuleLoadStats() {
  UserModuleLoadStats stats;
  stats.load_calls = user_module_load_calls_.load(std::memory_order_relaxed);
  stats.load_inflight =
      user_module_load_inflight_count_.load(std::memory_order_relaxed) != 0;
  stats.load_success =
      user_module_load_success_.load(std::memory_order_relaxed);
  stats.load_fail = user_module_load_fail_.load(std::memory_order_relaxed);
  stats.last_begin_ms =
      user_module_last_begin_ms_.load(std::memory_order_relaxed);
  stats.last_end_ms = user_module_last_end_ms_.load(std::memory_order_relaxed);
  stats.last_progress_ms =
      user_module_last_progress_ms_.load(std::memory_order_relaxed);
  stats.load_progress_count =
      user_module_progress_count_.load(std::memory_order_relaxed);
  stats.last_status = user_module_last_status_.load(std::memory_order_relaxed);
  stats.loaded_user_modules =
      loaded_user_modules_count_.load(std::memory_order_relaxed);
  stats.loaded_executable_user_modules =
      loaded_executable_user_modules_count_.load(std::memory_order_relaxed);

  return stats;
}

void KernelState::NoteUserModuleLoadProgress() {
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  user_module_last_progress_ms_.store(now_ms, std::memory_order_relaxed);
  user_module_progress_count_.fetch_add(1, std::memory_order_relaxed);
  if (!user_module_load_event_) {
    return;
  }

  uint64_t min_interval_ms =
      cvars::kernel_user_module_load_event_min_interval_ms;
  if (min_interval_ms == 0) {
    user_module_last_event_signal_ms_.store(now_ms, std::memory_order_relaxed);
    user_module_load_event_->Set();
    return;
  }

  // Never spin unbounded here: this is called from hot module-load paths.
  constexpr uint32_t kMaxEventSignalCasAttempts = 32;
  for (uint32_t attempt = 0; attempt < kMaxEventSignalCasAttempts; ++attempt) {
    uint64_t last_signal_ms =
        user_module_last_event_signal_ms_.load(std::memory_order_relaxed);
    // Avoid unsigned underflow / back-in-time CAS updates under contention.
    if (last_signal_ms != 0) {
      if (now_ms <= last_signal_ms) {
        return;
      }
      if (now_ms - last_signal_ms < min_interval_ms) {
        return;
      }
    }
    if (user_module_last_event_signal_ms_.compare_exchange_weak(
            last_signal_ms, now_ms, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      user_module_load_event_->Set();
      return;
    }
    // Refresh to avoid retrying with a stale timestamp forever.
    now_ms = Clock::QueryHostUptimeMillis();
  }
}

object_ref<UserModule> KernelState::FindFirstExecutableUserModule() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& user_module : user_modules_) {
    if (!user_module || !user_module->is_executable()) {
      continue;
    }
    return user_module;
  }
  return nullptr;
}
#endif

void KernelState::SetExecutableModule(object_ref<UserModule> module) {
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule begin module_ptr={} current_ptr={}",
         static_cast<void*>(module.get()),
         static_cast<void*>(executable_module_.get()));
#endif
  if (module.get() == executable_module_.get()) {
#ifdef __APPLE__
    XELOGI("KernelState::SetExecutableModule no-op (same module)");
#endif
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
#ifdef __APPLE__
    XELOGI("KernelState::SetExecutableModule cleared executable module");
#endif
    return;
  }

#ifdef __APPLE__
  XELOGI(
      "KernelState::SetExecutableModule selected module='{}' path='{}' "
      "entry={:08X} hmodule={:08X}",
      executable_module_->name(), executable_module_->path(),
      executable_module_->entry_point(), executable_module_->hmodule_ptr());

  XELOGI("   KernelState::SetExecutableModule step=InitializeProcess.begin");
#endif
  auto title_process =
      memory_->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());

  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule step=InitializeProcess.end");

  XELOGI("KernelState::SetExecutableModule step=TLS.begin");
#endif
  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    title_process->tls_static_data_address = tls_header->raw_data_address;
    title_process->tls_data_size = tls_header->data_size;
    title_process->tls_raw_data_size = tls_header->raw_data_size;
    title_process->tls_slot_size = tls_header->slot_count * 4;
    SetProcessTLSVars(title_process, tls_header->slot_count,
                      tls_header->data_size, tls_header->raw_data_address);
  }
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule step=TLS.end present={}",
         tls_header ? "YES" : "NO");
#endif

  uint32_t kernel_stacksize = 0;
  XELOGI("KernelState::SetExecutableModule step=StackSize.begin");
  // During deferred executable wiring (entry point still unresolved), avoid
  // optional-header traversal here - stack sizing can be finalized later once
  // LoadContinue completes.
  if (executable_module_->entry_point() != 0) {
    executable_module_->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE,
                                     &kernel_stacksize);
  } else {
    XELOGI(
        "KernelState::SetExecutableModule step=StackSize.skip "
        "(entry unresolved)");
  }
  if (kernel_stacksize != 0) {
    kernel_stacksize = (kernel_stacksize + 4095) & 0xFFFFF000;
    if (kernel_stacksize < 0x4000) {
      kernel_stacksize = 0x4000;
    }
    title_process->kernel_stack_size = kernel_stacksize;
  }
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule step=StackSize.end value={:08X}",
         kernel_stacksize);
#endif

  // Setup the kernel's XexExecutableModuleHandle field.
#ifdef __APPLE__
  XELOGI(
      "KernelState::SetExecutableModule "
      "step=XexExecutableModuleHandle.begin");
#endif
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory()->TranslateVirtual<xe::be<uint32_t>*>(
        export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }
#ifdef __APPLE__
  XELOGI(
      "KernelState::SetExecutableModule step=XexExecutableModuleHandle.end "
      "found={}",
      export_entry ? "YES" : "NO");
#endif

  // Setup the kernel's ExLoadedImageName field
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule step=ExLoadedImageName.begin");
#endif
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedImageName);

  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);
    xe::string_util::copy_truncating(
        variable_ptr, executable_module_->path(),
        xboxkrnl::XboxkrnlModule::kExLoadedImageNameSize);
  }
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule step=ExLoadedImageName.end found={}",
         export_entry ? "YES" : "NO");
#endif

  // Setup the kernel's ExLoadedCommandLine field
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule step=ExLoadedCommandLine.begin");
#endif
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedCommandLine);
  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);

    std::string module_name =
        fmt::format("\"{}.xex\"", executable_module_->name());
    if (!cvars::cl.empty()) {
      module_name += " " + cvars::cl;
    }

    xe::string_util::copy_truncating(
        variable_ptr, module_name,
        xboxkrnl::XboxkrnlModule::kExLoadedCommandLineSize);
  }
#ifdef __APPLE__
  XELOGI(
      "KernelState::SetExecutableModule step=ExLoadedCommandLine.end "
      "found={}",
      export_entry ? "YES" : "NO");
#endif

  // Spin up deferred dispatch worker.
  // TODO(benvanik): move someplace more appropriate (out of ctor, but around
  // here).
#ifdef __APPLE__
  XELOGI(
      "KernelState::SetExecutableModule step=DispatchThread.begin "
      "running={}",
      dispatch_thread_running_ ? "YES" : "NO");
  if (cvars::kernel_skip_dispatch_thread_start_in_set_executable) {
    XELOGW(
        "KernelState::SetExecutableModule: skipping dispatch thread "
        "startup due to kernel_skip_dispatch_thread_start_in_set_executable");
    XELOGI("KernelState::SetExecutableModule end (dispatch startup skipped)");
    return;
  }
#endif
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
#ifdef __APPLE__
    XELOGI("KernelState::SetExecutableModule step=DispatchThread.create.begin");
#endif
    dispatch_thread_ = object_ref<XHostThread>(new XHostThread(
        this, 128 * 1024, 0,
        [this]() {
          // As we run guest callbacks the debugger must be able to suspend us.
          dispatch_thread_->set_can_debugger_suspend(true);

          auto global_lock = global_critical_region_.AcquireDeferred();
          while (dispatch_thread_running_) {
            global_lock.lock();
            if (dispatch_queue_.empty()) {
              dispatch_cond_.wait(global_lock);
              if (!dispatch_thread_running_) {
                global_lock.unlock();
                break;
              }
            }
            auto fn = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
            global_lock.unlock();

            fn();
          }
          return 0;
        },
        GetSystemProcess()));  // don't think an equivalent exists on real hw
    dispatch_thread_->set_name("Kernel Dispatch");

#ifdef __APPLE__
    // Validate that Kernel Dispatch is a HOST thread
    if (dispatch_thread_->is_guest_thread()) {
      XELOGE("FATAL: Kernel Dispatch thread misclassified as GUEST thread!");
      XELOGE("Kernel infrastructure MUST be HOST threads.");
      std::abort();
    }
    XELOGI("Kernel Dispatch thread role validation PASSED (HOST thread)");

    XELOGI("KernelState::SetExecutableModule step=DispatchThread.create.call");
#endif
    dispatch_thread_->Create();
#ifdef __APPLE__
    XELOGI("KernelState::SetExecutableModule step=DispatchThread.create.end");
#endif
  }
#ifdef __APPLE__
  XELOGI("KernelState::SetExecutableModule end");
#endif
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(
    const std::string_view raw_name, bool call_entry) {
#ifdef __APPLE__
  uint64_t load_start_ms = Clock::QueryHostUptimeMillis();
  user_module_load_calls_.fetch_add(1, std::memory_order_relaxed);
  user_module_load_inflight_count_.fetch_add(1, std::memory_order_relaxed);
  user_module_last_begin_ms_.store(load_start_ms, std::memory_order_relaxed);
  NoteUserModuleLoadProgress();
  XELOGI("KernelState::LoadUserModule begin raw='{}' call_entry={}", raw_name,
         call_entry ? "YES" : "NO");
  auto finalize_load = [this](uint32_t status, bool success) {
    NoteUserModuleLoadProgress();
    if (success) {
      user_module_load_success_.fetch_add(1, std::memory_order_relaxed);
    } else {
      user_module_load_fail_.fetch_add(1, std::memory_order_relaxed);
    }
    user_module_last_status_.store(status, std::memory_order_relaxed);
    user_module_last_end_ms_.store(Clock::QueryHostUptimeMillis(),
                                   std::memory_order_relaxed);
    user_module_load_inflight_count_.fetch_sub(1, std::memory_order_relaxed);
    if (user_module_load_event_) {
      user_module_last_event_signal_ms_.store(Clock::QueryHostUptimeMillis(),
                                              std::memory_order_relaxed);
      user_module_load_event_->Set();
    }
  };
#endif
  // Some games try to load relative to launch module, others specify full path.
  auto name = xe::utf8::find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    if (!executable_module_) {
#ifdef XE_PLATFORM_WINDOWS
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path((*user_modules_.cbegin())->path()),
          name);
#endif
#ifdef __APPLE__
      if (!user_modules_.empty()) {
        path = xe::utf8::join_guest_paths(
            xe::utf8::find_base_guest_path((*user_modules_.cbegin())->path()),
            name);
      }
#endif
    } else {
      path = xe::utf8::join_guest_paths(
          xe::utf8::find_base_guest_path(executable_module_->path()), name);
    }
  }

  object_ref<UserModule> module;
  {
#ifdef __APPLE__
    uint64_t lock_wait_start_ms = Clock::QueryHostUptimeMillis();
#endif
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
#ifdef __APPLE__
    uint64_t lock_wait_end_ms = Clock::QueryHostUptimeMillis();
    XELOGI(
        "KernelState::LoadUserModule acquired global lock in {} ms "
        "(resolved_path='{}')",
        static_cast<unsigned long long>(lock_wait_end_ms - lock_wait_start_ms),
        path);
    NoteUserModuleLoadProgress();
#endif

    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(path)) {
#ifdef __APPLE__
        XELOGI(
            "KernelState::LoadUserModule hit already-loaded module '{}' "
            "(total={} ms)",
            path,
            static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                            load_start_ms));
        finalize_load(X_STATUS_SUCCESS, true);
#endif
        return existing_module;
      }
    }

#ifdef __APPLE__
  }
#endif
#ifdef XE_PLATFORM_WINDOWS
  global_lock.unlock();

  // Module wasn't loaded, so load it.
  module = object_ref<UserModule>(new UserModule(this));
  X_STATUS status = module->LoadFromFile(path);
  if (XFAILED(status)) {
    object_table()->ReleaseHandle(module->handle());
    return nullptr;
  }

  global_lock.lock();

  // Putting into the listing automatically retains.
  user_modules_.push_back(module);
#endif
#ifdef __APPLE__
  // Module wasn't loaded, so load it with the global lock released.
  module = object_ref<UserModule>(new UserModule(this));
  uint64_t load_from_file_start_ms = Clock::QueryHostUptimeMillis();
  X_STATUS status = module->LoadFromFile(path);
  uint64_t load_from_file_end_ms = Clock::QueryHostUptimeMillis();
  NoteUserModuleLoadProgress();
  XELOGI(
      "KernelState::LoadUserModule LoadFromFile('{}') took {} ms "
      "status={:08X}",
      path,
      static_cast<unsigned long long>(load_from_file_end_ms -
                                      load_from_file_start_ms),
      status);
  if (XFAILED(status)) {
    object_table()->ReleaseHandle(module->handle());
    finalize_load(status, false);
    return nullptr;
#endif
  }

#ifdef __APPLE__
  // Re-acquire for module list mutation and counters.
  uint64_t relock_wait_start_ms = Clock::QueryHostUptimeMillis();
  auto global_lock = global_critical_region_.Acquire();
  uint64_t relock_wait_end_ms = Clock::QueryHostUptimeMillis();
  XELOGI(
      "KernelState::LoadUserModule reacquired global lock in {} ms for "
      "module registration",
      static_cast<unsigned long long>(relock_wait_end_ms -
                                      relock_wait_start_ms));

  // Putting into the listing automatically retains.
  user_modules_.push_back(module);
  uint64_t loaded_user_modules =
      loaded_user_modules_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t loaded_executable_user_modules =
      loaded_executable_user_modules_count_.load(std::memory_order_relaxed);
  if (module && module->is_executable()) {
    loaded_executable_user_modules =
        loaded_executable_user_modules_count_.fetch_add(
            1, std::memory_order_relaxed) +
        1;
  }
  XELOGI(
      "KernelState::LoadUserModule checkpoint module='{}' path='{}' "
      "entry={:08X} loaded_user_modules={} loaded_executable_user_modules={}",
      module->name(), module->path(), module->entry_point(),
      static_cast<unsigned long long>(loaded_user_modules),
      static_cast<unsigned long long>(loaded_executable_user_modules));
  NoteUserModuleLoadProgress();
  global_lock.unlock();
  finalize_load(status, true);

  XELOGI("KernelState::LoadUserModule end '{}' total={} ms", path,
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                         load_start_ms));
#endif
  return module;
}

object_ref<UserModule> KernelState::LoadUserModuleFromMemory(
    const std::string_view raw_name, const void* addr, const size_t length) {
#ifdef __APPLE__
  NoteUserModuleLoadProgress();
#endif
  auto name = xe::utf8::find_base_name_from_guest_path(raw_name);

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->Matches(name)) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromMemoryNamed(name, addr, length);
#ifdef __APPLE__
    NoteUserModuleLoadProgress();
#endif
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
#ifdef __APPLE__
    loaded_user_modules_count_.fetch_add(1, std::memory_order_relaxed);
    if (module && module->is_executable()) {
      loaded_executable_user_modules_count_.fetch_add(
          1, std::memory_order_relaxed);
    }
    NoteUserModuleLoadProgress();
#endif
  }
  return module;
}

X_RESULT KernelState::FinishLoadingUserModule(
    const object_ref<UserModule> module, bool call_entry) {
  // TODO(Gliniak): Apply custom patches here
  X_RESULT result = module->LoadContinue();
  if (XFAILED(result)) {
    return result;
  }
  module->Dump();
  emulator_->patcher()->ApplyPatchesForTitle(memory_, module->title_id(),
                                             module->hash());
  emulator_->on_patch_apply();
  if (module->xex_module()) {
    module->xex_module()->Precompile();
  }

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_ATTACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        1,  // DLL_PROCESS_ATTACH
        0,  // 0 because always dynamic
    };

    module->is_attached_ = true;

    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }
  return result;
}

X_RESULT KernelState::ApplyTitleUpdate(
    const object_ref<UserModule> title_module) {
  const auto title_updates = FindTitleUpdate(title_module->title_id());
  if (title_updates.empty()) {
    return X_STATUS_SUCCESS;
  }

  auto patch_module = LoadTitleUpdate(&title_updates.front(), title_module);
  if (!patch_module) {
    return X_STATUS_SUCCESS;
  }

  if (!patch_module->xex_module()->is_patch()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!IsPatchSignatureProper(title_module, patch_module)) {
    if (!cvars::allow_incompatible_title_update) {
      XELOGW(
          "Skipping incompatible title update for {} due to signature mismatch",
          title_module->name());
      return X_STATUS_SUCCESS;
    }

    // First module that is loaded is always main executable. That way we can
    // prevent random message spam in case of loading/unloading.
    if (!GetExecutableModule()) {
      emulator_->display_window()->app_context().CallInUIThread([&]() {
        new xe::ui::HostNotificationWindow(
            emulator_->imgui_drawer(), "Warning!",
            "Title Update signature doesn't match. This can cause unexpected "
            "issues or crashes!",
            0);
      });
    }
  }

  return ApplyTitleUpdate(title_module, patch_module);
}

std::vector<xam::XCONTENT_AGGREGATE_DATA> KernelState::FindTitleUpdate(
    const uint32_t title_id) const {
  if (!cvars::apply_title_update) {
    return {};
  }

  return xam_state_->content_manager()->ListContent(
      1, 0, title_id, xe::XContentType::kInstaller);
}

const object_ref<UserModule> KernelState::LoadTitleUpdate(
    const xam::XCONTENT_AGGREGATE_DATA* title_update,
    const object_ref<UserModule> module) {
  uint32_t disc_number = -1;
  if (module->is_multi_disc_title()) {
    disc_number = module->disc_number();
  }

  uint32_t content_license = 0;
  X_RESULT open_status = content_manager()->OpenContent(
      "UPDATE", 0, *title_update, content_license, disc_number);
#ifdef __APPLE__
  (void)open_status;
#endif

  std::string mount_path = "";
  if (!file_system()->FindSymbolicLink("game:", mount_path)) {
    return nullptr;
  }

  if (!module->path().starts_with(mount_path)) {
    return nullptr;
  }

  std::string resolved_path = "";
  if (!file_system()->FindSymbolicLink("UPDATE:", resolved_path)) {
    return nullptr;
  }

  const std::string relative_path =
      module->path().substr(mount_path.size() + 1) + 'p';

  xe::vfs::Entry* patch_entry =
      kernel_state()->file_system()->ResolvePath(resolved_path + relative_path);

  if (!patch_entry) {
    return nullptr;
  }

  const std::string patch_path = patch_entry->absolute_path();
  XELOGI("Loading XEX patch from {}", patch_path);
  auto patch_module = object_ref<UserModule>(new UserModule(this));

  X_RESULT result = patch_module->LoadFromFile(patch_path);
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to load XEX patch, code: {}", result);
    return nullptr;
  }

  return patch_module;
}

bool KernelState::IsPatchSignatureProper(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) const {
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  patch_module->GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR,
                             reinterpret_cast<void**>(&patch_header));

  assert_not_null(patch_header);

  // Compare hash inside delta descriptor to base XEX signature
  uint8_t digest[0x14];
  sha1::SHA1 s;
  s.processBytes(title_module->xex_module()->xex_security_info()->rsa_signature,
                 0x100);
  s.finalize(digest);

  if (memcmp(digest, patch_header->digest_source, 0x14) != 0) {
    XELOGW(
        "XEX patch signature hash doesn't match base XEX signature hash, patch "
        "will likely fail!");

    return false;
  }
  return true;
}

X_RESULT KernelState::ApplyTitleUpdate(
    const object_ref<UserModule> title_module,
    const object_ref<UserModule> patch_module) {
  if (!title_module) {
    XELOGE("{}: No title_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  if (!patch_module) {
    XELOGE("{}: No patch_module provided!", __FUNCTION__);
    return X_STATUS_UNSUCCESSFUL;
  }

  X_STATUS result =
      patch_module->xex_module()->ApplyPatch(title_module->xex_module());
  if (result != X_STATUS_SUCCESS) {
    XELOGE("Failed to apply XEX patch, code: {}", result);
  }
  return result;
}

void KernelState::UnloadUserModule(const object_ref<UserModule>& module,
                                   bool call_entry) {
  auto global_lock = global_critical_region_.Acquire();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_DETACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        0,  // DLL_PROCESS_DETACH
        0,  // 0 for now, assume XexUnloadImage is like FreeLibrary
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }

  auto iter = std::find_if(
      user_modules_.begin(), user_modules_.end(),
      [&module](const auto& e) { return e->path() == module->path(); });
  assert_true(iter != user_modules_.end());  // Unloading an unregistered module
                                             // is probably really bad
#ifdef __APPLE__
  loaded_user_modules_count_.fetch_sub(1, std::memory_order_relaxed);
  if (*iter && (*iter)->is_executable()) {
    loaded_executable_user_modules_count_.fetch_sub(1,
                                                    std::memory_order_relaxed);
  }
#endif
  user_modules_.erase(iter);

  // Ensure this module was not somehow registered twice
  assert_true(std::find_if(user_modules_.begin(), user_modules_.end(),
                           [&module](const auto& e) {
                             return e->path() == module->path();
                           }) == user_modules_.end());

  object_table()->ReleaseHandleInLock(module->handle());
}

void KernelState::TerminateTitle() {
  XELOGD("KernelState::TerminateTitle");
  auto global_lock = global_critical_region_.Acquire();

  // Call terminate routines.
  // TODO(benvanik): these might take arguments.
  // FIXME: Calling these will send some threads into kernel code and they'll
  // hold the lock when terminated! Do we need to wait for all threads to exit?
  /*
  if (from_guest_thread) {
    for (auto routine : terminate_notifications_) {
      auto thread_state = XThread::GetCurrentThread()->thread_state();
      processor()->Execute(thread_state, routine.guest_routine);
    }
  }
  terminate_notifications_.clear();
  */

  // Kill all guest threads.
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end();) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread()) {
      auto thread = it->second;

      if (thread->is_running()) {
        // Need to step the thread to a safe point (returns it to guest code
        // so it's guaranteed to not be holding any locks / in host kernel
        // code / etc). Can't do that properly if we have the lock.
        if (!emulator_->is_paused()) {
          thread->thread()->Suspend();
        }

        global_lock.unlock();
        processor_->StepToGuestSafePoint(thread->thread_id());
        thread->Terminate(0);
        global_lock.lock();
      }

      // Erase it from the thread list.
      it = threads_by_id_.erase(it);
    } else {
      ++it;
    }
  }

  // Third: Unload all user modules (including the executable).
  for (size_t i = 0; i < user_modules_.size(); i++) {
    X_STATUS status = user_modules_[i]->Unload();
    assert_true(XSUCCEEDED(status));

    object_table_.RemoveHandle(user_modules_[i]->handle());
  }
  user_modules_.clear();
#ifdef __APPLE__
  loaded_user_modules_count_.store(0, std::memory_order_relaxed);
  loaded_executable_user_modules_count_.store(0, std::memory_order_relaxed);
#endif

  // Release all objects in the object table.
  object_table_.PurgeAllObjects();

  // Unregister all notify listeners.
  notify_listeners_.clear();

  // Clear the TLS map.
  tls_bitmap_.Reset();

  // Unset the executable module.
  executable_module_ = nullptr;

  if (XThread::IsInThread()) {
    threads_by_id_.erase(XThread::GetCurrentThread()->thread_id());

    // Now commit suicide (using Terminate, because we can't call into guest
    // code anymore).
    global_lock.unlock();
    XThread::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::RegisterThread(XThread* thread) {
#ifdef XE_PLATFORM_WINDOWS
  auto global_lock = global_critical_region_.Acquire();
#endif
#ifdef __APPLE__
  X_HANDLE handle = thread ? thread->handle() : 0;
  if (handle == 0x01000010) {
    fprintf(stderr,
            "[kernel_state] RegisterThread enter handle=%08X thread_id=%u\n",
            handle, thread->thread_id());
    fflush(stderr);
  }

  if (handle != 0x01000010) {
    auto global_lock = global_critical_region_.Acquire();
    threads_by_id_[thread->thread_id()] = thread;
    return;
  }

  auto global_lock = global_critical_region_.TryAcquire();
  int wait_ms = 0;
  if (!global_lock.owns_lock()) {
    fprintf(
        stderr,
        "[kernel_state] RegisterThread waiting for global_critical_region_\n");
    fflush(stderr);
    while (!global_lock.try_lock()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      wait_ms += 10;
      if ((wait_ms % 500) == 0) {
        fprintf(stderr, "[kernel_state] RegisterThread still waiting (%d ms)\n",
                wait_ms);
        fflush(stderr);
      }
    }
  }

  fprintf(stderr, "[kernel_state] RegisterThread acquired lock after %d ms\n",
          wait_ms);
  fflush(stderr);
#endif

  threads_by_id_[thread->thread_id()] = thread;

#ifdef __APPLE__
  fprintf(stderr, "[kernel_state] RegisterThread done handle=%08X\n", handle);
  fflush(stderr);
#endif
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

void KernelState::OnThreadExecute(XThread* thread) {
#ifdef __APPLE__
  if (thread && thread->main_thread()) {
    XELOGI(
        "GUEST MAIN THREAD: KernelState::OnThreadExecute begin "
        "(thread_id={} handle={:08X})",
        thread->thread_id(), thread->handle());
  }
  EnsureExecutingThreadBindings(thread, "OnThreadExecute");
#endif
  auto global_lock = global_critical_region_.Acquire();
#ifdef __APPLE__
  if (thread && thread->main_thread()) {
    XELOGI(
        "GUEST MAIN THREAD: KernelState::OnThreadExecute acquired global "
        "kernel region (user_modules={})",
        user_modules_.size());
  }
#endif

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_ATTACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
#ifdef __APPLE__
      if (thread && thread->main_thread()) {
        XELOGI(
            "GUEST MAIN THREAD: KernelState::OnThreadExecute attaching DLL "
            "module='{}' entry={:08X} attached={}",
            user_module->name(), user_module->entry_point(),
            user_module->is_attached() ? "YES" : "NO");
      }
#endif
      uint64_t args[] = {
          user_module->handle(),
          user_module->is_attached()
              ? static_cast<uint64_t>(2)   // DLL_THREAD_ATTACH - Used to call
                                           // DLL for each thread created.
              : static_cast<uint64_t>(1),  // DLL_PROCESS_ATTACH - Used only
                                           // once for initialization.
          0,                               // 0 because always dynamic
      };

      user_module->is_attached_ = true;

      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
#ifdef __APPLE__
      if (thread && thread->main_thread()) {
        XELOGI(
            "GUEST MAIN THREAD: KernelState::OnThreadExecute DLL attach "
            "returned module='{}'",
            user_module->name());
      }
#endif
    }
  }
#ifdef __APPLE__
  if (thread && thread->main_thread()) {
    XELOGI("GUEST MAIN THREAD: KernelState::OnThreadExecute end");
  }
#endif
}

void KernelState::OnThreadExit(XThread* thread) {
#ifdef __APPLE__
  EnsureExecutingThreadBindings(thread, "OnThreadExit");
#endif
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_DETACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          3,  // DLL_THREAD_DETACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }

  emulator()->processor()->OnThreadExit(thread->thread_id());
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

std::vector<uint32_t> KernelState::GetAllThreadIDs() {
  auto global_lock = global_critical_region_.Acquire();

  auto thread_ids_view =
      threads_by_id_ |
      std::views::transform([](const auto& pair) { return pair.first; });

  std::vector<std::uint32_t> thread_ids(thread_ids_view.begin(),
                                        thread_ids_view.end());

  return thread_ids;
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & kXNotifySystem) {
    has_notified_startup_ = true;
    // XN_SYS_UI (on, off)
    listener->EnqueueNotification(kXNotificationSystemUI, 1);
    listener->EnqueueNotification(kXNotificationSystemUI, 0);
    // XN_SYS_SIGNINCHANGED x2
    listener->EnqueueNotification(kXNotificationSystemSignInChanged, 1);
    listener->EnqueueNotification(kXNotificationSystemSignInChanged, 1);
  }
  if (!has_notified_live_startup_ && listener->mask() & kXNotifyLive) {
    has_notified_live_startup_ = true;
    // X_ONLINE_S_LOGON_DISCONNECTED
    listener->EnqueueNotification(kXNotificationLiveConnectionChanged,
                                  0x001510F1L);
    listener->EnqueueNotification(kXNotificationLiveLinkStateChanged, 0);
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end();
       ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& notify_listener : notify_listeners_) {
    notify_listener->EnqueueNotification(id, data);
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0);
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error,
                                       uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr,
                                              X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr,
                                                X_RESULT result,
                                                uint32_t extended_error,
                                                uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr,
                               result, result, 0, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, uint32_t extended_error, uint32_t length,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XObject>(event_handle);

    assert_not_null(ev);
    if (ev && ev->type() == XObject::Type::Event) {
      ev.get<XEvent>()->Reset();
    }
  }
  auto global_lock = global_critical_region_.Acquire();
  dispatch_queue_.push_back([this, completion_callback, overlapped_ptr,
                             pre_callback, post_callback]() {
    if (pre_callback) {
      pre_callback();
    }
    // 5454082B infinitely loads free roam in netplay without sleep.
    xe::threading::Sleep(kDeferredOverlappedDelayMillis);
    uint32_t extended_error, length;
    auto result = completion_callback(extended_error, length);
    CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
    if (post_callback) {
      post_callback();
    }
  });
  dispatch_cond_.notify_all();
}

bool KernelState::Save(ByteStream* stream) {
  XELOGD("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Write the TLS allocation bitmap
  auto tls_bitmap = tls_bitmap_.data();
  stream->Write(uint32_t(tls_bitmap.size()));
  for (size_t i = 0; i < tls_bitmap.size(); i++) {
    stream->Write<uint64_t>(tls_bitmap[i]);
  }

  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  XELOGD("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      XELOGD("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  XELOGD("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    if (object->is_host_object() || object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      XELOGD("Did not save object of type {}",
             static_cast<uint32_t>(object->type()));
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

// this only gets triggered once per ms at most, so fields other than tick count
// will probably not be updated in a timely manner for guest code that uses them
void KernelState::UpdateKeTimestampBundle() {
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ke_timestamp_bundle_ptr_);
  uint32_t uptime_ms = Clock::QueryGuestUptimeMillis();
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());
  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());
  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count, uptime_ms);
}

uint32_t KernelState::GetKeTimestampBundle() {
  XE_LIKELY_IF(ke_timestamp_bundle_ptr_) { return ke_timestamp_bundle_ptr_; }
  else {
    global_critical_region::PrepareToAcquire();
    return CreateKeTimestampBundle();
  }
}

XE_NOINLINE
XE_COLD
uint32_t KernelState::CreateKeTimestampBundle() {
  auto crit = global_critical_region::Acquire();

  uint32_t pKeTimeStampBundle =
      memory_->SystemHeapAlloc(sizeof(X_TIME_STAMP_BUNDLE));
  X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
      memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(pKeTimeStampBundle);

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->interrupt_time,
                               Clock::QueryGuestInterruptTime());

  xe::store_and_swap<uint64_t>(&lpKeTimeStampBundle->system_time,
                               Clock::QueryGuestSystemTime());

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->tick_count,
                               Clock::QueryGuestUptimeMillis());

  xe::store_and_swap<uint32_t>(&lpKeTimeStampBundle->padding, 0);

  ke_timestamp_bundle_ptr_ = pKeTimeStampBundle;
  timestamp_timer_ = xe::threading::HighResolutionTimer::CreateRepeating(
      std::chrono::milliseconds(1),
      [this]() { this->UpdateKeTimestampBundle(); });
  return pKeTimeStampBundle;
}

bool KernelState::Restore(ByteStream* stream) {
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table
  object_table_.Restore(stream);

  // Read the TLS allocation bitmap
  auto num_bitmap_entries = stream->Read<uint32_t>();
  auto& tls_bitmap = tls_bitmap_.data();
  tls_bitmap.resize(num_bitmap_entries);
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    tls_bitmap[i] = stream->Read<uint64_t>();
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  XELOGD("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  XELOGD("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  return true;
}

std::bitset<4> KernelState::GetConnectedUsers() const {
  auto input_sys = emulator_->input_system();

  auto lock = input_sys->lock();

  return input_sys->GetConnectedSlots();
}
// todo: definitely need to do more to pretend to be in a dpc
void KernelState::BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                                        DPCImpersonationScope& scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 0);
  scope.previous_irql_ = kpcr->current_irql;

  kpcr->current_irql = 2;
  kpcr->prcb_data.dpc_active = 1;
}
void KernelState::EndDPCImpersonation(cpu::ppc::PPCContext* context,
                                      DPCImpersonationScope& end_scope) {
  auto kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  xenia_assert(kpcr->prcb_data.dpc_active == 1);
  kpcr->current_irql = end_scope.previous_irql_;
  kpcr->prcb_data.dpc_active = 0;
}
void KernelState::EmulateCPInterruptDPC(uint32_t interrupt_callback,
                                        uint32_t interrupt_callback_data,
                                        uint32_t source, uint32_t cpu) {
  if (!interrupt_callback) {
    return;
  }
#ifdef __APPLE__
  if (cvars::kernel_validate_interrupt_callback_before_execute) {
    if ((interrupt_callback & 0x3) != 0) {
      XELOGW(
          "   CP interrupt DPC skipped: callback not 4-byte aligned "
          "callback={:08X} source={} cpu={}",
          interrupt_callback, source, cpu);
      return;
    }
    auto* callback_fn = processor_->LookupFunction(interrupt_callback);
    if (!callback_fn) {
      callback_fn = processor_->ResolveFunction(interrupt_callback);
    }
    if (!callback_fn) {
      XELOGW(
          "   CP interrupt DPC skipped: callback unresolved callback={:08X} "
          "source={} cpu={} data={:08X}",
          interrupt_callback, source, cpu, interrupt_callback_data);
      return;
    }
  }
#endif

  auto thread = kernel::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  /*
    in reality, our interrupt is a callback that is called in a dpc which is
    scheduled by the actual interrupt

    we need to impersonate a dpc
  */
  auto current_context = thread->thread_state()->context();
  auto kthread = memory()->TranslateVirtual<X_KTHREAD*>(thread->guest_object());
#ifdef __APPLE__
  (void)kthread;
#endif

  auto pcr = memory()->TranslateVirtual<X_KPCR*>(thread->pcr_ptr());
#ifdef __APPLE__
  (void)pcr;
#endif

  DPCImpersonationScope dpc_scope{};
  BeginDPCImpersonation(current_context, dpc_scope);

  // todo: check VdGlobalXamDevice here. if VdGlobalXamDevice is nonzero, should
  // set X_PROCTYPE_SYSTEM
  xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_TITLE, current_context);

  uint64_t args[] = {source, interrupt_callback_data};
  processor_->Execute(thread->thread_state(), interrupt_callback, args,
                      xe::countof(args));
  xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_IDLE, current_context);

  EndDPCImpersonation(current_context, dpc_scope);
}

void KernelState::InitializeProcess(X_KPROCESS* process, uint32_t type,
                                    char unk_18, char unk_19, char unk_1A) {
  uint32_t guest_kprocess = memory()->HostToGuestVirtual(process);

  uint32_t thread_list_guest_ptr =
      guest_kprocess + offsetof(X_KPROCESS, thread_list);

  process->unk_18 = unk_18;
  process->unk_19 = unk_19;
  process->unk_1A = unk_1A;
#ifdef __APPLE__
  // Defensive init: ensure spinlock starts unlocked even if memory isn't
  // zeroed.
  process->thread_list_spinlock.prcb_of_owner = 0;
#endif
  util::XeInitializeListHead(&process->thread_list, thread_list_guest_ptr);
  process->quantum = 60;
  // doubt any guest code uses this ptr, which i think probably has something to
  // do with the page table
  process->clrdataa_masked_ptr = 0;
  // clrdataa_ & ~(1U << 31);
  process->thread_count = 0;
  process->unk_1B = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;

  process->process_type = type;
  uint32_t unk_list_guest_ptr = guest_kprocess + offsetof(X_KPROCESS, unk_54);
  // TODO(benvanik): figure out what this list is.
  util::XeInitializeListHead(&process->unk_54, unk_list_guest_ptr);
}

void KernelState::SetProcessTLSVars(X_KPROCESS* process, int num_slots,
                                    int tls_data_size,
                                    int tls_static_data_address) {
  uint32_t slots_padded = (num_slots + 3) & 0xFFFFFFFC;
  process->tls_data_size = tls_data_size;
  process->tls_raw_data_size = tls_data_size;
  process->tls_static_data_address = tls_static_data_address;
  process->tls_slot_size = 4 * slots_padded;
  uint32_t count_div32 = slots_padded / 32;
  for (unsigned word_index = 0; word_index < count_div32; ++word_index) {
    process->bitmap[word_index] = -1;
  }

  // set remainder of bitset
  if (((num_slots + 3) & 0x1C) != 0)
    process->bitmap[count_div32] = -1 << (32 - ((num_slots + 3) & 0x1C));
}
void AllocateThread(PPCContext* context) {
  uint32_t thread_mem_size = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  if (thread_mem_size <= 0xFD8) thread_mem_size += 8;
  uint32_t result =
      xboxkrnl::xeAllocatePoolTypeWithTag(context, thread_mem_size, a2, a3);
  if (((unsigned short)result & 0xFFF) != 0) {
    result += 2;
  }

  context->r[3] = static_cast<uint64_t>(result);
}
void FreeThread(PPCContext* context) {
  uint32_t thread_memory = static_cast<uint32_t>(context->r[3]);
  if ((thread_memory & 0xFFF) != 0) {
    thread_memory -= 8;
  }
  xboxkrnl::xeFreePool(context, thread_memory);
}

void SimpleForwardAllocatePoolTypeWithTag(PPCContext* context) {
  uint32_t a1 = static_cast<uint32_t>(context->r[3]);
  uint32_t a2 = static_cast<uint32_t>(context->r[4]);
  uint32_t a3 = static_cast<uint32_t>(context->r[5]);
  context->r[3] = static_cast<uint64_t>(
      xboxkrnl::xeAllocatePoolTypeWithTag(context, a1, a2, a3));
}
void SimpleForwardFreePool(PPCContext* context) {
  xboxkrnl::xeFreePool(context, static_cast<uint32_t>(context->r[3]));
}

void DeleteMutant(PPCContext* context) {
  // todo: this should call kereleasemutant with some specific args

  xe::FatalError("DeleteMutant - need KeReleaseMutant(mutant, 1, 1, 0) ");
}
void DeleteTimer(PPCContext* context) {
  // todo: this should call KeCancelTimer
  xe::FatalError("DeleteTimer - need KeCancelTimer(mutant, 1, 1, 0) ");
}

void DeleteIoCompletion(PPCContext* context) {}

void UnknownProcIoDevice(PPCContext* context) {}

void CloseFileProc(PPCContext* context) {}

void DeleteFileProc(PPCContext* context) {}

void UnknownFileProc(PPCContext* context) {}

void DeleteSymlink(PPCContext* context) {
  X_KSYMLINK* lnk = context->TranslateVirtualGPR<X_KSYMLINK*>(context->r[3]);

  context->r[3] = lnk->refed_object_maybe;
  xboxkrnl::xeObDereferenceObject(context, lnk->refed_object_maybe);
}
void KernelState::InitializeKernelGuestGlobals() {
  kernel_guest_globals_ = memory_->SystemHeapAlloc(sizeof(KernelGuestGlobals));

  KernelGuestGlobals* block =
      memory_->TranslateVirtual<KernelGuestGlobals*>(kernel_guest_globals_);
  SecureZeroMemory(block, sizeof(KernelGuestGlobals));

  auto idle_process = memory()->TranslateVirtual<X_KPROCESS*>(GetIdleProcess());
  InitializeProcess(idle_process, X_PROCTYPE_IDLE, 0, 0, 0);
  idle_process->quantum = 0x7F;
  auto system_process =
      memory()->TranslateVirtual<X_KPROCESS*>(GetSystemProcess());
  InitializeProcess(system_process, X_PROCTYPE_SYSTEM, 2, 5, 9);
  SetProcessTLSVars(system_process, 32, 0, 0);

  uint32_t oddobject_offset =
      kernel_guest_globals_ + offsetof(KernelGuestGlobals, OddObj);

  // init unknown object

  block->OddObj.field0 = 0x1000000;
  block->OddObj.field4 = 1;
  block->OddObj.points_to_self =
      oddobject_offset + offsetof(X_UNKNOWN_TYPE_REFED, points_to_self);
  block->OddObj.points_to_prior = block->OddObj.points_to_self;

  // init thread object
  block->ExThreadObjectType.pool_tag = 0x65726854;
  block->ExThreadObjectType.allocate_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(AllocateThread);

  block->ExThreadObjectType.free_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(FreeThread);

  // several object types just call freepool/allocatepool
  uint32_t trampoline_allocatepool =
      kernel_trampoline_group_.NewLongtermTrampoline(
          SimpleForwardAllocatePoolTypeWithTag);
  uint32_t trampoline_freepool =
      kernel_trampoline_group_.NewLongtermTrampoline(SimpleForwardFreePool);

  // init event object
  block->ExEventObjectType.pool_tag = 0x76657645;
  block->ExEventObjectType.allocate_proc = trampoline_allocatepool;
  block->ExEventObjectType.free_proc = trampoline_freepool;

  // init mutant object
  block->ExMutantObjectType.pool_tag = 0x6174754D;
  block->ExMutantObjectType.allocate_proc = trampoline_allocatepool;
  block->ExMutantObjectType.free_proc = trampoline_freepool;

  block->ExMutantObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteMutant);
  // init semaphore obj
  block->ExSemaphoreObjectType.pool_tag = 0x616D6553;
  block->ExSemaphoreObjectType.allocate_proc = trampoline_allocatepool;
  block->ExSemaphoreObjectType.free_proc = trampoline_freepool;
  // init timer obj
  block->ExTimerObjectType.pool_tag = 0x656D6954;
  block->ExTimerObjectType.allocate_proc = trampoline_allocatepool;
  block->ExTimerObjectType.free_proc = trampoline_freepool;
  block->ExTimerObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteTimer);
  // iocompletion object
  block->IoCompletionObjectType.pool_tag = 0x706D6F43;
  block->IoCompletionObjectType.allocate_proc = trampoline_allocatepool;
  block->IoCompletionObjectType.free_proc = trampoline_freepool;
  block->IoCompletionObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteIoCompletion);
  block->IoCompletionObjectType.unknown_size_or_object_ = oddobject_offset;

  // iodevice object
  block->IoDeviceObjectType.pool_tag = 0x69766544;
  block->IoDeviceObjectType.allocate_proc = trampoline_allocatepool;
  block->IoDeviceObjectType.free_proc = trampoline_freepool;
  block->IoDeviceObjectType.unknown_size_or_object_ = oddobject_offset;
  block->IoDeviceObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownProcIoDevice);

  // file object
  block->IoFileObjectType.pool_tag = 0x656C6946;
  block->IoFileObjectType.allocate_proc = trampoline_allocatepool;
  block->IoFileObjectType.free_proc = trampoline_freepool;
  block->IoFileObjectType.unknown_size_or_object_ =
      0x38;  // sizeof fileobject, i believe
  block->IoFileObjectType.close_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(CloseFileProc);
  block->IoFileObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteFileProc);
  block->IoFileObjectType.unknown_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(UnknownFileProc);

  // directory object
  block->ObDirectoryObjectType.pool_tag = 0x65726944;
  block->ObDirectoryObjectType.allocate_proc = trampoline_allocatepool;
  block->ObDirectoryObjectType.free_proc = trampoline_freepool;
  block->ObDirectoryObjectType.unknown_size_or_object_ = oddobject_offset;

  // symlink object
  block->ObSymbolicLinkObjectType.pool_tag = 0x626D7953;
  block->ObSymbolicLinkObjectType.allocate_proc = trampoline_allocatepool;
  block->ObSymbolicLinkObjectType.free_proc = trampoline_freepool;
  block->ObSymbolicLinkObjectType.unknown_size_or_object_ = oddobject_offset;
  block->ObSymbolicLinkObjectType.delete_proc =
      kernel_trampoline_group_.NewLongtermTrampoline(DeleteSymlink);

#define offsetof32(s, m) static_cast<uint32_t>(offsetof(s, m))

  host_object_type_enum_to_guest_object_type_ptr_ = {
      {XObject::Type::Event,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExEventObjectType)},
      {XObject::Type::Semaphore,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExSemaphoreObjectType)},
      {XObject::Type::Thread,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExThreadObjectType)},
      {XObject::Type::File,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoFileObjectType)},
      {XObject::Type::Mutant,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, ExMutantObjectType)},
      {XObject::Type::Device,
       kernel_guest_globals_ +
           offsetof32(KernelGuestGlobals, IoDeviceObjectType)}};
  xboxkrnl::xeKeSetEvent(&block->UsbdBootEnumerationDoneEvent, 1, 0);
}

void KernelState::InitializeXbdmCpuCounters() {
  constexpr uint32_t counters_base_address = 0x91F00000;

  // These are not confirmed and there seems to be multiple types of counters,
  // but no idea how they're switched. For now this seems to be good enough.
  constexpr std::array<const char*, 0x11> xbdm_counters = {
      "load-hit-stores (S)",
      "instructions committed",
      "i-cache miss cycles",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "core 0 L2 data misses",
      "Bad counter number - must be 0-15."};

  auto xbdm_range = memory_->LookupHeap(counters_base_address);
  if (!xbdm_range->AllocFixed(
          counters_base_address, 0x1000, 0,
          kMemoryAllocationCommit | kMemoryAllocationReserve,
          kMemoryProtectRead | kMemoryProtectWrite)) {
    return;
  }

  uint32_t address = counters_base_address;

  for (size_t i = 0; i < xbdm_counters.size(); i++) {
    xbdm_counters_address[i] = address;
    const std::string entry = xbdm_counters[i];
    std::memcpy(memory_->TranslateVirtual<char*>(address), entry.c_str(),
                entry.size());
    address += static_cast<uint32_t>(entry.size()) + 1;
  }
}

}  // namespace kernel
}  // namespace xe
