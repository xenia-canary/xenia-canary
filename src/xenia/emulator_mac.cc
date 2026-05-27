/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <mutex>
#include <ranges>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

#include "xenia/emulator_mac.h"

#include "config.h"
#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tabulate/single_include/tabulate/tabulate.hpp"
#include "third_party/zarchive/include/zarchive/zarchivecommon.h"
#include "third_party/zarchive/include/zarchive/zarchivewriter.h"
#include "third_party/zarchive/src/sha_256.h"
#include "xenia/apu/audio_system.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/mapped_memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/backend/null_backend.h"
#include "xenia/cpu/cpu_flags_mac.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_opcode_info_mac.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/gpu/command_processor_mac.h"
#include "xenia/gpu/graphics_system_mac.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/title_id_utils_mac.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/xam/achievement_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xbdm/xbdm_module.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xobject_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules_mac.h"
#include "xenia/kernel/xboxkrnl/debug_monitor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading_mac.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/memory_mac.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/window_mac.h"
#include "xenia/ui/windowed_app_context_mac.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_zarchive_device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/virtual_file_system.h"

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend_mac.h"
#endif  // XE_ARCH

DEFINE_double(time_scalar, 1.0,
              "Scalar used to speed or slow time (1x, 2x, 1/2x, etc).",
              "General");

DEFINE_string(
    launch_module, "",
    "Executable to launch from the .iso or the package instead of default.xex "
    "or the module specified by the game. Leave blank to launch the default "
    "module.",
    "General");

DEFINE_uint32(
    complete_launch_load_user_module_attempts, 3,
    "Number of KernelState::LoadUserModule attempts during CompleteLaunch "
    "(minimum 1).",
    "Debug");
DEFINE_uint32(
    complete_launch_load_user_module_retry_delay_ms, 250,
    "Delay in milliseconds between CompleteLaunch LoadUserModule retries.",
    "Debug");
DEFINE_bool(
  complete_launch_load_user_module_wait_until_success, true,
  "If true, CompleteLaunch halts progression and keeps retrying "
  "LoadUserModule until it succeeds (or times out).",
  "Debug");
DEFINE_uint32(
  complete_launch_load_user_module_wait_timeout_ms, 0,
  "Maximum time in milliseconds to keep the CompleteLaunch module-load "
  "gate active while waiting for LoadUserModule success (0 = no timeout).",
  "Debug");
DEFINE_uint32(
  complete_launch_load_user_module_heartbeat_interval_ms, 1000,
  "Heartbeat interval in milliseconds while CompleteLaunch is halted "
  "waiting for LoadUserModule success (0 = log every failed attempt).",
  "Debug");
DEFINE_bool(
  complete_launch_global_module_gate_enable, true,
  "Close a global runtime gate while CompleteLaunch is in module-load "
  "critical section and reopen only after full module readiness.",
  "Debug");
#if XE_PLATFORM_MAC
constexpr bool kDefaultCompleteLaunchBlockingShaderStorage = false;
#else
constexpr bool kDefaultCompleteLaunchBlockingShaderStorage = true;
#endif
DEFINE_bool(
    complete_launch_blocking_shader_storage,
    kDefaultCompleteLaunchBlockingShaderStorage,
    "Initialize shader storage in blocking mode during CompleteLaunch. "
    "Disabling avoids long UI-thread stalls in RunTitle and lets shader "
    "storage initialize asynchronously.",
    "Debug");
DEFINE_bool(
    log_game_info_database_tables, false,
    "Log verbose tabulated game-info database tables during launch "
    "(achievements/properties/contexts/stats/presence).",
    "Debug");

DEFINE_bool(log_main_thread_pc, false,
            "Log main thread guest PC periodically while waiting for exit.",
            "Debug");
DEFINE_uint32(log_main_thread_pc_interval_ms, 1000,
              "Interval in ms between main thread PC samples.", "Debug");
DEFINE_uint32(
    log_main_thread_pc_detail_interval_ms, 60000,
    "Minimum interval in ms between detailed main thread PC dumps for the "
    "same stable PC. Set to 0 to only dump once per PC.",
    "Debug");
DEFINE_uint32(
    log_main_thread_pc_loop_address, 0,
    "If non-zero, enable extra profiling when the main thread PC matches this "
    "address.",
    "Debug");
DEFINE_uint32(
    log_main_thread_pc_loop_interval_ms, 2000,
    "Minimum interval in ms between PC loop profiles when the main thread PC "
    "matches log_main_thread_pc_loop_address.",
    "Debug");
DEFINE_uint32(
    log_main_thread_pc_loop_disasm_count, 16,
    "Number of instructions to disassemble around the loop PC when profiling "
    "is enabled.",
    "Debug");
DEFINE_bool(
    log_main_thread_pc_loop_dump_stack, true,
    "Dump a stack window when the main thread PC matches the loop profile "
    "address.",
    "Debug");
DEFINE_bool(
    log_main_thread_wait_profile, false,
    "Profile WaitUntilExit wait results and timing.",
    "Debug");
DEFINE_uint32(
    log_main_thread_wait_profile_interval_ms, 5000,
    "Interval in ms between WaitUntilExit profiling summaries.",
    "Debug");
DEFINE_bool(
  log_main_thread_stall_wait_nudge, true,
  "When main thread PC is stable on known GPU bootstrap stall targets, "
  "attempt periodic wait-loop nudges (resume + watched event set).",
  "Debug");
DEFINE_uint32(
  log_main_thread_stall_wait_nudge_after_samples, 10,
  "Minimum stable-PC sample count before main-thread stall wait nudges "
  "activate.",
  "Debug");
DEFINE_uint32(
  log_main_thread_stall_wait_nudge_interval_ms, 2000,
  "Minimum interval between main-thread stall wait nudges.",
  "Debug");
DEFINE_bool(
  log_main_thread_stall_gpu_bootstrap_nudge, true,
  "When main thread is stable at known GPU bootstrap stall PCs, issue a "
  "throttled GPU bootstrap nudge if callback is set but producer entry "
  "(VdSwap path) is still missing.",
  "Debug");
DEFINE_uint32(
  log_main_thread_stall_gpu_bootstrap_nudge_after_samples, 10,
  "Minimum stable-PC sample count before GPU bootstrap nudges activate.",
  "Debug");
DEFINE_uint32(
  log_main_thread_stall_gpu_bootstrap_nudge_interval_ms, 1500,
  "Minimum interval between main-thread GPU bootstrap nudges.",
  "Debug");
DEFINE_uint32(
  log_main_thread_stall_gpu_bootstrap_nudge_max_swap_packets, 1,
  "Only issue main-thread GPU bootstrap nudge if command processor swap "
  "packet count is at or below this value.",
  "Debug");
DEFINE_bool(
  log_main_thread_stall_force_ring_init_nudge, true,
  "When main-thread stall GPU nudge triggers, force CP ring init from the "
  "host path even if fallback policy would suppress it.",
  "Debug");
DEFINE_bool(
  log_main_thread_stall_debug_monitor_nudge, true,
  "When stable on known bootstrap stall PCs, validate and repair "
  "KeDebugMonitorData pointer wiring if it appears null/invalid.",
  "Debug");
DEFINE_uint32(
  log_main_thread_stall_debug_monitor_nudge_interval_ms, 2000,
  "Minimum interval between KeDebugMonitorData repair attempts from the "
  "main-thread stall path.",
  "Debug");

DECLARE_bool(track_guest_pc);

DEFINE_bool(allow_game_relative_writes, false,
            "Not useful to non-developers. Allows code to write to paths "
            "relative to game://. Used for "
            "generating test data to compare with original hardware. ",
            "General");

DECLARE_int32(user_language);

DECLARE_bool(allow_plugins);

DEFINE_int32(priority_class, 0,
             "Forces Xenia to use different process priority than default one. "
             "It might affect performance and cause unexpected bugs. Possible "
             "values: 0 - Normal, 1 - Above normal, 2 - High",
             "General");

namespace xe {
using namespace xe::literals;

namespace {
template <typename T>
struct SafeVirtualTranslation {
  bool success = false;
  T pointer = nullptr;
  std::string error;
};

template <typename T>
SafeVirtualTranslation<T> TranslateVirtualSafe(xe::Memory* memory,
                                               uint32_t guest_address) {
  static_assert(std::is_pointer_v<T>);
  SafeVirtualTranslation<T> result;
  if (!memory) {
    result.error = "memory unavailable";
    return result;
  }

  constexpr uint32_t kAccessSize =
      static_cast<uint32_t>(sizeof(std::remove_pointer_t<T>));
  uint32_t access_end = guest_address + kAccessSize;
  if (access_end < guest_address) {
    result.error = "address overflow";
    return result;
  }

  auto* heap = memory->LookupHeap(guest_address);
  if (!heap) {
    result.error = "heap unavailable";
    return result;
  }

  xe::HeapAllocationInfo info = {};
  if (!heap->QueryRegionInfo(guest_address, &info)) {
    result.error = "region query failed";
    return result;
  }
  uint32_t region_end = info.base_address + info.region_size;
  if ((info.state & xe::kMemoryAllocationCommit) == 0 ||
      region_end < info.base_address || access_end > region_end) {
    result.error = "region unavailable";
    return result;
  }

  result.pointer = memory->TranslateVirtual<T>(guest_address);
  result.success = result.pointer != nullptr;
  if (!result.success) {
    result.error = "translation failed";
  }
  return result;
}

template <typename T>
SafeVirtualTranslation<T> TranslateVirtualSafe(
    const std::unique_ptr<xe::Memory>& memory, uint32_t guest_address) {
  return TranslateVirtualSafe<T>(memory.get(), guest_address);
}

bool DecodeBranchTarget(uint32_t pc, uint32_t instr, uint32_t* target_out,
                        bool* link_out, bool* absolute_out) {
  uint32_t opcode = instr >> 26;
  if (opcode != 18) {
    return false;
  }
  bool aa = (instr & 0x2) != 0;
  bool lk = (instr & 0x1) != 0;
  uint32_t li = instr & 0x03FFFFFC;
  // Sign-extend 26-bit value.
  if (li & 0x02000000) {
    li |= 0xFC000000;
  }
  uint32_t target = aa ? li : pc + li;
  if (target_out) {
    *target_out = target;
  }
  if (link_out) {
    *link_out = lk;
  }
  if (absolute_out) {
    *absolute_out = aa;
  }
  return true;
}

constexpr bool IsGpuBootstrapStallTarget(uint32_t address) {
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

int GpuDispatchStallStatePriority(const std::string& state) {
  if (state == "critical_dispatch_window") {
    return 100;
  }
  if (state == "title_local_privilege_gate_before_gpu_dispatch") {
    return 90;
  }
  if (state == "title_local_busy_loop_without_import_dispatch") {
    return 80;
  }
  if (state == "title_local_busy_loop_before_gpu_dispatch") {
    return 70;
  }
  if (state == "busy_loop_before_gpu_dispatch") {
    return 60;
  }
  if (state == "gpu_bootstrap_entry_loop_before_callback") {
    return 50;
  }
  if (state == "startup_prolog_helper_only") {
    return 40;
  }
  if (state == "non_gpu_call_chain") {
    return 30;
  }
  if (state == "guest_main_context_not_ready") {
    return 20;
  }
  if (state == "no_branch_window_activity") {
    return 10;
  }
  return 0;
}

struct GpuDispatchStallSnapshot {
  std::mutex mutex;
  bool valid = false;
  std::string state;
  std::string detail;
  std::string chain;
  std::string target_summaries;
  uint32_t repeat_samples = 0;
  uint64_t now_ms = 0;
  uint64_t state_first_ms = 0;
  uint32_t zero_context_samples = 0;
  uint32_t valid_context_samples = 0;
  uint32_t last_nonzero_pc = 0;
  uint32_t last_nonzero_lr = 0;
  std::string last_nonzero_state;
  std::string last_nonzero_chain;
  std::string last_nonzero_targets;
  uint64_t last_nonzero_ms = 0;
};

GpuDispatchStallSnapshot& GetGpuDispatchStallSnapshotStorage();
}  // namespace

Emulator::GameConfigLoadCallback::GameConfigLoadCallback(Emulator& emulator)
    : emulator_(emulator) {
  emulator_.AddGameConfigLoadCallback(this);
}

Emulator::GameConfigLoadCallback::~GameConfigLoadCallback() {
  emulator_.RemoveGameConfigLoadCallback(this);
}

Emulator::Emulator(const std::filesystem::path& command_line,
                   const std::filesystem::path& storage_root,
                   const std::filesystem::path& content_root,
                   const std::filesystem::path& cache_root)
    : on_launch(),
      on_terminate(),
      on_exit(),
      command_line_(command_line),
      storage_root_(storage_root),
      content_root_(content_root),
      cache_root_(cache_root),
      title_name_(),
      title_version_(),
      display_window_(nullptr),
      memory_(),
      audio_system_(),
      audio_media_player_(),
      graphics_system_(),
      input_system_(),
      export_resolver_(),
      file_system_(),
      kernel_state_(),
      main_thread_(),
      title_id_(std::nullopt),
      game_info_database_(),
      paused_(false),
      restoring_(false),
      restore_fence_() {
  if (cvars::priority_class != 0) {
    if (SetProcessPriorityClass(cvars::priority_class)) {
      XELOGI("Higher priority class request: Successful. New priority: {}",
             cvars::priority_class);
    }
  }

#if XE_PLATFORM_WIN32 == 1
  // Show a disclaimer that links to the quickstart
  // guide the first time they ever open the emulator
  uint64_t persistent_flags = GetPersistentEmulatorFlags();
  if (!(persistent_flags & EmulatorFlagDisclaimerAcknowledged)) {
    if ((MessageBoxW(
             nullptr,
             L"DISCLAIMER: Xenia is not for enabling illegal activity, and "
             "support is unavailable for illegally obtained software.\n\n"
             "Please respect this policy as no further reminders will be "
             "given.\n\nThe quickstart guide explains how to use digital or "
             "physical games from your Xbox 360 console.\n\nWould you like "
             "to open it?",
             L"Xenia", MB_YESNO | MB_ICONQUESTION) == IDYES)) {
      LaunchWebBrowser(
          "https://github.com/xenia-canary/xenia-canary/wiki/"
          "Quickstart#how-to-rip-games");
    }
    SetPersistentEmulatorFlags(persistent_flags |
                               EmulatorFlagDisclaimerAcknowledged);
  }
#endif
}

Emulator::~Emulator() {
  // Note that we delete things in the reverse order they were initialized.

  // Give the systems time to shutdown before we delete them.
  if (graphics_system_) {
    graphics_system_->Shutdown();
  }
  if (audio_system_) {
    audio_system_->Shutdown();
  }

  input_system_.reset();
  graphics_system_.reset();
  audio_system_.reset();
  audio_media_player_.reset();

  kernel_state_.reset();
  file_system_.reset();

  processor_.reset();

  export_resolver_.reset();

  ExceptionHandler::Uninstall(Emulator::ExceptionCallbackThunk, this);
}

X_STATUS Emulator::Setup(
    ui::Window* display_window, ui::ImGuiDrawer* imgui_drawer,
    bool require_cpu_backend,
    std::function<std::unique_ptr<apu::AudioSystem>(cpu::Processor*)>
        audio_system_factory,
    std::function<std::unique_ptr<gpu::GraphicsSystem>()>
        graphics_system_factory,
    std::function<std::vector<std::unique_ptr<hid::InputDriver>>(ui::Window*)>
        input_driver_factory) {
  X_STATUS result = X_STATUS_UNSUCCESSFUL;

  display_window_ = display_window;
  imgui_drawer_ = imgui_drawer;

  // Initialize clock.
  // 360 uses a 50MHz clock.
  Clock::set_guest_tick_frequency(50000000);
  // We could reset this with save state data/constant value to help replays.
  Clock::set_guest_system_time_base(Clock::QueryHostSystemTime());
  // This can be adjusted dynamically, as well.
  Clock::set_guest_time_scalar(cvars::time_scalar);

  // Before we can set thread affinity we must enable the process to use all
  // logical processors.
  xe::threading::EnableAffinityConfiguration();

  XELOGI("{}: Initializing Memory...", __func__);
  // Create memory system first, as it is required for other systems.
  memory_ = std::make_unique<Memory>();
  if (!memory_->Initialize()) {
    XELOGE("{}: Cannot initalize memory!", __func__);
    return result;
  }

  XELOGI("{}: Initializing Exports...", __func__);
  // Shared export resolver used to attach and query for HLE exports.
  export_resolver_ = std::make_unique<xe::cpu::ExportResolver>();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#endif  // XE_ARCH
  if (cvars::cpu == "any") {
    if (!backend) {
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#endif  // XE_ARCH
    }
  }
  if (!backend && !require_cpu_backend) {
    backend.reset(new xe::cpu::backend::NullBackend());
  }

  XELOGI("{}: Initializing Processor...", __func__);
  // Initialize the CPU.
  processor_ = std::make_unique<xe::cpu::Processor>(memory_.get(),
                                                    export_resolver_.get());
  XELOGI("{}: Calling processor_->Setup()...", __func__);
  if (!processor_->Setup(std::move(backend))) {
    XELOGE("{}: Cannot initalize processor!", __func__);
    return X_STATUS_UNSUCCESSFUL;
  }
  XELOGI("{}: Processor setup completed successfully", __func__);

  XELOGI("{}: Initializing Audio...", __func__);
  // Initialize the APU.
  if (audio_system_factory) {
    XELOGI("{}: Creating audio system...", __func__);
    audio_system_ = audio_system_factory(processor_.get());
    if (!audio_system_) {
      XELOGE("{}: Cannot initalize audio_system!", __func__);
      return X_STATUS_NOT_IMPLEMENTED;
    }
    XELOGI("{}: Audio system created successfully", __func__);
  } else {
    XELOGI("{}: No audio system factory provided, skipping audio setup",
           __func__);
  }

  XELOGI("{}: Initializing Graphics...", __func__);
  // Initialize the GPU.
  XELOGI("{}: Creating graphics system...", __func__);
  graphics_system_ = graphics_system_factory();
  if (!graphics_system_) {
    XELOGE("{}: Cannot initalize graphics_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }
  XELOGI("{}: Graphics system created successfully", __func__);

  XELOGI("{}: Initializing HID...", __func__);
  // Initialize the HID.
  XELOGI("{}: Creating input system...", __func__);
  input_system_ = std::make_unique<xe::hid::InputSystem>(display_window_);
  if (!input_system_) {
    XELOGE("{}: Cannot initalize input_system!", __func__);
    return X_STATUS_NOT_IMPLEMENTED;
  }
  if (input_driver_factory) {
    XELOGI("{}: Adding input drivers...", __func__);
    auto input_drivers = input_driver_factory(display_window_);
    for (size_t i = 0; i < input_drivers.size(); ++i) {
      input_system_->AddDriver(std::move(input_drivers[i]));
    }
    XELOGI("{}: Added {} input drivers", __func__, input_drivers.size());
  }
  XELOGI("{}: Input system created successfully", __func__);

  XELOGI("{}: Setting up input system...", __func__);
  result = input_system_->Setup();
  if (result) {
    XELOGE("{}: Input system setup failed with result: {:08X}", __func__,
           result);
    return result;
  }
  XELOGI("{}: Input system setup completed successfully", __func__);

  // Add inputSystem to UI
  XELOGI("{}: Loading input system into UI...", __func__);
  imgui_drawer_->LoadInputSystem(input_system_.get());

  XELOGI("{}: Initializing VFS...", __func__);
  // Bring up the virtual filesystem used by the kernel.
  XELOGI("{}: Creating virtual filesystem...", __func__);
  file_system_ = std::make_unique<xe::vfs::VirtualFileSystem>();

  XELOGI("{}: Creating patcher...", __func__);
  patcher_ = std::make_unique<xe::patcher::Patcher>(storage_root_);

  XELOGI("{}: Initializing Kernel...", __func__);
  // Shared kernel state.
  XELOGI("{}: Creating kernel state...", __func__);
  kernel_state_ = std::make_unique<xe::kernel::KernelState>(this);

  XELOGI("{}: Loading kernel modules...", __func__);
#define LOAD_KERNEL_MODULE(t) \
  static_cast<void>(kernel_state_->LoadKernelModule<kernel::t>())
  // HLE kernel modules.
  XELOGI("{}: Loading XboxKrnl module...", __func__);
  LOAD_KERNEL_MODULE(xboxkrnl::XboxkrnlModule);
  XELOGI("{}: Loading XAM module...", __func__);
  LOAD_KERNEL_MODULE(xam::XamModule);
  XELOGI("{}: Loading XBDM module...", __func__);
  LOAD_KERNEL_MODULE(xbdm::XbdmModule);
#undef LOAD_KERNEL_MODULE

  XELOGI("{}: Creating plugin loader...", __func__);
  plugin_loader_ = std::make_unique<xe::patcher::PluginLoader>(
      kernel_state_.get(), storage_root() / "plugins");
  XELOGI("{}: Kernel initialization completed successfully", __func__);

  XELOGI("{}: Starting graphics_system...", __func__);
  fprintf(stderr, "[emulator] About to call graphics_system_->Setup()\n");
  fprintf(stderr, "[emulator]   processor: %p\n",
          static_cast<void*>(processor_.get()));
  fprintf(stderr, "[emulator]   kernel_state: %p\n",
          static_cast<void*>(kernel_state_.get()));
  fprintf(stderr, "[emulator]   display_window: %p\n",
          static_cast<void*>(display_window_));
  fprintf(stderr, "[emulator]   has_display: %d\n", display_window_ != nullptr);
  fprintf(stderr, "[emulator]   app_context: %p\n",
          display_window_ ? &display_window_->app_context() : nullptr);

  // Setup the core components.
  fprintf(stderr, "[emulator] Calling graphics_system_->Setup()...\n");
  fflush(stderr);
  XELOGI("{}: Setting up graphics system...", __func__);
  result = graphics_system_->Setup(
      processor_.get(), kernel_state_.get(),
      display_window_ ? &display_window_->app_context() : nullptr,
      display_window_ != nullptr);
  fflush(stderr);

  fprintf(stderr,
          "[emulator] graphics_system_->Setup() returned with result: 0x%08X\n",
          result);
  fflush(stderr);
  XELOGI("{}: Graphics system setup returned result: {:08X}", __func__, result);

  if (result) {
    XELOGE("{}: Failed to setup graphics_system!", __func__);
    fprintf(stderr, "[emulator] DEBUG: graphics_system setup FAILED\n");
    return result;
  }

  fprintf(stderr,
          "[emulator] DEBUG: graphics_system setup completed successfully\n");
  XELOGI("{}: Graphics system setup completed successfully", __func__);

  if (audio_system_) {
    XELOGI("{}: Starting audio_system...", __func__);
    result = audio_system_->Setup(kernel_state_.get());
    if (result) {
      XELOGE("{}: Failed to setup audio_system!", __func__);
      return result;
    }
    XELOGI("{}: Audio system setup completed successfully", __func__);
    XELOGI("{}: Creating audio media player...", __func__);
    audio_media_player_ = std::make_unique<apu::AudioMediaPlayer>(
        audio_system_.get(), kernel_state_.get());
    audio_media_player_->Setup();
    XELOGI("{}: Audio media player setup completed", __func__);
  } else {
    XELOGI("{}: No audio system, skipping audio setup", __func__);
  }

  // Initialize emulator fallback exception handling last.
  XELOGI("{}: Installing exception handler...", __func__);
  ExceptionHandler::Install(Emulator::ExceptionCallbackThunk, this);
  XELOGI("{}: Exception handler installed", __func__);

  XELOGI("{}: DEBUG: Emulator setup completed successfully!", __func__);
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::TerminateTitle() {
  if (!is_title_open()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  // Prevent stale guest-main state from leaking into a future launch.
  main_thread_ = nullptr;
  kernel_state_->TerminateTitle();
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  on_terminate();
  return X_STATUS_SUCCESS;
}

const std::unique_ptr<vfs::Device> Emulator::CreateVfsDevice(
    const std::filesystem::path& path, const std::string_view mount_path) {
  XELOGI("DEBUG: CreateVfsDevice ENTRY - EXTREME DEBUG MODE");
  XELOGI("DEBUG: PATH: '{}'", path.string());
  XELOGI("DEBUG: MOUNT_PATH: '{}'", mount_path);

  auto file_sig = GetFileSignature(path);
  XELOGI("DEBUG: FILE SIGNATURE: {}", static_cast<int>(file_sig));
  fprintf(stderr, "DEBUG: File signature detected: %d\n",
          static_cast<int>(file_sig));

  // Map signature to string for debugging
  const char* sig_name = "UNKNOWN";
  switch (file_sig) {
    case FileSignatureType::XEX1:
      sig_name = "XEX1";
      break;
    case FileSignatureType::XEX2:
      sig_name = "XEX2";
      break;
    case FileSignatureType::ELF:
      sig_name = "ELF";
      break;
    case FileSignatureType::LIVE:
      sig_name = "LIVE";
      break;
    case FileSignatureType::CON:
      sig_name = "CON";
      break;
    case FileSignatureType::PIRS:
      sig_name = "PIRS";
      break;
    case FileSignatureType::XISO:
      sig_name = "XISO";
      break;
    case FileSignatureType::ZAR:
      sig_name = "ZAR";
      break;
    case FileSignatureType::EXE:
      sig_name = "EXE";
      break;
    case FileSignatureType::Unknown:
      sig_name = "UNKNOWN";
      break;
  }
  XELOGI("DEBUG: FILE TYPE: {}", sig_name);
  fprintf(stderr, "DEBUG: File type: %s\n", sig_name);

  // Must check if the type has changed e.g. XamSwapDisc
  switch (file_sig) {
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      auto parent_path = path.parent_path();
      return std::make_unique<vfs::HostPathDevice>(
          mount_path, parent_path, !cvars::allow_game_relative_writes);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      return vfs::XContentContainerDevice::CreateContentDevice(mount_path,
                                                               path);
    } break;
    case FileSignatureType::XISO: {
      return std::make_unique<vfs::DiscImageDevice>(mount_path, path);
    } break;
    case FileSignatureType::ZAR: {
      return std::make_unique<vfs::DiscZarchiveDevice>(mount_path, path);
    } break;
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      return nullptr;
      break;
  }
}

uint64_t Emulator::GetPersistentEmulatorFlags() {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = 0;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    // let the Set function create the key and initialize it to 0
    SetPersistentEmulatorFlags(0ULL);
    return 0ULL;
  }

  lstat = RegQueryValueExA(xenia_hkey, "XEFLAGS", 0, NULL,
                           reinterpret_cast<LPBYTE>(&value), &value_size);
  RegCloseKey(xenia_hkey);
  if (lstat) {
    return 0ULL;
  }
  return value;
#else
  return EmulatorFlagDisclaimerAcknowledged;
#endif
}
void Emulator::SetPersistentEmulatorFlags(uint64_t new_flags) {
#if XE_PLATFORM_WIN32 == 1
  uint64_t value = new_flags;
  DWORD value_size = sizeof(value);
  HKEY xenia_hkey = nullptr;
  LSTATUS lstat =
      RegOpenKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  if (!xenia_hkey) {
    lstat = RegCreateKeyA(HKEY_CURRENT_USER, "SOFTWARE\\Xenia", &xenia_hkey);
  }

  lstat = RegSetValueExA(xenia_hkey, "XEFLAGS", 0, REG_QWORD,
                         reinterpret_cast<const BYTE*>(&value), 8);
  RegFlushKey(xenia_hkey);
  RegCloseKey(xenia_hkey);
#endif
}

X_STATUS Emulator::MountPath(const std::filesystem::path& path,
                             const std::string_view mount_path) {
  XELOGI("DEBUG: MountPath ENTRY - EXTREME DEBUG MODE");
  XELOGI("DEBUG: PATH: '{}'", path.string());
  XELOGI("DEBUG: MOUNT_PATH: '{}'", mount_path);
  XELOGI("DEBUG: FILE EXISTS: {}", std::filesystem::exists(path) ? "YES" : "NO");
  XELOGI("DEBUG: FILE SIZE: {} bytes",
         std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0);

  fprintf(stderr, "DEBUG: MountPath: %s -> %s\n", path.c_str(),
          mount_path.data());
  fprintf(stderr, "DEBUG: Creating VFS device...\n");

  auto device = CreateVfsDevice(path, mount_path);
  fprintf(stderr, "DEBUG: Device created: %p\n",
          static_cast<void*>(device.get()));
  XELOGI("DEBUG: DEVICE POINTER: {}", static_cast<void*>(device.get()));

  if (!device) {
    XELOGE("DEBUG: FAILED: CreateVfsDevice returned nullptr");
    fprintf(stderr, "DEBUG: CreateVfsDevice returned nullptr\n");
    return X_STATUS_NO_SUCH_FILE;
  }

  fprintf(stderr, "DEBUG: Initializing device...\n");
  bool init_result = device->Initialize();
  XELOGI("DEBUG: DEVICE INIT RESULT: {}", init_result);
  fprintf(stderr, "DEBUG: Device Initialize() returned: %s\n",
          init_result ? "SUCCESS" : "FAILED");

  if (!device || !device->Initialize()) {
    XELOGE(
        "DEBUG: FAILED: Unable to mount selected file - unsupported format or "
        "corrupted");
    fprintf(stderr,
            "DEBUG: Unable to mount file - unsupported format or corrupted\n");
    return X_STATUS_NO_SUCH_FILE;
  }

  fprintf(stderr, "DEBUG: Registering device with filesystem...\n");
  bool register_result = file_system_->RegisterDevice(std::move(device));
  XELOGI("DEBUG: REGISTER RESULT: {}", register_result);
  fprintf(stderr, "DEBUG: RegisterDevice returned: %s\n",
          register_result ? "SUCCESS" : "FAILED");

  if (!register_result) {
    XELOGE("DEBUG: FAILED: Unable to register input file to {}", mount_path);
    fprintf(stderr, "DEBUG: Unable to register file to %s\n", mount_path.data());
    return X_STATUS_NO_SUCH_FILE;
  }

  XELOGI("DEBUG: MountPath SUCCESS: '{}' -> '{}'", path.string(), mount_path);
  fprintf(stderr, "DEBUG: MountPath SUCCESS!\n");

  file_system_->UnregisterSymbolicLink(kDefaultPartitionSymbolicLink);
  file_system_->UnregisterSymbolicLink(kDefaultGameSymbolicLink);
  file_system_->UnregisterSymbolicLink("plugins:");

  // Create symlinks to the device.
  file_system_->RegisterSymbolicLink(kDefaultGameSymbolicLink, mount_path);
  file_system_->RegisterSymbolicLink(kDefaultPartitionSymbolicLink, mount_path);

  return X_STATUS_SUCCESS;
}

Emulator::FileSignatureType Emulator::GetFileSignature(
    const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");

  if (!file) {
    return FileSignatureType::Unknown;
  }

  const uint64_t file_size = std::filesystem::file_size(path);
  constexpr int64_t header_size = 4;

  if (file_size < header_size) {
    return FileSignatureType::Unknown;
  }

  char file_magic[header_size];
  fread(file_magic, sizeof(file_magic), 1, file);

  fourcc_t magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  fclose(file);

  switch (magic_value) {
    case xe::cpu::kXEX1Signature:
      return FileSignatureType::XEX1;
    case xe::cpu::kXEX2Signature:
      return FileSignatureType::XEX2;
    case xe::vfs::kCONSignature:
      return FileSignatureType::CON;
    case xe::vfs::kLIVESignature:
      return FileSignatureType::LIVE;
    case xe::vfs::kPIRSSignature:
      return FileSignatureType::PIRS;
    case xe::vfs::kXSFSignature:
      return FileSignatureType::XISO;
    case xe::cpu::kElfSignature:
      return FileSignatureType::ELF;
    default:
      break;
  }

  magic_value = make_fourcc(file_magic[0], file_magic[1], 0, 0);

  if (xe::kernel::kEXESignature == magic_value) {
    return FileSignatureType::EXE;
  }

  file = xe::filesystem::OpenFile(path, "rb");
  xe::filesystem::Seek(file, -header_size, SEEK_END);
  fread(file_magic, 1, header_size, file);
  fclose(file);

  magic_value =
      make_fourcc(file_magic[0], file_magic[1], file_magic[2], file_magic[3]);

  if (xe::vfs::kZarMagic == magic_value) {
    return FileSignatureType::ZAR;
  }

  // Check if XISO
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscImageDevice>("", path);

  XELOGI("Checking for XISO");

  if (device->Initialize()) {
    return FileSignatureType::XISO;
  }

  XELOGE("{}: {} ({:08X})", __func__, path.extension(), magic_value);
  return FileSignatureType::Unknown;
}

X_STATUS Emulator::LaunchPath(const std::filesystem::path& path) {
  X_STATUS mount_result = X_STATUS_SUCCESS;
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  fprintf(stderr, "DEBUG: LaunchPath ENTRY (thread %s)\n", oss.str().c_str());
  fflush(stderr);
  XELOGI("DEBUG: Emulator::LaunchPath ENTRY - EXTREME DEBUG MODE");
  XELOGI("DEBUG: TARGET PATH: '{}'", path.string());

  // Check if title is already open
  bool title_already_open = is_title_open();
  printf("DEBUG: Title open check completed\n");
  XELOGI("DEBUG: TITLE ALREADY OPEN: {}", title_already_open ? "YES" : "NO");
  if (title_already_open) {
    XELOGI("DEBUG: CURRENT TITLE ID: {:08X}", title_id_.value_or(0));
    XELOGI("DEBUG: CURRENT TITLE NAME: '{}'", title_name_);
  }

  auto file_signature = GetFileSignature(path);
  printf("DEBUG: File signature detection completed\n");
  XELOGI("DEBUG: FILE SIGNATURE: {}", static_cast<int>(file_signature));

  switch (file_signature) {
    case FileSignatureType::XEX1:
    case FileSignatureType::XEX2:
    case FileSignatureType::ELF: {
      printf("DEBUG: XEX/ELF case detected\n");
      XELOGI("LaunchPath: Detected XEX/ELF");
      mount_result = MountPath(path, "\\Device\\Harddisk0\\Partition1");
      printf("DEBUG: XEX/ELF MountPath completed\n");
      XELOGI("LaunchPath: MountPath result: {:08X}", mount_result);
      if (mount_result) return mount_result;
      return LaunchXexFile(path);
    } break;
    case FileSignatureType::LIVE:
    case FileSignatureType::CON:
    case FileSignatureType::PIRS: {
      printf("DEBUG: LIVE/CON/PIRS case detected\n");
      XELOGI("LaunchPath: Detected LIVE/CON/PIRS");
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      printf("DEBUG: LIVE/CON/PIRS MountPath completed\n");
      XELOGI("LaunchPath: MountPath result: {:08X}", mount_result);
      if (mount_result) return mount_result;
      return LaunchStfsContainer(path);
    } break;
    case FileSignatureType::XISO: {
      printf("DEBUG: XISO case detected\n");
      XELOGI("LaunchPath: Detected XISO");
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      printf("DEBUG: XISO MountPath completed\n");
      XELOGI("LaunchPath: MountPath result: {:08X}", mount_result);
      if (mount_result) return mount_result;
      printf("DEBUG: About to call LaunchDiscImage\n");
      auto res = LaunchDiscImage(path);
      printf("DEBUG: LaunchDiscImage completed\n");
      XELOGI("LaunchPath: LaunchDiscImage result: {:08X}", res);
      return res;
    } break;
    case FileSignatureType::ZAR: {
      XELOGI("LaunchPath: Detected ZAR");
      mount_result = MountPath(path, "\\Device\\Cdrom0");
      XELOGI("LaunchPath: MountPath result: {:08X}", mount_result);
      if (mount_result) return mount_result;
      return LaunchDiscArchive(path);
    } break;
    case FileSignatureType::EXE:
    case FileSignatureType::Unknown:
    default:
      printf("DEBUG: Unknown file type detected\n");
      XELOGE("LaunchPath: Unknown file type");
      return X_STATUS_NOT_SUPPORTED;
      break;
  }
  printf("DEBUG: LaunchPath function completed - should not reach here\n");
}

X_STATUS Emulator::LaunchXexFile(const std::filesystem::path& path) {
  // We create a virtual filesystem pointing to its directory and symlink
  // that to the game filesystem.
  // e.g., /my/files/foo.xex will get a local fs at:
  // \\Device\\Harddisk0\\Partition1
  // and then get that symlinked to game:\, so
  // -> game:\foo.xex
  // Get just the filename (foo.xex).
  auto file_name = path.filename();

  // Launch the game.
  auto fs_path = "game:\\" + xe::path_to_utf8(file_name);
  X_STATUS result = CompleteLaunch(path, fs_path);

  if (XFAILED(result)) {
    return result;
  }

  kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;

  if (!kernel::IsSystemTitle(kernel_state_->title_id())) {
    return result;
  }

  const std::string mount_path =
      utf8::find_base_guest_path(kernel_state_->GetExecutableModule()->path());

  // System related symlinks
  file_system_->RegisterSymbolicLink("media:", mount_path);
  file_system_->RegisterSymbolicLink("font:", mount_path);

  auto module = kernel_state_->LoadUserModule("xam.xex");

  if (!module) {
    module = kernel_state_->LoadUserModule("$flash_xam.xex");
  }

  if (module) {
    result = kernel_state_->FinishLoadingUserModule(module, false);
  }

  return result;
}

X_STATUS Emulator::LaunchDiscImage(const std::filesystem::path& path) {
  printf("DEBUG: LaunchDiscImage ENTRY\n");
  XELOGI("DEBUG: Emulator::LaunchDiscImage ENTRY");
  XELOGI("DEBUG: PATH: '{}'", path.string());

  std::string module_path = FindLaunchModule();
  printf("DEBUG: FindLaunchModule completed\n");
  XELOGI("DEBUG: Module path from FindLaunchModule: '{}'", module_path);

  printf("DEBUG: About to call CompleteLaunch\n");
  XELOGI("DEBUG: Calling CompleteLaunch...");
  X_STATUS result = CompleteLaunch(path, module_path);
  printf("DEBUG: CompleteLaunch completed\n");
  XELOGI("DEBUG: CompleteLaunch returned: {:08X}", result);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    XELOGI("DEBUG: Module not found, trying default module");
    return LaunchDefaultModule(path);
  }

  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  XELOGI("DEBUG: LaunchDiscImage returning: {:08X}", result);
  return result;
}

X_STATUS Emulator::LaunchDiscArchive(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
  return result;
}

X_STATUS Emulator::LaunchStfsContainer(const std::filesystem::path& path) {
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (result == X_STATUS_NOT_FOUND && !cvars::launch_module.empty()) {
    return LaunchDefaultModule(path);
  }
  kernel_state_->deployment_type_ = XDeploymentType::kDownload;
  return result;
}

X_STATUS Emulator::LaunchDefaultModule(const std::filesystem::path& path) {
  cvars::launch_module = "";
  std::string module_path = FindLaunchModule();
  X_STATUS result = CompleteLaunch(path, module_path);

  if (XSUCCEEDED(result)) {
    kernel_state_->deployment_type_ = XDeploymentType::kInstalledToHDD;
    auto title_id = kernel_state_->title_id();
    if (!kernel::IsSystemTitle(title_id)) {
      // Assumption that any loaded game is loaded as a disc.
      kernel_state_->deployment_type_ = XDeploymentType::kOpticalDisc;
    }
  }
  return result;
}

X_STATUS Emulator::DataMigration(const uint64_t xuid) {
  uint32_t failure_count = 0;
  const std::string xuid_string = fmt::format("{:016X}", xuid);
  const std::string common_xuid_string = fmt::format("{:016X}", 0);
  const std::filesystem::path path_to_profile_data =
      content_root_ / xuid_string / "FFFE07D1" / "00010000" / xuid_string;
  // Filter directories inside. First we need to find any content type
  // directories.
  // Savefiles must go to user specific directory
  // Everything else goes to common
  const auto titles_to_move = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(content_root_),
      std::regex("[A-F0-9]{8}"));

  for (const auto& title : titles_to_move) {
    if (xe::path_to_utf8(title.name) == "FFFE07D1" ||
        xe::path_to_utf8(title.name) == "00000000") {
      // SKip any dashboard/profile related data that was previously installed
      continue;
    }

    const auto content_type_dirs = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(title.path / title.name),
        std::regex("[A-F0-9]{8}"));

    for (const auto& content_type : content_type_dirs) {
      const std::string used_xuid =
          xe::path_to_utf8(content_type.name) == "00000001"
              ? xuid_string
              : common_xuid_string;

      const auto previous_path = content_root_ / title.name / content_type.name;
      const auto path = content_root_ / used_xuid / title.name;

      if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path);
      }

      std::error_code ec;
      std::filesystem::rename(previous_path, path / content_type.name, ec);

      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, previous_path, path / content_type.name, ec.message(),
               ec.value());
      }
    }
    // Other directories:
    // Headers - Just copy everything to both common and xuid locations
    // profile - ?
    if (std::filesystem::exists(title.path / title.name / "Headers")) {
      const auto xuid_path =
          content_root_ / xuid_string / title.name / "Headers";

      std::filesystem::create_directories(xuid_path);

      std::error_code ec;
      // Copy to specific user
      std::filesystem::copy(title.path / title.name / "Headers", xuid_path,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                            ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, title.path / title.name / "Headers", xuid_path,
               ec.message(), ec.value());
      }

      const auto header_types =
          xe::filesystem::ListDirectories(title.path / title.name / "Headers");

      if (!(header_types.size() == 1 &&
            header_types.at(0).name == "00000001")) {
        const auto common_path =
            content_root_ / common_xuid_string / title.name / "Headers";

        std::filesystem::create_directories(common_path);

        // Copy to common, skip cases where only savefile header is available
        std::filesystem::copy(title.path / title.name / "Headers", common_path,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
        if (ec) {
          failure_count++;
          XELOGW(
              "{}: Copying from: {} to: {} failed! Error message: {} ({:08X})",
              __func__, title.path / title.name / "Headers", common_path,
              ec.message(), ec.value());
        }
      }

      if (!ec) {
        // Remove previous directory
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "Headers", ec);
      }
    }

    if (std::filesystem::exists(title.path / title.name / "profile")) {
      // Find directory with previous username. There should be only one!
      const auto old_profile_data =
          xe::filesystem::ListDirectories(title.path / title.name / "profile");

      xe::filesystem::FileInfo entry_to_copy = xe::filesystem::FileInfo();
      if (old_profile_data.size() != 1) {
        for (const auto& entry : old_profile_data) {
          if (entry.name == "User") {
            entry_to_copy = entry;
          }
        }
      } else {
        entry_to_copy = old_profile_data.front();
      }

      const auto path_from =
          title.path / title.name / "profile" / entry_to_copy.name;
      std::error_code ec;
      // Move files from inside to outside for convenience
      std::filesystem::rename(path_from, path_to_profile_data / title.name, ec);
      if (ec) {
        failure_count++;
        XELOGW("{}: Moving from: {} to: {} failed! Error message: {} ({:08X})",
               __func__, path_from, path_to_profile_data / title.name,
               ec.message(), ec.value());
      } else {
        std::error_code ec;
        std::filesystem::remove_all(title.path / title.name / "profile", ec);
      }
    }

    const auto remaining_file_list =
        xe::filesystem::ListDirectories(title.path / title.name);

    if (remaining_file_list.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(title.path / title.name, ec);
    }
  }

  std::string migration_status_message =
      fmt::format("Migration finished with {} {}.", failure_count,
                  failure_count == 1 ? "error" : "errors");

  if (failure_count) {
    migration_status_message.append(
        " For more information check xenia.log file.");
  }
  new xe::ui::HostNotificationWindow(imgui_drawer_, "Migration Status",
                                     migration_status_message, 0);
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::ProcessContentPackageHeader(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.name_ = "Invalid Content Package!";
  installation_info.content_type_ = XContentType::kInvalid;
  installation_info.data_installation_path_ = xe::path_to_utf8(path.filename());

  const auto header = vfs::XContentContainerDevice::ReadContainerHeader(path);

  if (!header || !header->content_header.is_magic_valid()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_result_ = X_STATUS_INVALID_PARAMETER;
    installation_info.installation_error_message_ = "Invalid Package Type!";
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  // Always install savefiles to user signed to slot 0.
  const auto profile =
      kernel_state_->xam_state()->profile_manager()->GetProfile(
          static_cast<uint8_t>(0));

  uint64_t xuid = header->content_metadata.profile_id;
  if (header->content_metadata.content_type == XContentType::kSavedGame &&
      profile) {
    xuid = profile->xuid();
  }

  installation_info.data_installation_path_ = fmt::format(
      "{:016X}/{:08X}/{:08X}/{}", xuid,
      header->content_metadata.execution_info.title_id.get(),
      static_cast<uint32_t>(header->content_metadata.content_type.get()),
      path.filename());

  installation_info.header_installation_path_ = fmt::format(
      "{:016X}/{:08X}/Headers/{:08X}/{}", xuid,
      header->content_metadata.execution_info.title_id.get(),
      static_cast<uint32_t>(header->content_metadata.content_type.get()),
      path.filename());

  installation_info.name_ =
      xe::to_utf8(header->content_metadata.display_name(XLanguage::kEnglish));
  installation_info.content_type_ =
      static_cast<XContentType>(header->content_metadata.content_type);
  installation_info.content_size_ = header->content_metadata.content_size;
  installation_info.installation_state_ = InstallState::pending;

  installation_info.icon_ = imgui_drawer_->LoadImGuiIcon(
      std::span<const uint8_t>(header->content_metadata.title_thumbnail,
                               header->content_metadata.title_thumbnail_size));
  return X_STATUS_SUCCESS;
}

X_STATUS Emulator::InstallContentPackage(
    const std::filesystem::path& path, ContentInstallEntry& installation_info) {
  installation_info.installation_state_ = InstallState::preparing;

  std::unique_ptr<vfs::XContentContainerDevice> device =
      vfs::XContentContainerDevice::CreateContentDevice("", path);

  if (!device || !device->Initialize()) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ =
        "Device initialization failed!";
    installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  const std::filesystem::path installation_path =
      content_root() / installation_info.data_installation_path_;

  const std::filesystem::path header_path =
      content_root() / installation_info.header_installation_path_;

  if (!std::filesystem::exists(content_root())) {
    const std::error_code ec = xe::filesystem::CreateFolder(content_root());
    if (ec) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ = ec.message();
      installation_info.installation_result_ = X_STATUS_ACCESS_DENIED;
      return X_STATUS_ACCESS_DENIED;
    }
  }

  const auto disk_space = std::filesystem::space(content_root());
  if (disk_space.available < installation_info.content_size_ * 1.1f) {
    installation_info.installation_state_ = InstallState::failed;
    installation_info.installation_error_message_ = "Insufficient disk space!";
    installation_info.installation_result_ = X_STATUS_DISK_FULL;
    return X_STATUS_DISK_FULL;
  }

  if (std::filesystem::exists(installation_path)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(installation_path, error_code);
    if (error_code) {
      installation_info.installation_state_ = InstallState::failed;
      installation_info.installation_error_message_ =
          "Cannot Create Content Directory!";
      installation_info.installation_result_ = error_code.value();
      return error_code.value();
    }
  }

  installation_info.content_size_ = device->data_size();
  installation_info.installation_state_ = InstallState::installing;

  vfs::VirtualFileSystem::ExtractContentHeader(device.get(), header_path);

  X_STATUS error_code = vfs::VirtualFileSystem::ExtractContentFiles(
      device.get(), installation_path,
      installation_info.currently_installed_size_);
  if (error_code != X_ERROR_SUCCESS) {
    installation_info.installation_state_ = InstallState::failed;
    return error_code;
  }

  installation_info.installation_state_ = InstallState::installed;
  installation_info.currently_installed_size_ = installation_info.content_size_;
  kernel_state()->BroadcastNotification(kXNotificationLiveContentInstalled, 0);

  if (installation_info.content_type_ == XContentType::kProfile) {
    kernel_state_->xam_state()->profile_manager()->ReloadProfiles();
  }

  return error_code;
}

X_STATUS Emulator::ExtractZarchivePackage(
    const std::filesystem::path& path,
    const std::filesystem::path& extract_dir) {
  std::unique_ptr<vfs::Device> device =
      std::make_unique<vfs::DiscZarchiveDevice>("", path);
  if (!device->Initialize()) {
    XELOGE("Failed to initialize device");
    return X_STATUS_INVALID_PARAMETER;
  }

  if (std::filesystem::exists(extract_dir)) {
    // TODO(Gliniak): Popup
    // Do you want to overwrite already existing data?
  } else {
    std::error_code error_code;
    std::filesystem::create_directories(extract_dir, error_code);
    if (error_code) {
      return error_code.value();
    }
  }

  uint64_t progress = 0;
  return vfs::VirtualFileSystem::ExtractContentFiles(device.get(), extract_dir,
                                                     progress);
}

X_STATUS Emulator::CreateZarchivePackage(
    const std::filesystem::path& inputDirectory,
    const std::filesystem::path& outputFile) {
  std::vector<uint8_t> buffer;
  buffer.resize(64 * 1024);

  std::error_code ec;
  PackContext packContext;
  packContext.outputFilePath = outputFile;

  ZArchiveWriter zWriter(
      [](int32_t partIndex, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile =
            std::ofstream(packContext->outputFilePath, std::ios::binary);

        if (!packContext->currentOutputFile.is_open()) {
          XELOGI("Failed to create output file: {}\n",
                 packContext->outputFilePath.string());
          packContext->hasError = true;
        }
      },
      [](const void* data, size_t length, void* ctx) {
        PackContext* packContext = reinterpret_cast<PackContext*>(ctx);
        packContext->currentOutputFile.write(
            reinterpret_cast<const char*>(data), length);
      },
      &packContext);

  if (packContext.hasError) {
    return X_STATUS_UNSUCCESSFUL;
  }

  for (auto const& dirEntry :
       std::filesystem::recursive_directory_iterator(inputDirectory)) {
    std::filesystem::path pathEntry =
        std::filesystem::relative(dirEntry.path(), inputDirectory, ec);

    if (ec) {
      XELOGI("Failed to get relative path {}\n", pathEntry.string());
      return X_STATUS_UNSUCCESSFUL;
    }

    if (dirEntry.is_directory()) {
      if (!zWriter.MakeDir(pathEntry.generic_string().c_str(), false)) {
        XELOGI("Failed to create directory {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }
    } else if (dirEntry.is_regular_file()) {
      // Don't pack itself to prevent infinite packing.
      if (dirEntry == outputFile) {
        continue;
      }

      XELOGI("Adding file: {}\n", pathEntry.string());

      if (!zWriter.StartNewFile(pathEntry.generic_string().c_str())) {
        XELOGI("Failed to create archive file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      std::filesystem::path file_to_pack_path = inputDirectory / pathEntry;
      FILE* file = xe::filesystem::OpenFile(file_to_pack_path, "rb");

      if (!file) {
        XELOGI("Failed to open input file {}\n", pathEntry.string());
        return X_STATUS_UNSUCCESSFUL;
      }

      const uint64_t file_size = std::filesystem::file_size(file_to_pack_path);
      uint64_t total_bytes_read = 0;

      while (total_bytes_read < file_size) {
        uint64_t bytes_read = fread(buffer.data(), 1, buffer.size(), file);

        total_bytes_read += bytes_read;

        zWriter.AppendData(buffer.data(), bytes_read);
      }

      fclose(file);
    }

    if (packContext.hasError) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  zWriter.Finalize();

  return X_STATUS_SUCCESS;
}

void Emulator::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Don't hold the lock on this (so any waits follow through)
  graphics_system_->Pause();
  audio_system_->Pause();

  auto lock = global_critical_region::AcquireDirect();
  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  auto current_thread = kernel::XThread::IsInThread()
                            ? kernel::XThread::GetCurrentThread()
                            : nullptr;
  for (auto thread : threads) {
    // Don't pause ourself or host threads.
    if (thread == current_thread || !thread->can_debugger_suspend()) {
      continue;
    }

    if (thread->is_running()) {
      thread->thread()->Suspend(nullptr);
    }
  }

  XELOGD("! EMULATOR PAUSED !");
}

void Emulator::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;
  XELOGD("! EMULATOR RESUMED !");

  graphics_system_->Resume();
  audio_system_->Resume();

  auto threads =
      kernel_state()->object_table()->GetObjectsByType<kernel::XThread>(
          kernel::XObject::Type::Thread);
  for (auto thread : threads) {
    if (!thread->can_debugger_suspend()) {
      // Don't pause host threads.
      continue;
    }

    if (!thread->is_running()) {
      thread->thread()->Resume(nullptr);
    }
  }
}

bool Emulator::SaveToFile(const std::filesystem::path& path) {
  Pause();

  filesystem::CreateEmptyFile(path);
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite, 0, 2_GiB);
  if (!map) {
    return false;
  }

  // Save the emulator state to a file
  ByteStream stream(map->data(), map->size());
  stream.Write(kEmulatorSaveSignature);
  stream.Write(title_id_.has_value());
  if (title_id_.has_value()) {
    stream.Write(title_id_.value());
  }

  // It's important we don't hold the global lock here! XThreads need to step
  // forward (possibly through guarded regions) without worry!
  processor_->Save(&stream);
  graphics_system_->Save(&stream);
  audio_system_->Save(&stream);
  kernel_state_->Save(&stream);
  memory_->Save(&stream);
  map->Close(stream.offset());

  Resume();
  return true;
}

bool Emulator::RestoreFromFile(const std::filesystem::path& path) {
  // Restore the emulator state from a file
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kReadWrite);
  if (!map) {
    return false;
  }

  restoring_ = true;

  // Terminate any loaded titles.
  Pause();
  kernel_state_->TerminateTitle();

  auto lock = global_critical_region::AcquireDirect();
  ByteStream stream(map->data(), map->size());
  if (stream.Read<uint32_t>() != kEmulatorSaveSignature) {
    return false;
  }

  auto has_title_id = stream.Read<bool>();
  std::optional<uint32_t> title_id;
  if (!has_title_id) {
    title_id = {};
  } else {
    title_id = stream.Read<uint32_t>();
  }
  if (title_id_.has_value() != title_id.has_value() ||
      title_id_.value() != title_id.value()) {
    // Swapping between titles is unsupported at the moment.
    assert_always();
    return false;
  }

  if (!processor_->Restore(&stream)) {
    XELOGE("Could not restore processor!");
    return false;
  }
  if (!graphics_system_->Restore(&stream)) {
    XELOGE("Could not restore graphics system!");
    return false;
  }
  if (!audio_system_->Restore(&stream)) {
    XELOGE("Could not restore audio system!");
    return false;
  }
  if (!kernel_state_->Restore(&stream)) {
    XELOGE("Could not restore kernel state!");
    return false;
  }
  if (!memory_->Restore(&stream)) {
    XELOGE("Could not restore memory!");
    return false;
  }

  // Update the main thread.
  auto threads =
      kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
  for (auto thread : threads) {
    if (thread->main_thread()) {
      main_thread_ = thread;
      break;
    }
  }

  Resume();

  restore_fence_.Signal();
  restoring_ = false;

  return true;
}

const std::filesystem::path Emulator::GetNewDiscPath(
    std::string window_message) {
  std::filesystem::path path = "";

  auto file_picker = xe::ui::FilePicker::Create();
  file_picker->set_mode(ui::FilePicker::Mode::kOpen);
  file_picker->set_type(ui::FilePicker::Type::kFile);
  file_picker->set_multi_selection(false);
  file_picker->set_title(!window_message.empty() ? window_message
                                                 : "Select Content Package");
  file_picker->set_extensions({
      {"Supported Files", "*.iso;*.xex;*.xcp;*.*"},
      {"Disc Image (*.iso)", "*.iso"},
      {"Xbox Executable (*.xex)", "*.xex"},
      {"All Files (*.*)", "*.*"},
  });

  if (file_picker->Show()) {
    auto selected_files = file_picker->selected_files();
    if (!selected_files.empty()) {
      path = selected_files[0];
    }
  }
  return path;
}

bool Emulator::ExceptionCallbackThunk(Exception* ex, void* data) {
  return reinterpret_cast<Emulator*>(data)->ExceptionCallback(ex);
}

bool Emulator::ExceptionCallback(Exception* ex) {
  // Check to see if the exception occurred in guest code.
  auto code_cache = processor()->backend()->code_cache();
  auto code_base = code_cache->execute_base_address();
  auto code_end = code_base + code_cache->total_size();

  if (!processor()->is_debugger_attached() && debugging::IsDebuggerAttached()) {
    // If Xenia's debugger isn't attached but another one is, pass it to that
    // debugger.
    return false;
  } else if (processor()->is_debugger_attached()) {
    // Let the debugger handle this exception. It may decide to continue past
    // it (if it was a stepping breakpoint, etc).
    return processor()->OnUnhandledException(ex);
  }

  if (!(ex->pc() >= code_base && ex->pc() < code_end)) {
    // Didn't occur in guest code. Let it pass.
    return false;
  }

  // Within range. Pause the emulator and eat the exception.
  Pause();

  // Dump information into the log.
  auto current_thread = kernel::XThread::GetCurrentThread();
  assert_not_null(current_thread);

  auto guest_function = code_cache->LookupFunction(ex->pc());
  assert_not_null(guest_function);

  auto context = current_thread->thread_state()->context();

  std::string crash_msg;
  crash_msg.append("==== CRASH DUMP ====\n");
  crash_msg.append(fmt::format("Thread ID (Host: 0x{:08X} / Guest: 0x{:08X})\n",
                               current_thread->thread()->system_id(),
                               current_thread->thread_id()));
  crash_msg.append(
      fmt::format("Thread Handle: 0x{:08X}\n", current_thread->handle()));
  crash_msg.append(
      fmt::format("PC: 0x{:08X}\n",
                  guest_function->MapMachineCodeToGuestAddress(ex->pc())));
  crash_msg.append("Registers:\n");
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" r{:<3} = {:016X}\n", i, context->r[i]));
  }
  for (int i = 0; i < 32; i++) {
    crash_msg.append(fmt::format(" f{:<3} = {:016X} = (double){} = (float){}\n",
                                 i,
                                 *reinterpret_cast<uint64_t*>(&context->f[i]),
                                 context->f[i], *(float*)&context->f[i]));
  }
  for (int i = 0; i < 128; i++) {
    crash_msg.append(
        fmt::format(" v{:<3} = [0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}]\n", i,
                    context->v[i].u32[0], context->v[i].u32[1],
                    context->v[i].u32[2], context->v[i].u32[3]));
  }
  XELOGE("{}", crash_msg);
  std::string crash_dlg = fmt::format(
      "The guest has crashed.\n\n"
      "Xenia has now paused itself.\n\n"
      "{}",
      crash_msg);
  // Display a dialog telling the user the guest has crashed.
  if (display_window_ && imgui_drawer_) {
    display_window_->app_context().CallInUIThreadSynchronous([this,
                                                              &crash_dlg]() {
      xe::ui::ImGuiDialog::ShowMessageBox(imgui_drawer_, "Uh-oh!", crash_dlg);
    });
  }

  // Now suspend ourself (we should be a guest thread).
  current_thread->Suspend(nullptr);

  // We should not arrive here!
  assert_always();
  return false;
}

void Emulator::WaitUntilExit() {
  uint64_t wait_profile_last_log_ms = 0;
  uint64_t wait_profile_count_timeout = 0;
  uint64_t wait_profile_count_success = 0;
  uint64_t wait_profile_count_other = 0;
  while (true) {
    if (main_thread_) {
      if (cvars::log_main_thread_pc) {
        uint32_t interval_ms = cvars::log_main_thread_pc_interval_ms;
        if (interval_ms < 50) {
          interval_ms = 50;
        }
        auto wait_start = Clock::QueryHostUptimeMillis();
        auto wait_result =
            xe::threading::Wait(main_thread_->thread(), false,
                                std::chrono::milliseconds(interval_ms));
        auto wait_end = Clock::QueryHostUptimeMillis();
        if (cvars::log_main_thread_wait_profile) {
          switch (wait_result) {
            case xe::threading::WaitResult::kTimeout:
              ++wait_profile_count_timeout;
              break;
            case xe::threading::WaitResult::kSuccess:
              ++wait_profile_count_success;
              break;
            default:
              ++wait_profile_count_other;
              break;
          }
          uint64_t now_ms = wait_end;
          uint64_t interval =
              static_cast<uint64_t>(cvars::log_main_thread_wait_profile_interval_ms);
          if (wait_profile_last_log_ms == 0 ||
              interval == 0 ||
              now_ms - wait_profile_last_log_ms >= interval) {
            wait_profile_last_log_ms = now_ms;
            XELOGI(
                "DEBUG: WaitUntilExit profile: timeouts={} success={} other={} "
                "last_wait_ms={} thread_handle={:08X} thread_id={}",
                wait_profile_count_timeout, wait_profile_count_success,
                wait_profile_count_other,
                static_cast<unsigned long long>(wait_end - wait_start),
                main_thread_->handle(), main_thread_->thread_id());
          }
        }
        if (wait_result == xe::threading::WaitResult::kTimeout) {
          LogMainThreadPcSample("WaitUntilExit");
          continue;
        } else if (wait_result != xe::threading::WaitResult::kSuccess) {
          XELOGW("DEBUG: MainThread wait returned {}",
                 static_cast<int>(wait_result));
          LogMainThreadPcSample("WaitUntilExit");
          continue;
        }
      } else {
        xe::threading::Wait(main_thread_->thread(), false);
      }
    }

    if (restoring_) {
      restore_fence_.Wait();
    } else {
      // Not restoring and the thread exited. We're finished.
      break;
    }
  }

  on_exit();
}

void Emulator::LogMainThreadPcSample(const char* reason) const {
  static uint32_t last_pc = 0;
  static uint32_t same_pc_samples = 0;
  static uint32_t last_detailed_pc = 0;
  static uint64_t last_detailed_time_ms = 0;
  static uint64_t last_loop_profile_ms = 0;
  if (!main_thread_) {
    XELOGW("DEBUG: MainThread PC sample ({}) - no main thread", reason);
    return;
  }
  auto* thread = main_thread_.get();
  auto* thread_state = thread->thread_state();
  if (!thread_state) {
    XELOGW("DEBUG: MainThread PC sample ({}) - no thread_state", reason);
    return;
  }
  auto* ctx = thread_state->context();
  if (!ctx) {
    XELOGW("DEBUG: MainThread PC sample ({}) - no PPC context", reason);
    return;
  }

  uint32_t pc = ctx->current_pc;
  uint32_t lr = static_cast<uint32_t>(ctx->lr);
  uint32_t ctr = static_cast<uint32_t>(ctx->ctr);
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (pc == last_pc) {
    ++same_pc_samples;
  } else {
    same_pc_samples = 0;
    last_pc = pc;
  }

  const char* module_name = "<unknown>";
  const char* function_name = "<unknown>";
  uint32_t function_address = 0;
  if (processor_) {
    auto module = processor_->LookupModule(pc);
    if (module) {
      module_name = module->name().c_str();
    }
    auto function = processor_->LookupFunction(pc);
    if (function) {
      function_name = function->name().c_str();
      function_address = function->address();
    }
  }
  XELOGI(
      "DEBUG: MainThread PC sample ({}) thread_id={} handle={:08X} pc={:08X} "
      "lr={:08X} ctr={:08X} "
      "r1={:016X} r3={:016X} r4={:016X} r13={:016X} msr={:016X} module={} "
      "function={}({:08X})",
      reason, thread->thread_id(), thread->handle(), pc, lr, ctr,
      static_cast<uint64_t>(ctx->r[1]), static_cast<uint64_t>(ctx->r[3]),
      static_cast<uint64_t>(ctx->r[4]), static_cast<uint64_t>(ctx->r[13]),
      static_cast<uint64_t>(ctx->msr), module_name, function_name,
      function_address);
  uint32_t sp = static_cast<uint32_t>(ctx->r[1]);
  uint32_t stack_base = thread->stack_base();
  uint32_t stack_limit = thread->stack_limit();
  bool sp_in_range = sp >= stack_limit && sp <= stack_base;
  XELOGI("DEBUG: MainThread stack range: {:08X}-{:08X} (sp in range: {})",
         stack_limit, stack_base, sp_in_range ? "YES" : "NO");

  // Extra profiling when the main thread is stuck at a known loop address.
  uint32_t loop_pc = cvars::log_main_thread_pc_loop_address;
  if (loop_pc != 0 && pc == loop_pc) {
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    uint64_t interval =
        static_cast<uint64_t>(cvars::log_main_thread_pc_loop_interval_ms);
    if (last_loop_profile_ms == 0 || interval == 0 ||
        now_ms - last_loop_profile_ms >= interval) {
      last_loop_profile_ms = now_ms;
      XELOGW("DEBUG: PC loop profile: pc={:08X} lr={:08X} ctr={:08X} r1={:08X}",
             pc, lr, ctr, sp);
      XELOGW("DEBUG: PC loop profile: module={} function={}({:08X})",
             module_name, function_name, function_address);

      auto dump_disasm = [&](uint32_t base, uint32_t count,
                             const char* label) {
        if (!memory_ || count == 0) {
          return;
        }
        XELOGW("DEBUG: {} @ {:08X} ({} ins):", label ? label : "disasm", base,
               count);
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t addr = base + (i * 4);
          auto ins_safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
          if (!ins_safe.success || !ins_safe.pointer) {
            XELOGW("DEBUG: {:08X}: <invalid> ({})", addr, ins_safe.error);
            continue;
          }
          uint32_t code = xe::load_and_swap<uint32_t>(ins_safe.pointer);
          xe::StringBuffer sb;
          xe::cpu::ppc::DisasmPPC(addr, code, &sb);
          XELOGW("DEBUG: {:08X}: {:08X} {}", addr, code, sb.to_string());
        }
      };

      uint32_t disasm_count = std::max<uint32_t>(
          1, cvars::log_main_thread_pc_loop_disasm_count);
      dump_disasm(pc, disasm_count, "loop disasm");
      if (lr != 0) {
        dump_disasm(lr, std::min<uint32_t>(disasm_count, 8), "lr target");
      }

      if (cvars::log_main_thread_pc_loop_dump_stack && memory_) {
        auto stack_safe = TranslateVirtualSafe<uint32_t*>(memory_, sp);
        if (stack_safe.success && stack_safe.pointer) {
          XELOGW("DEBUG: PC loop stack window @ r1={:08X} (first 32 dwords):", sp);
          for (int i = 0; i < 32; ++i) {
            uint32_t addr = sp + (i * 4);
            uint32_t value =
                xe::load_and_swap<uint32_t>(stack_safe.pointer + i);
            XELOGW("DEBUG: [SP+{:02X}] {:08X}", i * 4, value);
          }
        } else {
          XELOGW("DEBUG: PC loop stack dump failed @ {:08X}: {}", sp,
                 stack_safe.error);
        }
      }
    }
  }

  if (same_pc_samples > 0 && (same_pc_samples % 10) == 0) {
    XELOGW("DEBUG: MainThread PC appears stable at {:08X} for {} samples", pc,
           same_pc_samples);

    if (IsGpuBootstrapStallTarget(pc)) {
      auto& snapshot = GetGpuDispatchStallSnapshotStorage();
      const std::string stall_state =
          "gpu_bootstrap_entry_loop_before_callback";
      const std::string stall_detail = fmt::format(
          "guest main thread remained at early GPU bootstrap entry {:08X} "
          "for {} samples before callback registration or ring submission "
          "(lr={:08X} ctr={:08X} module={} function={}({:08X}))",
          pc, same_pc_samples, lr, ctr, module_name, function_name,
          function_address);
      const std::string stall_chain = fmt::format(
          "stable_pc={:08X} lr={:08X} ctr={:08X}", pc, lr, ctr);
      const std::string stall_targets = fmt::format(
          "module={} function={}({:08X})", module_name, function_name,
          function_address);
      std::lock_guard<std::mutex> lock(snapshot.mutex);
      int existing_stall_priority =
          snapshot.valid ? GpuDispatchStallStatePriority(snapshot.state) : -1;
      int new_stall_priority = GpuDispatchStallStatePriority(stall_state);
      if (snapshot.valid && existing_stall_priority > new_stall_priority) {
        snapshot.valid_context_samples =
            std::max<uint32_t>(snapshot.valid_context_samples, 1);
        snapshot.last_nonzero_pc = pc;
        snapshot.last_nonzero_lr = lr;
        snapshot.last_nonzero_ms = now_ms;
      } else {
      bool same_stall_state = snapshot.valid && snapshot.state == stall_state;
      snapshot.valid = true;
      snapshot.state = stall_state;
      snapshot.detail = stall_detail;
      snapshot.chain = stall_chain;
      snapshot.target_summaries = stall_targets;
      snapshot.repeat_samples = same_pc_samples;
      snapshot.now_ms = now_ms;
      snapshot.state_first_ms =
          same_stall_state && snapshot.state_first_ms != 0
              ? snapshot.state_first_ms
              : now_ms;
      snapshot.valid_context_samples =
          std::max<uint32_t>(snapshot.valid_context_samples, 1);
      snapshot.last_nonzero_pc = pc;
      snapshot.last_nonzero_lr = lr;
      snapshot.last_nonzero_state = stall_state;
      snapshot.last_nonzero_chain = stall_chain;
      snapshot.last_nonzero_targets = stall_targets;
      snapshot.last_nonzero_ms = now_ms;
      }
    }

    if (cvars::log_main_thread_stall_wait_nudge &&
        same_pc_samples >=
            cvars::log_main_thread_stall_wait_nudge_after_samples &&
        IsGpuBootstrapStallTarget(pc)) {
      static std::atomic<uint64_t> stall_wait_nudge_last_ms{0};
      uint64_t nudge_interval_ms = static_cast<uint64_t>(
          cvars::log_main_thread_stall_wait_nudge_interval_ms);
      uint64_t prev_nudge_ms =
          stall_wait_nudge_last_ms.load(std::memory_order_relaxed);
      bool nudge_due =
          prev_nudge_ms == 0 || nudge_interval_ms == 0 || now_ms < prev_nudge_ms ||
          now_ms - prev_nudge_ms >= nudge_interval_ms;
      if (nudge_due) {
        stall_wait_nudge_last_ms.store(now_ms, std::memory_order_relaxed);
        bool resumed = false;
        if (HasGuestMainThread()) {
          if (main_thread_->is_running()) {
            resumed = true;
          } else {
            uint32_t suspend_count = 0;
            X_STATUS resume_status = main_thread_->Resume(&suspend_count);
            resumed = main_thread_->is_running();
            XELOGW(
                "DEBUG: GUEST MAIN THREAD resume nudge: reason='{}' status={:08X} "
                "suspend_count={} running_now={} handle={:08X} thread_id={}",
                "stable-pc-stall",
                static_cast<uint32_t>(resume_status), suspend_count,
                resumed ? "YES" : "NO", main_thread_->handle(),
                main_thread_->thread_id());
          }
        }

        uint32_t wait_watch_obj = kernel::xboxkrnl::GetWaitWatchObjectPtr();
        uint32_t signal_obj =
            wait_watch_obj ? wait_watch_obj : 0x30028030u;
        bool signal_attempted = false;
        bool signal_succeeded = false;
        uint32_t signal_handle = 0;

        if (kernel_state_ && memory_ && signal_obj != 0) {
          auto dispatch_safe =
              TranslateVirtualSafe<kernel::X_DISPATCH_HEADER*>(memory_,
                                                               signal_obj);
          if (dispatch_safe.success && dispatch_safe.pointer &&
              dispatch_safe.pointer->wait_list.flink_ptr ==
                  kernel::kXObjSignature) {
            signal_handle = dispatch_safe.pointer->wait_list.blink_ptr;
            auto event_obj = kernel_state_->object_table()->LookupObject<kernel::XEvent>(
                signal_handle);
            if (event_obj) {
              signal_attempted = true;
              event_obj->Set(0, false);
              signal_succeeded = true;
            }
          }
        }

        XELOGW(
            "DEBUG: MainThread wait-loop nudge: pc={:08X} samples={} resumed={} "
            "watch_obj={:08X} signal_obj={:08X} signal_attempted={} "
            "signal_succeeded={} signal_handle={:08X}",
            pc, same_pc_samples, resumed ? "YES" : "NO", wait_watch_obj,
            signal_obj, signal_attempted ? "YES" : "NO",
            signal_succeeded ? "YES" : "NO", signal_handle);
      }
    }

    if (cvars::log_main_thread_stall_gpu_bootstrap_nudge &&
        same_pc_samples >=
            cvars::log_main_thread_stall_gpu_bootstrap_nudge_after_samples &&
        IsGpuBootstrapStallTarget(pc)) {
      static std::atomic<uint64_t> stall_gpu_nudge_last_ms{0};
      uint64_t nudge_interval_ms = static_cast<uint64_t>(
          cvars::log_main_thread_stall_gpu_bootstrap_nudge_interval_ms);
      uint64_t prev_nudge_ms =
          stall_gpu_nudge_last_ms.load(std::memory_order_relaxed);
      bool nudge_due =
          prev_nudge_ms == 0 || nudge_interval_ms == 0 || now_ms < prev_nudge_ms ||
          now_ms - prev_nudge_ms >= nudge_interval_ms;
      if (nudge_due) {
        auto* gs = graphics_system();
        auto* cp = gs ? gs->command_processor() : nullptr;
        if (gs && cp) {
          auto prereq = gs->GetPipelinePrereqSnapshot();
          auto cp_state = cp->GetRingBufferDebugState();
            bool guest_swap_packets_seen =
              cp_state.swap_packet_count >
              cp_state.debug_host_swap_injection_count;
          bool producer_entry_missing =
              prereq.interrupt_callback_set &&
              prereq.vd_swap_calls == 0 &&
              prereq.mmio_cp_rb_wptr_guest_writes == 0 &&
              cp_state.ring_buffer_initialized &&
              cp_state.initialization_acknowledged &&
              (!guest_swap_packets_seen) &&
              cp_state.swap_packet_count <=
                  static_cast<uint64_t>(
                      cvars::log_main_thread_stall_gpu_bootstrap_nudge_max_swap_packets);
          if (producer_entry_missing) {
            stall_gpu_nudge_last_ms.store(now_ms, std::memory_order_relaxed);
            uint32_t nudge_width = cp_state.last_swap_width;
            uint32_t nudge_height = cp_state.last_swap_height;
            if (nudge_width == 0 || nudge_height == 0) {
              nudge_width = 1280;
              nudge_height = 720;
            }
            if (cvars::log_main_thread_stall_force_ring_init_nudge) {
              cp->ForceFallbackInitialization();
            }
            gs->AttemptFallbackInitialization();
            cp->DebugIssueSwapFromHost(nudge_width, nudge_height);
            XELOGW(
                "DEBUG: MainThread GPU bootstrap nudge: pc={:08X} samples={} "
                "cb_set={} vd_swap_calls={} mmio_wptr_writes={} cp_swap_packets={} "
                "cp_debug_host_swaps={} guest_swap_packets_seen={} "
                "force_ring_init_nudge={} issuing host swap {}x{}",
                pc, same_pc_samples,
                prereq.interrupt_callback_set ? "YES" : "NO",
                prereq.vd_swap_calls, prereq.mmio_cp_rb_wptr_guest_writes,
                cp_state.swap_packet_count,
                cp_state.debug_host_swap_injection_count,
                guest_swap_packets_seen ? "YES" : "NO",
                cvars::log_main_thread_stall_force_ring_init_nudge ? "YES" : "NO",
                nudge_width, nudge_height);
          }
        }
      }
    }

    if (cvars::log_main_thread_stall_debug_monitor_nudge &&
        IsGpuBootstrapStallTarget(pc)) {
      static std::atomic<uint64_t> debug_monitor_nudge_last_ms{0};
      uint64_t nudge_interval_ms = static_cast<uint64_t>(
          cvars::log_main_thread_stall_debug_monitor_nudge_interval_ms);
      uint64_t prev_nudge_ms =
          debug_monitor_nudge_last_ms.load(std::memory_order_relaxed);
      bool nudge_due =
          prev_nudge_ms == 0 || nudge_interval_ms == 0 || now_ms < prev_nudge_ms ||
          now_ms - prev_nudge_ms >= nudge_interval_ms;
      if (nudge_due && kernel_state_ && memory_) {
        auto xboxkrnl =
          kernel_state_->GetKernelModule<kernel::xboxkrnl::XboxkrnlModule>(
            "xboxkrnl.exe");
        uint32_t ke_debug_var_addr =
            xboxkrnl
                ? xboxkrnl->GetProcAddressByOrdinal(ordinals::KeDebugMonitorData)
                : 0;
        if (ke_debug_var_addr != 0) {
          auto slot_safe =
              TranslateVirtualSafe<xe::be<uint32_t>*>(memory_,
                                                      ke_debug_var_addr);
          if (slot_safe.success && slot_safe.pointer) {
            uint32_t current_struct_ptr =
                xe::load_and_swap<uint32_t>(slot_safe.pointer);
            uint32_t fallback_struct_ptr = ke_debug_var_addr + 4;
            auto current_struct_safe =
                TranslateVirtualSafe<
                    kernel::xboxkrnl::X_KEDEBUGMONITORDATA*>(
                    memory_, current_struct_ptr);
            auto fallback_struct_safe =
                TranslateVirtualSafe<
                    kernel::xboxkrnl::X_KEDEBUGMONITORDATA*>(
                    memory_, fallback_struct_ptr);
            uint32_t current_callback =
                (current_struct_ptr != 0 && current_struct_safe.success &&
                 current_struct_safe.pointer)
                ? static_cast<uint32_t>(
                    current_struct_safe.pointer->callback_fn)
                    : 0;
            uint32_t fallback_callback =
                (fallback_struct_safe.success && fallback_struct_safe.pointer)
                ? static_cast<uint32_t>(
                    fallback_struct_safe.pointer->callback_fn)
                    : 0;
            bool repaired = false;
            if ((current_struct_ptr == 0 || current_callback == 0) &&
                fallback_callback != 0) {
              xe::store_and_swap<uint32_t>(slot_safe.pointer, fallback_struct_ptr);
              repaired = true;
            }
            debug_monitor_nudge_last_ms.store(now_ms,
                                              std::memory_order_relaxed);
            XELOGW(
                "DEBUG: MainThread debug-monitor nudge: pc={:08X} samples={} "
                "var_addr={:08X} current_struct={:08X} current_cb={:08X} "
                "fallback_struct={:08X} fallback_cb={:08X} repaired={}",
                pc, same_pc_samples, ke_debug_var_addr, current_struct_ptr,
                current_callback, fallback_struct_ptr, fallback_callback,
                repaired ? "YES" : "NO");
          }
        }
      }
    }

    bool allow_detail = true;
    uint32_t detail_interval_ms =
        cvars::log_main_thread_pc_detail_interval_ms;
    if (pc == last_detailed_pc) {
      if (detail_interval_ms == 0) {
        allow_detail = false;
      } else if (now_ms - last_detailed_time_ms < detail_interval_ms) {
        allow_detail = false;
      }
    }
    if (!allow_detail) {
      if (detail_interval_ms == 0) {
        XELOGI("DEBUG: MainThread detail suppressed (already dumped for {:08X})",
               pc);
      } else {
        XELOGI(
            "DEBUG: MainThread detail suppressed (last dump {} ms ago, "
            "interval={} ms)",
            static_cast<unsigned long long>(now_ms - last_detailed_time_ms),
            detail_interval_ms);
      }
      return;
    }
    last_detailed_pc = pc;
    last_detailed_time_ms = now_ms;

    XELOGI(
        "DEBUG: MainThread regs: r0={:016X} r2={:016X} r5={:016X} r6={:016X} "
        "r7={:016X} r8={:016X} r9={:016X} r10={:016X} r12={:016X} "
        "r28={:016X} r29={:016X} r30={:016X} r31={:016X}",
        static_cast<uint64_t>(ctx->r[0]), static_cast<uint64_t>(ctx->r[2]),
        static_cast<uint64_t>(ctx->r[5]), static_cast<uint64_t>(ctx->r[6]),
        static_cast<uint64_t>(ctx->r[7]), static_cast<uint64_t>(ctx->r[8]),
        static_cast<uint64_t>(ctx->r[9]), static_cast<uint64_t>(ctx->r[10]),
        static_cast<uint64_t>(ctx->r[12]), static_cast<uint64_t>(ctx->r[28]),
        static_cast<uint64_t>(ctx->r[29]), static_cast<uint64_t>(ctx->r[30]),
        static_cast<uint64_t>(ctx->r[31]));

    // If we're stuck on a branch, decode and inspect the target.
    if (memory_) {
      auto pc_safe = TranslateVirtualSafe<uint32_t*>(memory_, pc);
      if (pc_safe.success && pc_safe.pointer) {
        uint32_t instr = xe::load_and_swap<uint32_t>(pc_safe.pointer);
        uint32_t target = 0;
        bool lk = false;
        bool aa = false;
        if (DecodeBranchTarget(pc, instr, &target, &lk, &aa)) {
          XELOGW(
              "DEBUG: Stable branch detected: instr={:08X} pc={:08X} target={:08X} "
              "link={} absolute={}",
              instr, pc, target, lk ? "YES" : "NO", aa ? "YES" : "NO");

          const char* target_module = "<unknown>";
          const char* target_function = "<unknown>";
          uint32_t target_function_address = 0;
          const char* target_behavior = "<unknown>";
          if (processor_) {
            auto tmod = processor_->LookupModule(target);
            if (tmod) {
              target_module = tmod->name().c_str();
            }
            auto tfn = processor_->LookupFunction(target);
            if (tfn) {
              target_function = tfn->name().c_str();
              target_function_address = tfn->address();
              switch (tfn->behavior()) {
                case cpu::Function::Behavior::kDefault:
                  target_behavior = "Default";
                  break;
                case cpu::Function::Behavior::kProlog:
                  target_behavior = "Prolog";
                  break;
                case cpu::Function::Behavior::kEpilog:
                  target_behavior = "Epilog";
                  break;
                case cpu::Function::Behavior::kEpilogReturn:
                  target_behavior = "EpilogReturn";
                  break;
                case cpu::Function::Behavior::kBuiltin:
                  target_behavior = "Builtin";
                  break;
                case cpu::Function::Behavior::kExtern:
                  target_behavior = "Extern";
                  break;
                default:
                  target_behavior = "Unknown";
                  break;
              }
              XELOGI(
                  "DEBUG: Target function: module={} function={}({:08X}) "
                  "behavior={} status={}",
                  target_module, target_function, target_function_address,
                  target_behavior, static_cast<int>(tfn->status()));

              if (auto* guest_fn = dynamic_cast<cpu::GuestFunction*>(tfn)) {
                auto* exp = guest_fn->export_data();
                if (exp) {
                  XELOGI(
                      "DEBUG: Target export: name={} ordinal={} tags=0x{:08X} "
                      "implemented={}",
                      exp->name, exp->ordinal, exp->tags,
                      exp->is_implemented() ? "YES" : "NO");
                }
                if (guest_fn->extern_handler()) {
                  XELOGI("DEBUG: Target function has extern handler");
                }
              }
            }
          }

          // Disassemble the target for more context.
          xe::StringBuffer tgt_sb;
          XELOGI("DEBUG: Target disasm @ {:08X}:", target);
          std::array<uint32_t, 8> nested_targets{};
          size_t nested_target_count = 0;
          for (int i = 0; i < 12; ++i) {
            uint32_t addr = target + (i * 4);
            auto tgt_safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
            if (!tgt_safe.success || !tgt_safe.pointer) {
              XELOGW("DEBUG: {:08X}: <invalid> ({})", addr, tgt_safe.error);
              continue;
            }
            uint32_t code = xe::load_and_swap<uint32_t>(tgt_safe.pointer);
            tgt_sb.Reset();
            xe::cpu::ppc::DisasmPPC(addr, code, &tgt_sb);
            XELOGI("DEBUG: {:08X}: {:08X} {}", addr, code, tgt_sb.to_string());

            uint32_t nested_target = 0;
            bool nested_lk = false;
            bool nested_aa = false;
            if (DecodeBranchTarget(addr, code, &nested_target, &nested_lk,
                                   &nested_aa) &&
                nested_lk && nested_target != 0) {
              bool already_recorded = false;
              for (size_t n = 0; n < nested_target_count; ++n) {
                if (nested_targets[n] == nested_target) {
                  already_recorded = true;
                  break;
                }
              }
              if (!already_recorded &&
                  nested_target_count < nested_targets.size()) {
                nested_targets[nested_target_count++] = nested_target;
              }
            }
          }

          for (size_t n = 0; n < nested_target_count; ++n) {
            uint32_t nested_target = nested_targets[n];
            const char* nested_module_name = "<unknown>";
            const char* nested_function_name = "<unknown>";
            uint32_t nested_function_address = 0;
            if (processor_) {
              auto* nested_module = processor_->LookupModule(nested_target);
              if (nested_module) {
                nested_module_name = nested_module->name().c_str();
              }
              auto* nested_function = processor_->LookupFunction(nested_target);
              if (nested_function) {
                nested_function_name = nested_function->name().c_str();
                nested_function_address = nested_function->address();
              }
            }
            XELOGI(
                "DEBUG: Nested call target: addr={:08X} module={} function={}({:08X})",
                nested_target, nested_module_name, nested_function_name,
                nested_function_address);

            std::array<uint32_t, 8> nested_level2_targets{};
            size_t nested_level2_target_count = 0;

            for (int i = 0; i < 8; ++i) {
              uint32_t addr = nested_target + (i * 4);
              auto nested_safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
              if (!nested_safe.success || !nested_safe.pointer) {
                XELOGW("DEBUG: {:08X}: <invalid> ({})", addr, nested_safe.error);
                continue;
              }
              uint32_t code = xe::load_and_swap<uint32_t>(nested_safe.pointer);
              tgt_sb.Reset();
              xe::cpu::ppc::DisasmPPC(addr, code, &tgt_sb);
              XELOGI("DEBUG: {:08X}: {:08X} {}", addr, code, tgt_sb.to_string());

              uint32_t nested_level2_target = 0;
              bool nested_level2_lk = false;
              bool nested_level2_aa = false;
              if (DecodeBranchTarget(addr, code, &nested_level2_target,
                                     &nested_level2_lk,
                                     &nested_level2_aa) &&
                  nested_level2_lk && nested_level2_target != 0) {
                bool already_recorded = false;
                for (size_t k = 0; k < nested_level2_target_count; ++k) {
                  if (nested_level2_targets[k] == nested_level2_target) {
                    already_recorded = true;
                    break;
                  }
                }
                if (!already_recorded &&
                    nested_level2_target_count < nested_level2_targets.size()) {
                  nested_level2_targets[nested_level2_target_count++] =
                      nested_level2_target;
                }
              }
            }

            for (size_t k = 0; k < nested_level2_target_count; ++k) {
              uint32_t nested_level2_target = nested_level2_targets[k];
              const char* nested_level2_module_name = "<unknown>";
              const char* nested_level2_function_name = "<unknown>";
              uint32_t nested_level2_function_address = 0;
              if (processor_) {
                auto* nested_level2_module =
                    processor_->LookupModule(nested_level2_target);
                if (nested_level2_module) {
                  nested_level2_module_name =
                      nested_level2_module->name().c_str();
                }
                auto* nested_level2_function =
                    processor_->LookupFunction(nested_level2_target);
                if (nested_level2_function) {
                  nested_level2_function_name =
                      nested_level2_function->name().c_str();
                  nested_level2_function_address =
                      nested_level2_function->address();
                }
              }
              XELOGI(
                  "DEBUG: Nested level-2 call target: addr={:08X} module={} "
                  "function={}({:08X})",
                  nested_level2_target, nested_level2_module_name,
                  nested_level2_function_name, nested_level2_function_address);

              for (int i = 0; i < 6; ++i) {
                uint32_t addr = nested_level2_target + (i * 4);
                auto nested_level2_safe =
                    TranslateVirtualSafe<uint32_t*>(memory_, addr);
                if (!nested_level2_safe.success ||
                    !nested_level2_safe.pointer) {
                  XELOGW("DEBUG: {:08X}: <invalid> ({})", addr,
                         nested_level2_safe.error);
                  continue;
                }
                uint32_t code =
                    xe::load_and_swap<uint32_t>(nested_level2_safe.pointer);
                tgt_sb.Reset();
                xe::cpu::ppc::DisasmPPC(addr, code, &tgt_sb);
                XELOGI("DEBUG: {:08X}: {:08X} {}", addr, code,
                       tgt_sb.to_string());
              }
            }
          }
        }
      }
    }

    // Dump stack: current frame then walk back-chain so the dump is "full"
    // (multiple frames) instead of a single mostly-empty frame.
    if (memory_) {
      auto stack_safe = TranslateVirtualSafe<uint32_t*>(memory_, sp);
      if (stack_safe.success && stack_safe.pointer) {
        XELOGI("DEBUG: MainThread stack dump @ r1={:08X} (first 16 dwords):", sp);
        for (int i = 0; i < 16; ++i) {
          uint32_t addr = sp + (i * 4);
          uint32_t value = xe::load_and_swap<uint32_t>(stack_safe.pointer + i);
          XELOGI("DEBUG: [SP+{:02X}] {:08X}", i * 4, value);
        }
        // PowerPC back-chain walk: [frame_sp+0] = previous r1, [frame_sp+8] =
        // saved LR. Walk up to 16 frames so the log shows a full call stack.
        constexpr int kMaxBackChainFrames = 16;
        uint32_t frame_sp = sp;
        int frame_index = 0;
        XELOGI("DEBUG: MainThread back-chain (up to {} frames):",
               kMaxBackChainFrames);
        while (frame_index < kMaxBackChainFrames) {
          auto frame_safe =
              TranslateVirtualSafe<uint32_t*>(memory_, frame_sp);
          if (!frame_safe.success || !frame_safe.pointer) {
            XELOGW("DEBUG: frame {}: {:08X} <invalid>", frame_index, frame_sp);
            break;
          }
          uint32_t next_sp =
              xe::load_and_swap<uint32_t>(frame_safe.pointer + 0);
          uint32_t saved_lr =
              xe::load_and_swap<uint32_t>(frame_safe.pointer + 2);
          XELOGI(
              "DEBUG: frame {}: sp={:08X} [sp+0]={:08X} [sp+8]={:08X} (saved LR)",
              frame_index, frame_sp, next_sp, saved_lr);
          if (next_sp == 0) {
            XELOGI("DEBUG: (null back-chain, end of walk)");
            break;
          }
          if (next_sp < stack_limit || next_sp > stack_base) {
            XELOGW("DEBUG: (next_sp {:08X} outside stack {:08X}-{:08X}, stop)",
                   next_sp, stack_limit, stack_base);
            break;
          }
          if (next_sp <= frame_sp) {
            XELOGW("DEBUG: (next_sp {:08X} not above current, possible cycle)",
                   next_sp);
            break;
          }
          frame_sp = next_sp;
          ++frame_index;
        }
      } else {
        XELOGW("DEBUG: Stack dump failed @ {:08X}: {}", sp, stack_safe.error);
      }

      uint32_t base = sp >= 0x40 ? sp - 0x40 : sp;
      XELOGI("DEBUG: MainThread stack window @ {:08X}..{:08X}:", base,
             base + (32 * 4) - 4);
      for (int i = 0; i < 32; ++i) {
        uint32_t addr = base + (i * 4);
        auto word_safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
        if (!word_safe.success || !word_safe.pointer) {
          XELOGW("DEBUG: [{:08X}] <invalid> ({})", addr, word_safe.error);
          continue;
        }
        uint32_t value = xe::load_and_swap<uint32_t>(word_safe.pointer);
        XELOGI("DEBUG: [{:08X}] {:08X}", addr, value);
      }
    }

    // Also disassemble around LR to see return context if available.
    if (memory_ && lr != 0) {
      xe::StringBuffer lr_sb;
      XELOGI("DEBUG: LR disasm @ {:08X}:", lr);
      for (int i = -2; i <= 1; ++i) {
        uint32_t addr = lr + (i * 4);
        auto lr_safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
        if (!lr_safe.success || !lr_safe.pointer) {
          XELOGW("DEBUG: {:08X}: <invalid> ({})", addr, lr_safe.error);
          continue;
        }
        uint32_t code = xe::load_and_swap<uint32_t>(lr_safe.pointer);
        lr_sb.Reset();
        xe::cpu::ppc::DisasmPPC(addr, code, &lr_sb);
        XELOGI("DEBUG: {:08X}: {:08X} {}", addr, code, lr_sb.to_string());
      }
    }
  }

  // Disassemble a few instructions at the current PC for visibility.
  if (memory_) {
    auto safe = TranslateVirtualSafe<uint32_t*>(memory_, pc);
    if (!safe.success || !safe.pointer) {
      XELOGW("DEBUG: PC disasm failed at {:08X}: {}", pc, safe.error);
    } else {
      xe::StringBuffer sb;
      for (int i = 0; i < 4; ++i) {
        uint32_t addr = pc + (i * 4);
        uint32_t code = xe::load_and_swap<uint32_t>(safe.pointer + i);
        sb.Reset();
        xe::cpu::ppc::DisasmPPC(addr, code, &sb);
        XELOGI("DEBUG: PC disasm {:08X}: {:08X} {}", addr, code, sb.to_string());
        if (processor_) {
          uint32_t target = 0;
          bool lk = false;
          bool aa = false;
          if (DecodeBranchTarget(addr, code, &target, &lk, &aa)) {
            auto* target_module = processor_->LookupModule(target);
            auto* target_function = processor_->LookupFunction(target);
            const char* module_name =
                target_module ? target_module->name().c_str() : "<unknown>";
            const char* function_name =
                target_function ? target_function->name().c_str() : "<unknown>";
            const cpu::Export* target_export = nullptr;
            if (auto* guest_fn = dynamic_cast<cpu::GuestFunction*>(
                    target_function)) {
              target_export = guest_fn->export_data();
            }
            if (target_export &&
                kernel::xboxkrnl::IsGpuBootstrapOrdinal(target_export->ordinal)) {
              XELOGW(
                  "RING BUFFER: callback path main-thread branch addr={:08X} "
                  "target={:08X} link={} absolute={} module={} function={} "
                  "ordinal=0x{:03X} export={}",
                  addr, target, lk ? "YES" : "NO", aa ? "YES" : "NO",
                  module_name, function_name, target_export->ordinal,
                  target_export->name ? target_export->name : "<null>");
            } else if (target_export &&
                       target_export->ordinal == 0x01D5) {
              XELOGW(
                  "RING BUFFER: callback path main-thread branch hits "
                  "VdSetGraphicsInterruptCallback directly addr={:08X} "
                  "target={:08X} function={}",
                  addr, target, function_name);
            }
          }
        }
      }
    }
  }

  if (!cvars::track_guest_pc) {
    XELOGW("DEBUG: MainThread PC sampling note: track_guest_pc is disabled");
  } else if (pc == 0) {
    XELOGW(
        "DEBUG: MainThread PC sampling note: current_pc is 0 (guest code may not "
        "have executed yet)");
  }
}

namespace {

GpuDispatchStallSnapshot& GetGpuDispatchStallSnapshotStorage() {
  static GpuDispatchStallSnapshot snapshot;
  return snapshot;
}

}  // namespace

void Emulator::LogMainThreadGpuDispatchStallSample(const char* reason,
                                                   uint32_t thunk_01C3,
                                                   uint32_t thunk_01D5,
                                                   uint32_t thunk_025B) const {
  LogMainThreadPcSample(reason ? reason : "gpu-dispatch-stall");

  if (!main_thread_) {
    return;
  }
  auto* thread = main_thread_.get();
  auto* thread_state = thread->thread_state();
  auto* ctx = thread_state ? thread_state->context() : nullptr;
  if (!ctx || !memory_) {
    return;
  }

  uint32_t pc = ctx->current_pc;
  uint32_t lr = static_cast<uint32_t>(ctx->lr);
  uint32_t ctr = static_cast<uint32_t>(ctx->ctr);
  uint64_t now_ms = Clock::QueryHostUptimeMillis();

  static uint32_t last_dispatch_stall_pc = 0;
  static uint32_t same_dispatch_stall_samples = 0;
  if (pc == last_dispatch_stall_pc) {
    ++same_dispatch_stall_samples;
  } else {
    last_dispatch_stall_pc = pc;
    same_dispatch_stall_samples = 0;
  }

  XELOGW(
      "RING BUFFER: main-thread dispatch stall sample reason={} pc={:08X} "
      "lr={:08X} ctr={:08X} thread_id={} repeat_samples={} "
      "thunk_01C3={:08X} thunk_01D5={:08X} thunk_025B={:08X} now_ms={}",
      reason ? reason : "<null>", pc, lr, ctr, thread->thread_id(),
      same_dispatch_stall_samples, thunk_01C3, thunk_01D5, thunk_025B, now_ms);

  auto classify_target = [&](uint32_t target) -> const char* {
    if (target == 0) {
      return nullptr;
    }
    if (thunk_01C3 != 0 && target == thunk_01C3) {
      return "VdInitializeRingBuffer";
    }
    if (thunk_01D5 != 0 && target == thunk_01D5) {
      return "VdSetGraphicsInterruptCallback";
    }
    if (thunk_025B != 0 && target == thunk_025B) {
      return "VdSwap";
    }
    return nullptr;
  };

  auto log_direct_match = [&](const char* source, uint32_t addr, uint32_t target,
                              bool lk, bool aa) {
    const char* target_name = classify_target(target);
    if (!target_name) {
      return;
    }
    XELOGW(
        "RING BUFFER: main-thread dispatch stall direct branch source={} "
        "addr={:08X} target={:08X} thunk_name={} link={} absolute={}",
        source ? source : "<unknown>", addr, target, target_name,
        lk ? "YES" : "NO", aa ? "YES" : "NO");
  };
  auto* current_module = processor_ ? processor_->LookupModule(pc) : nullptr;
  const char* current_module_name =
      current_module ? current_module->name().c_str() : "<unknown>";
  auto describe_function_behavior = [](const cpu::Function* function)
      -> const char* {
    if (!function) {
      return "Missing";
    }
    switch (function->behavior()) {
      case cpu::Function::Behavior::kDefault:
        return "Default";
      case cpu::Function::Behavior::kProlog:
        return "Prolog";
      case cpu::Function::Behavior::kEpilog:
        return "Epilog";
      case cpu::Function::Behavior::kEpilogReturn:
        return "EpilogReturn";
      case cpu::Function::Behavior::kBuiltin:
        return "Builtin";
      case cpu::Function::Behavior::kExtern:
        return "Extern";
    }
    return "Unknown";
  };
  auto describe_function_label = [](uint32_t address,
                                    const cpu::Function* function)
      -> std::string {
    if (!function) {
      return "<unknown>";
    }
    if (!function->name().empty()) {
      return function->name();
    }
    return fmt::format("({:08X})", address);
  };
  bool branch_seen = false;
  bool critical_branch_seen = false;
  bool backward_branch_seen = false;
  bool same_module_branch_seen = false;
  uint32_t first_backward_branch_addr = 0;
  uint32_t first_backward_branch_target = 0;
  std::ostringstream branch_chain_stream;
  bool branch_chain_has_entry = false;
  std::vector<uint32_t> unique_targets;

  if (classify_target(pc)) {
    XELOGW(
        "RING BUFFER: main-thread dispatch stall current_pc matches "
        "critical thunk pc={:08X} name={}",
        pc, classify_target(pc));
  }
  if (classify_target(lr)) {
    XELOGW(
        "RING BUFFER: main-thread dispatch stall lr matches critical thunk "
        "lr={:08X} name={}",
        lr, classify_target(lr));
  }

  auto scan_window = [&](uint32_t base, int begin_word, int end_word,
                         const char* label) {
    if (base == 0) {
      return;
    }
    for (int i = begin_word; i <= end_word; ++i) {
      uint32_t addr = base + (i * 4);
      auto safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
      if (!safe.success || !safe.pointer) {
        XELOGW(
            "RING BUFFER: main-thread dispatch stall {} probe unreadable "
            "addr={:08X} error={}",
            label ? label : "<window>", addr, safe.error);
        continue;
      }
      uint32_t code = xe::load_and_swap<uint32_t>(safe.pointer);
      uint32_t target = 0;
      bool lk = false;
      bool aa = false;
      if (DecodeBranchTarget(addr, code, &target, &lk, &aa)) {
        branch_seen = true;
        const char* target_name = classify_target(target);
        if (target_name) {
          critical_branch_seen = true;
        }
        if (target < addr) {
          backward_branch_seen = true;
          if (!first_backward_branch_addr) {
            first_backward_branch_addr = addr;
            first_backward_branch_target = target;
          }
        }
        auto* target_module = processor_ ? processor_->LookupModule(target) : nullptr;
        auto* target_function =
            processor_ ? processor_->LookupFunction(target) : nullptr;
        const char* target_module_name =
            target_module ? target_module->name().c_str() : "<unknown>";
        std::string target_function_label =
            describe_function_label(target, target_function);
        int target_status =
            target_function ? static_cast<int>(target_function->status()) : -1;
        const cpu::Export* target_export = nullptr;
        if (auto* guest_function =
                dynamic_cast<cpu::GuestFunction*>(target_function)) {
          target_export = guest_function->export_data();
        }
        bool same_module =
            current_module && target_module && current_module == target_module;
        if (same_module) {
          same_module_branch_seen = true;
        }
        if (std::find(unique_targets.begin(), unique_targets.end(), target) ==
            unique_targets.end()) {
          unique_targets.push_back(target);
        }
        if (branch_chain_has_entry) {
          branch_chain_stream << " | ";
        }
        branch_chain_stream
            << fmt::format(
                   "{}:{:08X}->{:08X}(module={} function={} behavior={} "
                   "status={} export={} same_module={})",
                   label ? label : "<window>", addr, target,
                   target_module_name, target_function_label,
                   describe_function_behavior(target_function),
                   target_status,
                   target_export
                       ? fmt::format(
                             "0x{:03X}:{}",
                             target_export->ordinal,
                             target_export->name ? target_export->name
                                                 : "<unnamed>")
                       : "none",
                   same_module ? "YES" : "NO");
        branch_chain_has_entry = true;
        XELOGW(
            "RING BUFFER: main-thread dispatch stall branch source={} "
            "addr={:08X} target={:08X} link={} absolute={} "
            "critical_thunk={} target_module={} target_function={} "
            "target_status={} same_module={}",
            label ? label : "<window>", addr, target, lk ? "YES" : "NO",
            aa ? "YES" : "NO", target_name ? target_name : "NO",
            target_module_name, target_function_label, target_status,
            same_module ? "YES" : "NO");
        log_direct_match(label, addr, target, lk, aa);
      }
    }
  };

  scan_window(pc, 0, 3, "pc");
  scan_window(lr, -2, 1, "lr");
  std::string branch_chain =
      branch_chain_has_entry ? branch_chain_stream.str() : "<none>";
  static std::string last_branch_chain;
  static uint32_t same_branch_chain_samples = 0;
  if (branch_chain == last_branch_chain) {
    ++same_branch_chain_samples;
  } else {
    last_branch_chain = branch_chain;
    same_branch_chain_samples = 0;
  }
  XELOGW(
      "RING BUFFER: main-thread dispatch stall chain repeat_samples={} "
      "chain='{}'",
      same_branch_chain_samples, branch_chain);
  std::ostringstream target_summary_stream;
  bool target_summary_has_entry = false;
  bool all_targets_default_same_module = !unique_targets.empty();
  bool all_targets_prolog_same_module = !unique_targets.empty();
  bool any_target_export = false;
  bool any_target_dynamic_loader = false;
  bool any_target_gpu_dispatch = false;
  bool any_target_kernel_export = false;
  uint32_t first_xex_check_executable_privilege_addr = 0;
  uint32_t first_xex_check_executable_privilege_target = 0;
  uint32_t first_xex_check_executable_privilege_instr = 0;
  std::string first_xex_check_executable_privilege_instr_text;
  for (uint32_t target : unique_targets) {
    auto* target_module = processor_ ? processor_->LookupModule(target) : nullptr;
    auto* target_function =
        processor_ ? processor_->LookupFunction(target) : nullptr;
    const char* target_module_name =
        target_module ? target_module->name().c_str() : "<unknown>";
    std::string target_function_label =
        describe_function_label(target, target_function);
    const char* target_behavior = describe_function_behavior(target_function);
    int target_status =
        target_function ? static_cast<int>(target_function->status()) : -1;
    const cpu::Export* target_export = nullptr;
    if (auto* guest_function =
            dynamic_cast<cpu::GuestFunction*>(target_function)) {
      target_export = guest_function->export_data();
    }
    if (target_export) {
      any_target_export = true;
    }
    bool target_same_module =
        current_module && target_module && current_module == target_module;
    bool target_default_same_module =
        target_same_module && target_function &&
        target_function->behavior() == cpu::Function::Behavior::kDefault;
    if (!target_default_same_module) {
      all_targets_default_same_module = false;
    }
    bool target_prolog_same_module =
        target_same_module && target_function &&
        target_function->behavior() == cpu::Function::Behavior::kProlog;
    if (!target_prolog_same_module) {
      all_targets_prolog_same_module = false;
    }
    std::ostringstream disasm_stream;
    bool disasm_has_entry = false;
    if (memory_) {
      for (int i = 0; i < 3; ++i) {
        uint32_t addr = target + (i * 4);
        auto safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
        if (!safe.success || !safe.pointer) {
          if (disasm_has_entry) {
            disasm_stream << " ; ";
          }
          disasm_stream << fmt::format("{:08X}:<invalid:{}>", addr, safe.error);
          disasm_has_entry = true;
          continue;
        }
        uint32_t code = xe::load_and_swap<uint32_t>(safe.pointer);
        xe::StringBuffer sb;
        xe::cpu::ppc::DisasmPPC(addr, code, &sb);
        if (disasm_has_entry) {
          disasm_stream << " ; ";
        }
        disasm_stream << fmt::format("{:08X}:{}", addr, sb.to_string());
        disasm_has_entry = true;
      }
    }
    uint32_t scan_begin =
        target_function ? target_function->address() : target;
    uint32_t scan_end = scan_begin + (32 * 4);
    if (target_function && target_function->has_end_address() &&
        target_function->end_address() > scan_begin) {
      scan_end = std::min<uint32_t>(target_function->end_address(), scan_end);
    }
    uint32_t scan_instructions =
        scan_end > scan_begin ? ((scan_end - scan_begin) / 4) : 0;
    uint32_t scan_branch_count = 0;
    uint32_t scan_local_branch_count = 0;
    uint32_t scan_backward_branch_count = 0;
    bool scan_hits_dynamic_loader = false;
    bool scan_hits_gpu_dispatch = false;
    bool scan_hits_kernel_export = false;
    bool scan_hits_xex_check_executable_privilege = false;
    std::string first_dynamic_loader_hit;
    std::string first_gpu_dispatch_hit;
    std::string first_kernel_export_hit;
    std::string first_xex_check_executable_privilege_hit;
    if (memory_ && scan_instructions != 0) {
      for (uint32_t i = 0; i < scan_instructions; ++i) {
        uint32_t addr = scan_begin + (i * 4);
        auto safe = TranslateVirtualSafe<uint32_t*>(memory_, addr);
        if (!safe.success || !safe.pointer) {
          continue;
        }
        uint32_t code = xe::load_and_swap<uint32_t>(safe.pointer);
        uint32_t branch_target = 0;
        bool branch_lk = false;
        bool branch_aa = false;
        if (!DecodeBranchTarget(addr, code, &branch_target, &branch_lk,
                                &branch_aa)) {
          continue;
        }
        ++scan_branch_count;
        auto* branch_target_module =
            processor_ ? processor_->LookupModule(branch_target) : nullptr;
        auto* branch_target_function =
            processor_ ? processor_->LookupFunction(branch_target) : nullptr;
        const cpu::Export* branch_target_export = nullptr;
        if (auto* guest_function =
                dynamic_cast<cpu::GuestFunction*>(branch_target_function)) {
          branch_target_export = guest_function->export_data();
        }
        bool branch_same_module =
            target_module && branch_target_module &&
            target_module == branch_target_module;
        if (branch_same_module) {
          ++scan_local_branch_count;
        }
        if (branch_target < addr) {
          ++scan_backward_branch_count;
        }
        if (branch_target == thunk_01C3 || branch_target == thunk_01D5 ||
            branch_target == thunk_025B ||
            (branch_target_export &&
             kernel::xboxkrnl::IsGpuBootstrapOrdinal(branch_target_export->ordinal))) {
          scan_hits_gpu_dispatch = true;
          if (first_gpu_dispatch_hit.empty()) {
            first_gpu_dispatch_hit = fmt::format(
                "{:08X}->{:08X} export={} ordinal=0x{:03X}",
                addr, branch_target,
                branch_target_export && branch_target_export->name
                    ? branch_target_export->name
                    : "<none>",
                branch_target_export ? branch_target_export->ordinal : 0);
          }
        }
        if (branch_target_export && branch_target_export->name) {
          std::string_view export_name = branch_target_export->name;
          if (export_name == "XexGetProcedureAddress" ||
              export_name == "XexGetModuleHandle") {
            scan_hits_dynamic_loader = true;
            if (first_dynamic_loader_hit.empty()) {
              first_dynamic_loader_hit = fmt::format(
                  "{:08X}->{:08X} export={}", addr, branch_target,
                  branch_target_export->name);
            }
          }
          if (export_name == "XexCheckExecutablePrivilege") {
            scan_hits_xex_check_executable_privilege = true;
            if (first_xex_check_executable_privilege_hit.empty()) {
              first_xex_check_executable_privilege_hit = fmt::format(
                  "{:08X}->{:08X} export={} ordinal=0x{:03X}", addr,
                  branch_target, branch_target_export->name,
                  branch_target_export->ordinal);
              first_xex_check_executable_privilege_addr = addr;
              first_xex_check_executable_privilege_target = branch_target;
              first_xex_check_executable_privilege_instr = code;
              StringBuffer first_privilege_instr_stream;
              xe::cpu::ppc::DisasmPPC(addr, code,
                                      &first_privilege_instr_stream);
              first_xex_check_executable_privilege_instr_text =
                  first_privilege_instr_stream.to_string();
            }
          }
          if (!kernel::xboxkrnl::IsGpuBootstrapOrdinal(branch_target_export->ordinal)) {
            scan_hits_kernel_export = true;
            if (first_kernel_export_hit.empty()) {
              first_kernel_export_hit = fmt::format(
                  "{:08X}->{:08X} export={} ordinal=0x{:03X}", addr,
                  branch_target, branch_target_export->name,
                  branch_target_export->ordinal);
            }
          }
        }
      }
    }
    any_target_dynamic_loader =
        any_target_dynamic_loader || scan_hits_dynamic_loader;
    any_target_gpu_dispatch = any_target_gpu_dispatch || scan_hits_gpu_dispatch;
    any_target_kernel_export =
        any_target_kernel_export || scan_hits_kernel_export;
    std::string scan_summary = fmt::format(
        "range={:08X}-{:08X} ins={} branches={} local={} backward={} "
        "dynamic_loader={} gpu_dispatch={} kernel_export={} "
        "xex_privilege_check={} first_dynamic={} first_gpu={} "
        "first_kernel={} first_privilege={} first_privilege_instr={}",
        scan_begin, scan_end, scan_instructions, scan_branch_count,
        scan_local_branch_count, scan_backward_branch_count,
        scan_hits_dynamic_loader ? "YES" : "NO",
        scan_hits_gpu_dispatch ? "YES" : "NO",
        scan_hits_kernel_export ? "YES" : "NO",
        scan_hits_xex_check_executable_privilege ? "YES" : "NO",
        first_dynamic_loader_hit.empty() ? "<none>" : first_dynamic_loader_hit,
        first_gpu_dispatch_hit.empty() ? "<none>" : first_gpu_dispatch_hit,
        first_kernel_export_hit.empty() ? "<none>" : first_kernel_export_hit,
        first_xex_check_executable_privilege_hit.empty()
            ? "<none>"
            : first_xex_check_executable_privilege_hit,
        first_xex_check_executable_privilege_instr_text.empty()
            ? "<none>"
            : first_xex_check_executable_privilege_instr_text);
    std::string export_desc =
        target_export
            ? fmt::format(
                  "0x{:03X}:{}",
                  target_export->ordinal,
                  target_export->name ? target_export->name : "<unnamed>")
            : "none";
    std::string target_summary = fmt::format(
        "{:08X}(module={} function={} behavior={} status={} export={} "
        "same_module={} disasm=[{}] scan=[{}])",
        target, target_module_name, target_function_label, target_behavior,
        target_status, export_desc, target_same_module ? "YES" : "NO",
        disasm_has_entry ? disasm_stream.str() : "<none>", scan_summary);
    XELOGW("RING BUFFER: main-thread dispatch stall target summary {}",
           target_summary);
    if (target_summary_has_entry) {
      target_summary_stream << " | ";
    }
    target_summary_stream << target_summary;
    target_summary_has_entry = true;
  }
  std::string target_summaries =
      target_summary_has_entry ? target_summary_stream.str() : "<none>";
  auto classify_privilege_execution_probe = [&](uint32_t value)
      -> const char* {
    if (!first_xex_check_executable_privilege_addr ||
        !first_xex_check_executable_privilege_target) {
      return "absent";
    }
    if (!value) {
      return "zero";
    }
    if (value == first_xex_check_executable_privilege_addr) {
      return "callsite";
    }
    if (value == first_xex_check_executable_privilege_target) {
      return "thunk_target";
    }
    if (value > first_xex_check_executable_privilege_target &&
        value < first_xex_check_executable_privilege_target + 8) {
      return "thunk_body";
    }
    if ((value + 8) >= first_xex_check_executable_privilege_addr &&
        value <= first_xex_check_executable_privilege_addr + 8) {
      return "near_callsite";
    }
    if ((value + 8) >= first_xex_check_executable_privilege_target &&
        value <= first_xex_check_executable_privilege_target + 8) {
      return "near_thunk";
    }
    return "other";
  };
  std::string privilege_execution_probe =
      (first_xex_check_executable_privilege_addr &&
       first_xex_check_executable_privilege_target)
          ? fmt::format(
                "callsite={:08X} target={:08X} instr={} pc_probe={} "
                "lr_probe={} ctr_probe={} current_pc={:08X} current_lr={:08X} "
                "current_ctr={:08X}",
                first_xex_check_executable_privilege_addr,
                first_xex_check_executable_privilege_target,
                first_xex_check_executable_privilege_instr_text.empty()
                    ? "<none>"
                    : first_xex_check_executable_privilege_instr_text,
                classify_privilege_execution_probe(pc),
                classify_privilege_execution_probe(lr),
                classify_privilege_execution_probe(ctr), pc, lr, ctr)
          : "<none>";
  auto& snapshot = GetGpuDispatchStallSnapshotStorage();
  uint32_t zero_context_samples = 0;
  uint32_t valid_context_samples = 0;
  uint32_t last_nonzero_pc = 0;
  uint32_t last_nonzero_lr = 0;
  std::string last_nonzero_state;
  std::string last_nonzero_chain;
  std::string last_nonzero_targets;
  uint64_t last_nonzero_ms = 0;
  {
    std::lock_guard<std::mutex> lock(snapshot.mutex);
    zero_context_samples = snapshot.zero_context_samples;
    valid_context_samples = snapshot.valid_context_samples;
    last_nonzero_pc = snapshot.last_nonzero_pc;
    last_nonzero_lr = snapshot.last_nonzero_lr;
    last_nonzero_state = snapshot.last_nonzero_state;
    last_nonzero_chain = snapshot.last_nonzero_chain;
    last_nonzero_targets = snapshot.last_nonzero_targets;
    last_nonzero_ms = snapshot.last_nonzero_ms;
  }
  bool context_ready = pc != 0 || lr != 0;
  if (context_ready) {
    ++valid_context_samples;
  } else {
    ++zero_context_samples;
  }

  const char* stall_state = "unknown";
  std::string stall_detail = "no dispatch stall classification available";
  if (critical_branch_seen) {
    stall_state = "critical_dispatch_window";
    stall_detail =
        "sampled pc/lr branch window already contains a critical GPU thunk";
  } else if (pc == 0 && lr == 0 && !branch_seen) {
    stall_state = "guest_main_context_not_ready";
    if (last_nonzero_ms != 0) {
      uint64_t last_nonzero_age_ms =
          now_ms >= last_nonzero_ms ? (now_ms - last_nonzero_ms) : 0;
      stall_detail = fmt::format(
          "guest main thread has not reached a valid PC/LR yet, so no "
          "dispatch path can execute (last_nonzero_state={} "
          "last_nonzero_age_ms={} last_nonzero_pc={:08X} "
          "last_nonzero_lr={:08X} last_nonzero_chain='{}')",
          last_nonzero_state.empty() ? "<none>" : last_nonzero_state,
          last_nonzero_age_ms, last_nonzero_pc, last_nonzero_lr,
          last_nonzero_chain.empty() ? "<none>" : last_nonzero_chain);
    } else {
      stall_detail =
          "guest main thread has not reached a valid PC/LR yet, so no "
          "dispatch path can execute";
    }
  } else if (branch_seen && all_targets_prolog_same_module &&
             !any_target_export && !any_target_dynamic_loader &&
             !any_target_gpu_dispatch && !any_target_kernel_export) {
    stall_state = "startup_prolog_helper_only";
    stall_detail = fmt::format(
        "sampled pc/lr branch window resolves only to same-module prolog "
        "helper targets before any loader/GPU dispatch path is observed "
        "(current_module={} chain={} targets={})",
        current_module_name, branch_chain, target_summaries);
  } else if (branch_seen && backward_branch_seen && same_module_branch_seen &&
             all_targets_default_same_module && !any_target_export &&
             !any_target_dynamic_loader && !any_target_gpu_dispatch &&
             target_summaries.find("xex_privilege_check=YES") !=
                 std::string::npos) {
    stall_state = "title_local_privilege_gate_before_gpu_dispatch";
    stall_detail = fmt::format(
        "sampled pc/lr branch window stays inside same-module title "
        "functions with default behavior; bounded target scans show "
        "XexCheckExecutablePrivilege before any XexGetModuleHandle/"
        "XexGetProcedureAddress or GPU dispatch path; backward branch "
        "{:08X}->{:08X}; privilege_probe={} targets={}",
        first_backward_branch_addr, first_backward_branch_target,
        privilege_execution_probe, target_summaries);
  } else if (branch_seen && backward_branch_seen && same_module_branch_seen &&
             all_targets_default_same_module && !any_target_export &&
             !any_target_dynamic_loader && !any_target_gpu_dispatch &&
             !any_target_kernel_export) {
    stall_state = "title_local_busy_loop_without_import_dispatch";
    stall_detail = fmt::format(
        "sampled pc/lr branch window stays entirely inside same-module title "
        "functions with default behavior; bounded scans of those targets show "
        "no XexGetModuleHandle/XexGetProcedureAddress, no GPU dispatch, and "
        "no kernel export branches; backward branch {:08X}->{:08X}; "
        "targets={}",
        first_backward_branch_addr, first_backward_branch_target,
        target_summaries);
  } else if (branch_seen && backward_branch_seen && same_module_branch_seen &&
             all_targets_default_same_module && !any_target_export) {
    stall_state = "title_local_busy_loop_before_gpu_dispatch";
    stall_detail = fmt::format(
        "sampled pc/lr branch window stays entirely inside same-module title "
        "functions with default behavior and no exports; backward branch "
        "{:08X}->{:08X}; targets={}",
        first_backward_branch_addr, first_backward_branch_target,
        target_summaries);
  } else if (branch_seen && backward_branch_seen && same_module_branch_seen) {
    stall_state = "busy_loop_before_gpu_dispatch";
    stall_detail = fmt::format(
        "sampled pc/lr branch window stays inside module {} and includes a "
        "backward branch {:08X}->{:08X}; no critical GPU thunk branch was "
        "observed; chain={} targets={}",
        current_module_name, first_backward_branch_addr,
        first_backward_branch_target, branch_chain, target_summaries);
  } else if (branch_seen) {
    stall_state = "non_gpu_call_chain";
    stall_detail = fmt::format(
        "sampled pc/lr branch window contains non-critical branches only "
        "(current_module={} chain={} targets={})",
        current_module_name, branch_chain, target_summaries);
  } else {
    stall_state = "no_branch_window_activity";
    stall_detail =
        "no direct branch instructions were found in the sampled pc/lr "
        "windows";
  }
  XELOGW(
      "RING BUFFER: main-thread dispatch stall classification state={} "
      "detail='{}' branch_seen={} critical_branch_seen={} "
      "backward_branch_seen={} repeat_samples={} current_module={}",
      stall_state, stall_detail, branch_seen ? "YES" : "NO",
      critical_branch_seen ? "YES" : "NO",
      backward_branch_seen ? "YES" : "NO", same_dispatch_stall_samples,
      current_module_name);
  if (std::string_view(stall_state) ==
          "title_local_privilege_gate_before_gpu_dispatch" &&
      privilege_execution_probe != "<none>" &&
      (same_dispatch_stall_samples <= 5 ||
       (same_dispatch_stall_samples % 120) == 0)) {
    XELOGW("RING BUFFER: title pre-gpu privilege execution probe {}",
           privilege_execution_probe);
  }
  uint64_t last_nonzero_age_ms =
      last_nonzero_ms && now_ms >= last_nonzero_ms ? (now_ms - last_nonzero_ms)
                                                   : 0;
  XELOGW(
      "RING BUFFER: main-thread dispatch stall lifecycle "
      "zero_context_samples={} valid_context_samples={} "
      "last_nonzero_state={} last_nonzero_age_ms={} "
      "last_nonzero_pc={:08X} last_nonzero_lr={:08X} "
      "last_nonzero_chain='{}' last_nonzero_targets='{}'",
      zero_context_samples, valid_context_samples,
      last_nonzero_state.empty() ? "<none>" : last_nonzero_state,
      last_nonzero_age_ms, last_nonzero_pc, last_nonzero_lr,
      last_nonzero_chain.empty() ? "<none>" : last_nonzero_chain,
      last_nonzero_targets.empty() ? "<none>" : last_nonzero_targets);
  {
    std::lock_guard<std::mutex> lock(snapshot.mutex);
    bool same_stall_state = snapshot.valid && snapshot.state == stall_state;
    snapshot.valid = true;
    snapshot.state = stall_state;
    snapshot.detail = stall_detail;
    snapshot.chain = branch_chain;
    snapshot.target_summaries = target_summaries;
    snapshot.repeat_samples = same_dispatch_stall_samples;
    snapshot.now_ms = now_ms;
    snapshot.state_first_ms =
        same_stall_state && snapshot.state_first_ms != 0
            ? snapshot.state_first_ms
            : now_ms;
    snapshot.zero_context_samples = zero_context_samples;
    snapshot.valid_context_samples = valid_context_samples;
    if (context_ready) {
      snapshot.last_nonzero_pc = pc;
      snapshot.last_nonzero_lr = lr;
      snapshot.last_nonzero_state = stall_state;
      snapshot.last_nonzero_chain = branch_chain;
      snapshot.last_nonzero_targets = target_summaries;
      snapshot.last_nonzero_ms = now_ms;
    }
  }
}

bool Emulator::GetLastGpuDispatchStallSnapshot(std::string* out_state,
                                               std::string* out_detail,
                                               std::string* out_chain,
                                               std::string* out_target_summaries,
                                               uint32_t* out_repeat_samples,
                                               uint64_t* out_now_ms) const {
  auto& snapshot = GetGpuDispatchStallSnapshotStorage();
  std::lock_guard<std::mutex> lock(snapshot.mutex);
  if (!snapshot.valid) {
    return false;
  }
  if (out_state) {
    *out_state = snapshot.state;
  }
  if (out_detail) {
    *out_detail = snapshot.detail;
  }
  if (out_chain) {
    *out_chain = snapshot.chain;
  }
  if (out_target_summaries) {
    *out_target_summaries = snapshot.target_summaries;
  }
  if (out_repeat_samples) {
    *out_repeat_samples = snapshot.repeat_samples;
  }
  if (out_now_ms) {
    *out_now_ms =
        snapshot.state_first_ms != 0 ? snapshot.state_first_ms : snapshot.now_ms;
  }
  return true;
}

bool Emulator::HasMainThread() const { return main_thread_ != nullptr; }

bool Emulator::HasGuestMainThread() const {
  return main_thread_ != nullptr && main_thread_->is_guest_thread() &&
         main_thread_->main_thread();
}

bool Emulator::IsGuestMainThreadRunning() const {
  return HasGuestMainThread() && main_thread_->is_running();
}

bool Emulator::EnsureGuestMainThreadRunning(const char* reason) {
  if (!HasGuestMainThread()) {
    return false;
  }
  if (main_thread_->is_running()) {
    return true;
  }

  uint32_t suspend_count = 0;
  X_STATUS resume_status = main_thread_->Resume(&suspend_count);
  bool running_now = main_thread_->is_running();
  XELOGW(
      "DEBUG: GUEST MAIN THREAD resume nudge: reason='{}' status={:08X} "
      "suspend_count={} running_now={} handle={:08X} thread_id={}",
      reason ? reason : "unspecified", static_cast<uint32_t>(resume_status),
      suspend_count, running_now ? "YES" : "NO", main_thread_->handle(),
      main_thread_->thread_id());
  return running_now;
}

void Emulator::CloseModuleLoadGate(const std::string_view reason) {
  bool was_closed =
      module_load_gate_closed_.exchange(true, std::memory_order_acq_rel);
  if (!was_closed) {
    XELOGW("DEBUG: Global module-load gate CLOSED (reason={})", reason);
  }
}

void Emulator::OpenModuleLoadGate(const std::string_view reason) {
  bool was_closed =
      module_load_gate_closed_.exchange(false, std::memory_order_acq_rel);
  if (was_closed) {
    XELOGI("DEBUG: Global module-load gate OPEN (reason={})", reason);
  }
}

bool Emulator::IsModuleLoadGateClosed() const {
  return module_load_gate_closed_.load(std::memory_order_acquire);
}

void Emulator::AddGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  // Check if already added.
  if (std::find(game_config_load_callbacks_.cbegin(),
                game_config_load_callbacks_.cend(),
                callback) != game_config_load_callbacks_.cend()) {
    return;
  }
  game_config_load_callbacks_.push_back(callback);
}

void Emulator::RemoveGameConfigLoadCallback(GameConfigLoadCallback* callback) {
  assert_not_null(callback);
  // Game config load callbacks handling is entirely in the UI thread.
  assert_true(!display_window_ ||
              display_window_->app_context().IsInUIThread());
  auto it = std::find(game_config_load_callbacks_.cbegin(),
                      game_config_load_callbacks_.cend(), callback);
  if (it == game_config_load_callbacks_.cend()) {
    return;
  }
  if (game_config_load_callback_loop_next_index_ != SIZE_MAX) {
    // Actualize the next callback index after the erasure from the vector.
    size_t existing_index =
        size_t(std::distance(game_config_load_callbacks_.cbegin(), it));
    if (game_config_load_callback_loop_next_index_ > existing_index) {
      --game_config_load_callback_loop_next_index_;
    }
  }
  game_config_load_callbacks_.erase(it);
}

std::string Emulator::FindLaunchModule() {
  XELOGI("DEBUG: FindLaunchModule ENTRY");
  std::string path("game:\\");

  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  XELOGI("DEBUG: XAM loader_data().launch_path: '{}'",
         xam->loader_data().launch_path);

  if (!xam->loader_data().launch_path.empty()) {
    std::string symbolic_link_path;
    XELOGI("DEBUG: Looking for symbolic link: '{}'", kDefaultGameSymbolicLink);
    if (kernel_state_->file_system()->FindSymbolicLink(kDefaultGameSymbolicLink,
                                                       symbolic_link_path)) {
      XELOGI("DEBUG: Found symbolic link: '{}' -> '{}'", kDefaultGameSymbolicLink,
             symbolic_link_path);
      std::filesystem::path file_path = symbolic_link_path;
      // Remove previous symbolic links.
      // Some titles can provide root within specific directory.
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultPartitionSymbolicLink);
      kernel_state_->file_system()->UnregisterSymbolicLink(
          kDefaultGameSymbolicLink);

      file_path /= std::filesystem::path(xam->loader_data().launch_path);

      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultPartitionSymbolicLink,
          xe::path_to_utf8(file_path.parent_path()));
      kernel_state_->file_system()->RegisterSymbolicLink(
          kDefaultGameSymbolicLink, xe::path_to_utf8(file_path.parent_path()));

      std::string result = xe::path_to_utf8(file_path);
      XELOGI("DEBUG: FindLaunchModule returning (from loader_data): '{}'", result);
      return result;
    } else {
      XELOGI("DEBUG: Symbolic link '{}' not found", kDefaultGameSymbolicLink);
    }
  }

  if (!cvars::launch_module.empty()) {
    std::string result = path + cvars::launch_module;
    XELOGI("DEBUG: FindLaunchModule returning (from cvars): '{}'", result);
    return result;
  }

  std::string result = path + "default.xex";
  XELOGI("DEBUG: FindLaunchModule returning (default): '{}'", result);
  return result;
}

static std::string format_version(xex2_version version) {
  // fmt::format doesn't like bit fields we use + to bypass it
  return fmt::format("{}.{}.{}.{}", +version.major, +version.minor,
                     +version.build, +version.qfe);
}

X_STATUS Emulator::CompleteLaunch(const std::filesystem::path& path,
                                  const std::string_view module_path) {
  printf("DEBUG: CompleteLaunch ENTRY\n");
  XELOGI("DEBUG: Emulator::CompleteLaunch ENTRY - DETAILED DEBUG");
  XELOGI("DEBUG: PATH: '{}'", path.string());
  XELOGI("DEBUG: MODULE_PATH: '{}'", module_path);
  XELOGI("DEBUG: IS_IN_UI_THREAD: {}",
         display_window_->app_context().IsInUIThread() ? "YES" : "NO");

  // Making changes to the UI (setting the icon) and executing game config
  // load callbacks which expect to be called from the UI thread.
  assert_true(display_window_->app_context().IsInUIThread());

  bool use_global_module_gate =
      cvars::complete_launch_global_module_gate_enable;
  if (use_global_module_gate) {
    CloseModuleLoadGate("CompleteLaunch.begin");
  }
  struct ModuleGateScope {
    Emulator* emulator = nullptr;
    bool enabled = false;
    ~ModuleGateScope() {
      if (enabled && emulator) {
        emulator->OpenModuleLoadGate("CompleteLaunch.exit");
      }
    }
  } module_gate_scope{this, use_global_module_gate};

  // Setup NullDevices for raw HDD partition accesses
  // Cache/STFC code baked into games tries reading/writing to these
  // By using a NullDevice that just returns success to all IO requests it
  // should allow games to believe cache/raw disk was accessed successfully

  // NOTE: this should probably be moved to xenia_main.cc, but right now we
  // need to register the \Device\Harddisk0\ NullDevice _after_ the
  // \Device\Harddisk0\Partition1 HostPathDevice, otherwise requests to
  // Partition1 will go to this. Registering during CompleteLaunch allows us
  // to make sure any HostPathDevices are ready beforehand. (see comment above
  // cache:\ device registration for more info about why)
  auto null_paths = {std::string("\\Partition0"), std::string("\\Cache0"),
                     std::string("\\Cache1")};
  auto null_device =
      std::make_unique<vfs::NullDevice>("\\Device\\Harddisk0", null_paths);
  if (null_device->Initialize()) {
    file_system_->RegisterDevice(std::move(null_device));
  }

  // Reset state.
  title_id_ = std::nullopt;
  title_name_ = "";
  title_version_ = "";
  // CompleteLaunch may be invoked while a previous title is still tearing down,
  // so clear any cached main thread before resolving launch readiness.
  main_thread_ = nullptr;
  display_window_->SetIcon(nullptr, 0);

  // Allow xam to request module loads.
  XELOGI("DEBUG: Getting XAM module...");
  auto xam = kernel_state()->GetKernelModule<kernel::xam::XamModule>("xam.xex");
  XELOGI("DEBUG: XAM module: {}", static_cast<void*>(xam.get()));

  XELOGI("DEBUG: Loading user module: '{}'", module_path);
  uint32_t max_load_attempts =
      std::max<uint32_t>(1, cvars::complete_launch_load_user_module_attempts);
  uint32_t load_retry_delay_ms =
      cvars::complete_launch_load_user_module_retry_delay_ms;
  bool wait_until_success =
      cvars::complete_launch_load_user_module_wait_until_success;
  uint64_t wait_timeout_ms = static_cast<uint64_t>(
      cvars::complete_launch_load_user_module_wait_timeout_ms);
  uint64_t heartbeat_interval_ms = static_cast<uint64_t>(
      cvars::complete_launch_load_user_module_heartbeat_interval_ms);
  std::string resolved_module_path;
  if (file_system_) {
    if (auto* resolved_entry = file_system_->ResolvePath(module_path)) {
      resolved_module_path = resolved_entry->absolute_path();
    }
  }
  uint64_t load_user_module_start_ms = Clock::QueryHostUptimeMillis();
  uint64_t last_gate_heartbeat_ms = 0;
  bool persistent_gate_logged = false;
  bool load_gate_timed_out = false;
  kernel::object_ref<kernel::UserModule> module;
  for (uint32_t attempt = 1;; ++attempt) {
    if (attempt > 1 && load_retry_delay_ms) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(load_retry_delay_ms));
    }
    const std::string attempt_path = resolved_module_path.empty()
                                         ? std::string(module_path)
                                         : resolved_module_path;
    bool in_persistent_wait = wait_until_success && attempt > max_load_attempts;
    if (in_persistent_wait && !persistent_gate_logged) {
      XELOGW(
          "DEBUG: CompleteLaunch module-load gate ACTIVE: halting launch "
          "progression until LoadUserModule succeeds (initial attempts={} "
          "retry_delay={}ms timeout={}ms)",
          max_load_attempts, load_retry_delay_ms, wait_timeout_ms);
      persistent_gate_logged = true;
    }
    XELOGI(
        "DEBUG: CompleteLaunch: LoadUserModule attempt {}{} path='{}' "
        "(resolved_fallback={})",
        attempt,
        in_persistent_wait
            ? fmt::format(" [persistent-wait after {} attempts]",
                          max_load_attempts)
            : fmt::format("/{}", max_load_attempts),
        attempt_path,
        resolved_module_path.empty() ? "NO" : "YES");
    uint64_t attempt_begin_ms = Clock::QueryHostUptimeMillis();
    auto load_attempt_done = std::make_shared<std::atomic<bool>>(false);
    if (wait_until_success) {
      uint64_t watchdog_interval_ms =
          std::max<uint64_t>(1, heartbeat_interval_ms);
      auto load_attempt_done_ref = load_attempt_done;
      std::thread([&, watchdog_interval_ms, load_attempt_done_ref]() {
        while (!load_attempt_done_ref->load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(watchdog_interval_ms));
          if (load_attempt_done_ref->load(std::memory_order_acquire)) {
            break;
          }
          uint64_t now_ms = Clock::QueryHostUptimeMillis();
          auto load_stats = kernel_state_->GetUserModuleLoadStats();
            auto executable_module = kernel_state_->GetExecutableModule();
            bool executable_ready = executable_module != nullptr &&
                        executable_module->entry_point() != 0 &&
                        !load_stats.load_inflight;
          uint64_t attempt_elapsed_ms =
              now_ms >= attempt_begin_ms ? now_ms - attempt_begin_ms : 0;
          uint64_t since_progress_ms =
              (load_stats.last_progress_ms != 0 &&
               now_ms >= load_stats.last_progress_ms)
                  ? (now_ms - load_stats.last_progress_ms)
                  : 0;
          XELOGW(
              "DEBUG: CompleteLaunch attempt watchdog: still waiting inside "
              "LoadUserModule (attempt={} elapsed={}ms load_inflight={} "
              "executable_ready={} load_calls={} load_success={} "
              "load_fail={} load_progress_count={} "
              "loaded_executable_user_modules={} "
              "since_last_progress={}ms)",
              attempt, attempt_elapsed_ms,
              load_stats.load_inflight ? "YES" : "NO",
              executable_ready ? "YES" : "NO",
              static_cast<unsigned long long>(load_stats.load_calls),
              static_cast<unsigned long long>(load_stats.load_success),
              static_cast<unsigned long long>(load_stats.load_fail),
              static_cast<unsigned long long>(load_stats.load_progress_count),
              static_cast<unsigned long long>(
                  load_stats.loaded_executable_user_modules),
              static_cast<unsigned long long>(since_progress_ms));
        }
      }).detach();
    }

    XELOGI("DEBUG: CompleteLaunch: invoking KernelState::LoadUserModule (attempt={})",
           attempt);
    module = kernel_state_->LoadUserModule(attempt_path);
    XELOGI(
        "DEBUG: CompleteLaunch: KernelState::LoadUserModule returned to caller "
        "(attempt={})",
        attempt);

    load_attempt_done->store(true, std::memory_order_release);

    uint64_t attempt_end_ms = Clock::QueryHostUptimeMillis();
    XELOGI(
        "DEBUG: CompleteLaunch: LoadUserModule attempt {} returned in {} ms "
        "result={}",
        attempt,
        static_cast<unsigned long long>(attempt_end_ms - attempt_begin_ms),
        module ? "SUCCESS" : "NULL");
    if (module) {
      auto load_stats = kernel_state_->GetUserModuleLoadStats();
      auto executable_module = kernel_state_->GetExecutableModule();
      bool executable_ready = executable_module != nullptr &&
                  executable_module->entry_point() != 0 &&
                  !load_stats.load_inflight;
      XELOGI(
          "DEBUG: CompleteLaunch: module-load success verification "
          "(load_inflight={} executable_ready={} load_success={} "
          "loaded_executable_user_modules={})",
          load_stats.load_inflight ? "YES" : "NO",
          executable_ready ? "YES" : "NO",
          static_cast<unsigned long long>(load_stats.load_success),
          static_cast<unsigned long long>(
              load_stats.loaded_executable_user_modules));
      break;
    }

    auto load_stats = kernel_state_->GetUserModuleLoadStats();
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    uint64_t elapsed_ms = now_ms >= load_user_module_start_ms
                              ? now_ms - load_user_module_start_ms
                              : 0;
    XELOGW(
        "DEBUG: CompleteLaunch: LoadUserModule attempt {} failed "
        "(load_inflight={} load_success={} load_fail={} "
        "loaded_user_modules={} loaded_executable_user_modules={} "
        "elapsed={}ms)",
        attempt, load_stats.load_inflight ? "YES" : "NO",
        static_cast<unsigned long long>(load_stats.load_success),
        static_cast<unsigned long long>(load_stats.load_fail),
        static_cast<unsigned long long>(load_stats.loaded_user_modules),
        static_cast<unsigned long long>(
            load_stats.loaded_executable_user_modules),
        static_cast<unsigned long long>(elapsed_ms));

    bool timeout_reached =
        wait_until_success && wait_timeout_ms > 0 && elapsed_ms >= wait_timeout_ms;
    if (wait_until_success) {
      bool log_heartbeat =
          heartbeat_interval_ms == 0 || last_gate_heartbeat_ms == 0 ||
          now_ms - last_gate_heartbeat_ms >= heartbeat_interval_ms;
      if (log_heartbeat) {
        last_gate_heartbeat_ms = now_ms;
        uint64_t since_progress_ms =
            (load_stats.last_progress_ms != 0 && now_ms >= load_stats.last_progress_ms)
                ? (now_ms - load_stats.last_progress_ms)
                : 0;
        auto executable_module = kernel_state_->GetExecutableModule();
        bool executable_ready = executable_module != nullptr &&
                                executable_module->entry_point() != 0 &&
                                !load_stats.load_inflight;
        XELOGW(
            "DEBUG: CompleteLaunch module-load gate heartbeat: waiting for "
          "LoadUserModule success (attempt={} elapsed={}ms "
          "load_inflight={} executable_ready={} "
            "load_calls={} load_success={} load_fail={} "
            "load_progress_count={} since_last_progress={}ms)",
          attempt, elapsed_ms, load_stats.load_inflight ? "YES" : "NO",
          executable_ready ? "YES" : "NO",
            static_cast<unsigned long long>(load_stats.load_calls),
            static_cast<unsigned long long>(load_stats.load_success),
            static_cast<unsigned long long>(load_stats.load_fail),
            static_cast<unsigned long long>(load_stats.load_progress_count),
            static_cast<unsigned long long>(since_progress_ms));
      }
      if (timeout_reached) {
        load_gate_timed_out = true;
        XELOGE(
            "DEBUG: CompleteLaunch module-load gate timed out after {} ms "
            "without LoadUserModule success",
            elapsed_ms);
        break;
      }
      continue;
    }

    if (attempt >= max_load_attempts) {
      break;
    }
  }
  uint64_t load_user_module_end_ms = Clock::QueryHostUptimeMillis();
  XELOGI("DEBUG: CompleteLaunch stage timing: LoadUserModule took {} ms",
         static_cast<unsigned long long>(load_user_module_end_ms -
                                         load_user_module_start_ms));
  if (!module) {
    if (load_gate_timed_out) {
      XELOGE(
          "DEBUG: FAILED: LoadUserModule did not succeed before module-load gate "
          "timeout ({} ms)",
          static_cast<unsigned long long>(wait_timeout_ms));
    }
    XELOGE("DEBUG: FAILED: LoadUserModule returned nullptr for '{}'", module_path);
    XELOGE(
        "DEBUG: This usually means the module file was not found in the virtual "
        "filesystem");
    XELOGE("DEBUG: Check that the ISO/disc image was mounted correctly");
    return X_STATUS_NOT_FOUND;
  }
  XELOGI("DEBUG: Module loaded successfully: {}", static_cast<void*>(module.get()));

  XELOGI("DEBUG: Checking if module is executable...");
  if (!module->is_executable()) {
    kernel_state_->UnloadUserModule(module, false);
    XELOGE("DEBUG: FAILED: Module is not executable");
    return X_STATUS_NOT_SUPPORTED;
  }
  XELOGI("DEBUG: Module is executable");

  XELOGI("DEBUG: CompleteLaunch: checking existing executable module...");
  uint64_t get_exec_start_ms = Clock::QueryHostUptimeMillis();
  auto existing_exec_module = kernel_state_->GetExecutableModule();
  uint64_t get_exec_end_ms = Clock::QueryHostUptimeMillis();
  XELOGI(
      "DEBUG: CompleteLaunch: GetExecutableModule returned {} in {} ms",
      existing_exec_module ? "NON-NULL" : "NULL",
      static_cast<unsigned long long>(get_exec_end_ms - get_exec_start_ms));

  if (!existing_exec_module) {
    XELOGI("DEBUG: CompleteLaunch: wiring executable module now...");
    kernel_state_->SetExecutableModule(module);
    XELOGI(
        "DEBUG: Executable module wired at load stage: module='{}' entry={:08X}",
        module->name(), module->entry_point());
  }

  XELOGI("DEBUG: Applying title update...");
  uint64_t apply_tu_begin_ms = Clock::QueryHostUptimeMillis();
  auto apply_tu_done = std::make_shared<std::atomic<bool>>(false);
  if (wait_until_success) {
    uint64_t watchdog_interval_ms =
        std::max<uint64_t>(1, heartbeat_interval_ms);
    auto apply_tu_done_ref = apply_tu_done;
    std::thread([&, watchdog_interval_ms, apply_tu_done_ref]() {
      while (!apply_tu_done_ref->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(watchdog_interval_ms));
        if (apply_tu_done_ref->load(std::memory_order_acquire)) {
          break;
        }
        uint64_t now_ms = Clock::QueryHostUptimeMillis();
        uint64_t elapsed_ms = now_ms >= apply_tu_begin_ms
                                  ? now_ms - apply_tu_begin_ms
                                  : 0;
        XELOGW(
            "DEBUG: CompleteLaunch stage heartbeat: waiting in ApplyTitleUpdate "
            "(elapsed={}ms)",
            elapsed_ms);
      }
    }).detach();
  }
  X_RESULT result = kernel_state_->ApplyTitleUpdate(module);
  apply_tu_done->store(true, std::memory_order_release);
  if (XFAILED(result)) {
    XELOGE("DEBUG: FAILED: ApplyTitleUpdate returned {:08X}", result);
    return result;
  }
  XELOGI("DEBUG: Title update applied successfully");

  XELOGI("DEBUG: Finishing loading user module...");
  uint64_t finish_module_start_ms = Clock::QueryHostUptimeMillis();
  auto finish_module_done = std::make_shared<std::atomic<bool>>(false);
  if (wait_until_success) {
    uint64_t watchdog_interval_ms =
        std::max<uint64_t>(1, heartbeat_interval_ms);
    auto finish_module_done_ref = finish_module_done;
    std::thread([&, watchdog_interval_ms, finish_module_done_ref]() {
      while (!finish_module_done_ref->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(watchdog_interval_ms));
        if (finish_module_done_ref->load(std::memory_order_acquire)) {
          break;
        }
        uint64_t now_ms = Clock::QueryHostUptimeMillis();
        uint64_t elapsed_ms = now_ms >= finish_module_start_ms
                                  ? now_ms - finish_module_start_ms
                                  : 0;
        auto load_stats = kernel_state_->GetUserModuleLoadStats();
        auto executable_module = kernel_state_->GetExecutableModule();
        uint32_t executable_entry =
            executable_module ? executable_module->entry_point() : 0;
        XELOGW(
            "DEBUG: CompleteLaunch stage heartbeat: waiting in "
            "FinishLoadingUserModule (elapsed={}ms load_inflight={} "
            "loaded_exec_modules={} executable_entry={:08X} "
            "load_progress_count={})",
            elapsed_ms, load_stats.load_inflight ? "YES" : "NO",
            static_cast<unsigned long long>(
                load_stats.loaded_executable_user_modules),
            executable_entry,
            static_cast<unsigned long long>(load_stats.load_progress_count));
      }
    }).detach();
  }
  result = kernel_state_->FinishLoadingUserModule(module);
  finish_module_done->store(true, std::memory_order_release);
  uint64_t finish_module_end_ms = Clock::QueryHostUptimeMillis();
  XELOGI("DEBUG: CompleteLaunch stage timing: FinishLoadingUserModule took {} ms",
         static_cast<unsigned long long>(finish_module_end_ms -
                                         finish_module_start_ms));
  if (XFAILED(result)) {
    XELOGE("DEBUG: FAILED: FinishLoadingUserModule returned {:08X}", result);
    return result;
  }
  XELOGI("DEBUG: User module finished loading successfully");

  if (use_global_module_gate) {
    auto load_stats = kernel_state_->GetUserModuleLoadStats();
    auto executable_module = kernel_state_->GetExecutableModule();
    bool fully_ready = executable_module != nullptr &&
                       executable_module->entry_point() != 0 &&
                       !load_stats.load_inflight;
    XELOGI(
        "DEBUG: CompleteLaunch full-readiness verification "
        "(load_inflight={} executable_present={} executable_entry={:08X} "
        "fully_ready={})",
        load_stats.load_inflight ? "YES" : "NO",
        executable_module ? "YES" : "NO",
        executable_module ? executable_module->entry_point() : 0,
        fully_ready ? "YES" : "NO");
    if (!fully_ready) {
      XELOGE(
          "DEBUG: CompleteLaunch blocked: module not fully ready after "
          "FinishLoadingUserModule (entry unresolved or load still inflight)");
      return X_STATUS_UNSUCCESSFUL;
    }
    XELOGI(
        "DEBUG: CompleteLaunch: module fully ready; keeping module-load gate "
        "closed until guest main thread is launched");
  }

  if (!kernel_state_->GetExecutableModule()) {
    kernel_state_->SetExecutableModule(module);
    XELOGI(
        "DEBUG: Executable module wired early during CompleteLaunch: module='{}' "
        "entry={:08X}",
        module->name(), module->entry_point());
  }

  // Grab the current title ID.
  xex2_opt_execution_info* info = nullptr;
  uint32_t workspace_address = 0;
  module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &info);

  kernel_state_->memory()
      ->LookupHeapByType(false, 0x1000)
      ->Alloc(module->workspace_size(), 0x1000,
              kMemoryAllocationReserve | kMemoryAllocationCommit,
              kMemoryProtectRead | kMemoryProtectWrite, false,
              &workspace_address);

  if (!info) {
    title_id_ = 0;
  } else {
    title_id_ = info->title_id;
    auto title_version = info->version();
    if (title_version.value != 0) {
      title_version_ = format_version(title_version);
    }
  }

  // Try and load the resource database (xex only).
  if (module->title_id()) {
    auto title_id = fmt::format("{:08X}", module->title_id());

    // Load the per-game configuration file and make sure updates are handled
    // by the callbacks.
    config::LoadGameConfig(title_id);
    assert_true(game_config_load_callback_loop_next_index_ == SIZE_MAX);
    game_config_load_callback_loop_next_index_ = 0;
    while (game_config_load_callback_loop_next_index_ <
           game_config_load_callbacks_.size()) {
      game_config_load_callbacks_[game_config_load_callback_loop_next_index_++]
          ->PostGameConfigLoad();
    }
    game_config_load_callback_loop_next_index_ = SIZE_MAX;

    const auto db = kernel_state_->module_xdbf(module);

    game_info_database_ =
        std::make_unique<kernel::util::GameInfoDatabase>(db.get());
    kernel_state_->xam_state()->LoadSpaInfo(db.get());

    kernel_state_->xam_state()->user_tracker()->AddTitleToPlayedList();

    if (game_info_database_->IsValid()) {
      title_name_ = game_info_database_->GetTitleName(
          static_cast<XLanguage>(cvars::user_language));
      XELOGI("Title name: {}", title_name_);

      if (cvars::log_game_info_database_tables) {
        // Show achievement and matchmaking metadata tables only when explicitly
        // requested, as this can become very expensive in heavily instrumented
        // startup runs.
        tabulate::Table table;
        table.format().multi_byte_characters(true);
        table.add_row({"ID", "Title", "Description", "Type", "Gamerscore"});

        const std::vector<kernel::util::GameInfoDatabase::Achievement>
            achievement_list = game_info_database_->GetAchievements();
        for (const kernel::util::GameInfoDatabase::Achievement& entry :
             achievement_list) {
          const std::string type = GetAchievementTypeName(
              kernel::xam::GetAchievementType(entry.flags));

          table.add_row({fmt::format("{}", entry.id), entry.label,
                         entry.description, type,
                         fmt::format("{}", entry.gamerscore)});
        }
        XELOGI("\n-------------------- ACHIEVEMENTS --------------------\n{}",
               table.str());

        const std::vector<kernel::util::GameInfoDatabase::Property>
            properties_list = game_info_database_->GetProperties();

        table = tabulate::Table();
        table.format().multi_byte_characters(true);
        table.add_row({"ID", "Name", "Matchmaking", "Data Size"});

        for (const kernel::util::GameInfoDatabase::Property& entry :
             properties_list) {
          std::string label =
              string_util::remove_eol(string_util::trim(entry.description));

          table.add_row({fmt::format("{:08X}", entry.id), label,
                         entry.is_matchmaking ? "True" : "False",
                         fmt::format("{}", entry.data_size)});
        }
        XELOGI("\n-------------------- PROPERTIES --------------------\n{}",
               table.str());

        const std::vector<kernel::util::GameInfoDatabase::Context>
            contexts_list = game_info_database_->GetContexts();

        table = tabulate::Table();
        table.format().multi_byte_characters(true);
        table.add_row(
            {"ID", "Name", "Matchmaking", "Default Value", "Max Value"});

        for (const kernel::util::GameInfoDatabase::Context& entry :
             contexts_list) {
          std::string label =
              string_util::remove_eol(string_util::trim(entry.description));

          table.add_row({fmt::format("{:08X}", entry.id), label,
                         entry.is_matchmaking ? "True" : "False",
                         fmt::format("{}", entry.default_value),
                         fmt::format("{}", entry.max_value)});
        }
        XELOGI("\n-------------------- CONTEXTS --------------------\n{}",
               table.str());

        const std::vector<kernel::util::GameInfoDatabase::StatsView> stats_views =
            game_info_database_->GetStatsViews();

        // 4D5307EA SPA contains a lot of stats, limit views to log.
        const auto stats_views_limit = stats_views | std::views::take(100);

        table = tabulate::Table();
        table.format().multi_byte_characters(true);
        table.add_row({"ID", "View Type", "Name", "Skilled", "Arbitrated",
                       "Hidden", "Team View", "Online Only"});

        for (const kernel::util::GameInfoDatabase::StatsView& entry :
             stats_views_limit) {
          const std::string name =
              string_util::remove_eol(string_util::trim(entry.view.name));

          const std::string view_type =
              kernel::xam::GetViewTypeName(entry.view.view_type);

          table.add_row({fmt::format("{:08X}", entry.view.id), view_type, name,
                         entry.view.skilled ? "True" : "False",
                         entry.view.arbitrated ? "True" : "False",
                         entry.view.hidden ? "True" : "False",
                         entry.view.team_view ? "True" : "False",
                         entry.view.online_only ? "True" : "False"});
        }

        std::string totals;

        if (stats_views.size() > stats_views_limit.size()) {
          totals = fmt::format("\nViews: {}/{}", stats_views_limit.size(),
                               stats_views.size());
        }
        XELOGI("\n-------------------- Stats Views --------------------{}\n{}",
               totals.c_str(), table.str());

        const std::vector<kernel::util::GameInfoDatabase::PresenceMode>
            presence_modes = game_info_database_->GetPresenceModes();

        table = tabulate::Table();
        table.format().multi_byte_characters(true);
        table.add_row({"Context Value", "Contexts Count", "Properties Count"});

        for (const kernel::util::GameInfoDatabase::PresenceMode& entry :
             presence_modes) {
          table.add_row(
              {fmt::format("{}", entry.context_value),
               fmt::format("{}", entry.property_bag.contexts.size()),
               fmt::format("{}", entry.property_bag.properties.size())});
        }
        XELOGI("\n-------------------- PRESENCE MODES --------------------\n{}",
               table.str());
      }

      auto icon_block = game_info_database_->GetIcon();
      if (!icon_block.empty()) {
        display_window_->SetIcon(icon_block.data(), icon_block.size());
      }
    }
  }

  // Initializing the shader storage in a blocking way so the user doesn't
  // miss the initial seconds - for instance, sound from an intro video may
  // start playing before the video can be seen if doing this in parallel with
  // the main thread.
  XELOGI("DEBUG: Initializing shader storage...");
  uint64_t shader_storage_start_ms = Clock::QueryHostUptimeMillis();
  bool shader_storage_blocking = cvars::complete_launch_blocking_shader_storage;
  on_shader_storage_initialization(true);
  graphics_system_->InitializeShaderStorage(cache_root_, title_id_.value(),
                                            shader_storage_blocking);
  on_shader_storage_initialization(false);
  uint64_t shader_storage_end_ms = Clock::QueryHostUptimeMillis();
  XELOGI("DEBUG: Shader storage init request completed in {} ms (blocking={}). "
         "Launching module...",
         static_cast<unsigned long long>(shader_storage_end_ms -
                                         shader_storage_start_ms),
         shader_storage_blocking ? "YES" : "NO");

  printf("DEBUG: About to call LaunchModule\n");
  XELOGI("DEBUG: Calling LaunchModule...");
  auto main_thread = kernel_state_->LaunchModule(module);
  printf("DEBUG: LaunchModule call completed\n");
  if (!main_thread) {
    printf("DEBUG: LaunchModule returned nullptr\n");
    XELOGE("DEBUG: FAILED: LaunchModule returned nullptr");
    XELOGE("DEBUG: This is a critical failure - module could not be launched");
    return X_STATUS_UNSUCCESSFUL;
  }
  XELOGI("DEBUG: LaunchModule succeeded, main thread: {}",
         static_cast<void*>(main_thread.get()));
  main_thread_ = main_thread;
  if (!HasGuestMainThread()) {
    XELOGE("DEBUG: LaunchModule returned a non-guest or non-main thread");
    return X_STATUS_UNSUCCESSFUL;
  }
  XELOGI("DEBUG: Guest main thread ready: handle={:08X} thread_id={} running={}",
         main_thread_->handle(), main_thread_->thread_id(),
         main_thread_->is_running() ? "YES" : "NO");
  if (use_global_module_gate) {
    EnsureGuestMainThreadRunning("CompleteLaunch.post_launch_gate_open");
    OpenModuleLoadGate("CompleteLaunch.guest_main_ready");
    module_gate_scope.enabled = false;
  }

  XELOGI("DEBUG: Calling on_launch callback...");
  on_launch(title_id_.value(), title_name_);
  XELOGI("DEBUG: on_launch callback completed");

  // Plugins must be loaded after calling LaunchModule() and
  // FinishLoadingUserModule() which will apply TUs and patching to the main
  // xex.
  if (cvars::allow_plugins) {
    XELOGI("DEBUG: Checking for plugins...");
    if (plugin_loader_->IsAnyPluginForTitleAvailable(title_id_.value(),
                                                     module->hash().value())) {
      XELOGI("DEBUG: Loading title plugins...");
      plugin_loader_->LoadTitlePlugins(title_id_.value(),
                                       module->hash().value());
      XELOGI("DEBUG: Plugins loaded");
    } else {
      XELOGI("No plugins available for this title");
    }
  }

  XELOGI("DEBUG: CompleteLaunch SUCCEEDED - Returning X_STATUS_SUCCESS");
  return X_STATUS_SUCCESS;
}

}  // namespace xe
