/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/graphics_system_mac.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <sstream>
#include <string_view>
#include <thread>

#include "xenia/base/byte_order_mac.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math_mac.h"
#include "xenia/base/memory_mac.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading_mac.h"
#include "xenia/base/utf8.h"
#include "xenia/config.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context_mac.h"
#include "xenia/cpu/ppc/ppc_opcode_info_mac.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator_mac.h"
#include "xenia/gpu/command_processor_mac.h"
#include "xenia/gpu/gpu_flags_mac.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/kernel/util/object_table_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading_mac.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/window_mac.h"
#include "xenia/ui/windowed_app_context_mac.h"

DECLARE_bool(track_guest_pc);
DECLARE_bool(log_gpu_import_thunks);

DEFINE_uint32(internal_display_resolution, 8,
              "Allow games that support different resolutions to render "
              "in a specific resolution.\n"
              "This is not guaranteed to work with all games or improve "
              "performance.\n"
              "   0=640x480\n"
              "   1=640x576\n"
              "   2=720x480\n"
              "   3=720x576\n"
              "   4=800x600\n"
              "   5=848x480\n"
              "   6=1024x768\n"
              "   7=1152x864\n"
              "   8=1280x720 (Default)\n"
              "   9=1280x768\n"
              "   10=1280x960\n"
              "   11=1280x1024\n"
              "   12=1360x768\n"
              "   13=1440x900\n"
              "   14=1680x1050\n"
              "   15=1920x540\n"
              "   16=1920x1080\n"
              "   17=internal_display_resolution_x/y",
              "Video");
DEFINE_uint32(internal_display_resolution_x, 1280,
              "Custom width. See internal_display_resolution. Range 1-1920.",
              "Video");
DEFINE_bool(gpu_log_mmio_writes_cp_rb, false,
            "Debug: Log MMIO writes to CP_RB_* registers with guest PC.",
            "GPU");
DEFINE_bool(gpu_log_mmio_writes_fetch, false,
            "Debug: Log MMIO writes to shader fetch constants (index 0).",
            "GPU");
DEFINE_bool(gpu_log_mmio_reads_cp_rb, false,
            "Debug: Log MMIO reads of CP_RB_* registers with guest PC.", "GPU");
DEFINE_bool(
    gpu_mmio_cp_endian_autofix, true,
    "Debug: If a CP_RB_* MMIO write value is implausible but its byte-swapped "
    "form is plausible, use the swapped value for handling/logging.",
    "GPU");
DEFINE_bool(
    gpu_mmio_cp_endian_log, true,
    "Debug: Log CP_RB_* MMIO raw vs byte-swapped plausibility checks.",
    "GPU");
DEFINE_bool(
    gpu_log_no_swap_dump_mmio_state, false,
    "Debug: Dump CP_RB_* MMIO write state when no swap packets are seen.",
    "GPU");
DEFINE_bool(gpu_log_no_swap_dump_vd_state, false,
            "Debug: Dump Vd* call state when no swap packets are seen.", "GPU");
DEFINE_bool(
    gpu_log_no_swap_dump_ring_watch, false,
    "Debug: Dump ring buffer write watch state when no swap packets are seen.",
    "GPU");
DEFINE_bool(gpu_log_no_swap_dump_threads, false,
            "Debug: Dump guest thread PC states when no swap packets are seen.",
            "GPU");
DEFINE_bool(gpu_debug_gate_watch, false,
            "Debug: Log the main-loop gate value at 0x821F4898 every vblank.",
            "GPU");
DEFINE_bool(
  gpu_autofix_debug_monitor_import_slot, false,
  "Debug: If GPU bootstrap never enters and import slot 0x820005E0 is 0, "
  "repair it from xboxkrnl KeDebugMonitorData export mapping.",
  "GPU");
DEFINE_uint32(
    gpu_debug_gate_watch_every_n, 1,
    "Debug: Log the main-loop gate value every N vblanks (0 = every).", "GPU");
DEFINE_bool(gpu_debug_gate_watch_log_on_change_only, false,
            "Debug: Only log the main-loop gate when it changes.", "GPU");
DEFINE_bool(
    gpu_suspend_nonessential_until_ring_init_ack, true,
    "Debug safety gate: while ring buffer init handshake is incomplete "
    "(ring_buffer_initialized && init_ack), suspend non-essential no-swap "
    "diagnostics and compatibility fallback paths.",
    "GPU");
DEFINE_uint32(
    gpu_suspend_nonessential_bootstrap_log_every_n_vblank, 120,
    "Debug safety gate: log waiting-for-ring-init state every N vblanks "
    "(0 = every).",
    "GPU");
DEFINE_bool(
    gpu_suspend_force_fallback_until_module_ready, true,
    "Safety gate: defer host force/fallback bootstrap actions until "
    "LoadUserModule has progressed and an executable module is ready.",
    "GPU");
DEFINE_uint32(
    gpu_suspend_force_fallback_log_interval_ms, 1000,
    "Minimum interval for logs emitted while force/fallback actions are "
    "deferred by the module-ready safety gate (0 = every check).",
    "GPU");
DEFINE_bool(
    gpu_no_force_run_profile, false,
    "Run profile: keep early startup diagnostics and readiness checks, but "
    "disable host force/fallback bootstrap, callback injection, module-load "
    "nudges, and guest-main launch nudges.",
    "GPU");
DEFINE_uint32(
    gpu_no_force_run_profile_log_interval_ms, 1000,
    "Minimum interval for logs emitted when force/fallback paths are "
    "suppressed by gpu_no_force_run_profile (0 = every check).",
    "GPU");
DEFINE_bool(
    gpu_no_force_allow_callback_recovery_on_startup_gate, false,
    "When gpu_no_force_run_profile is enabled, allow auto callback recovery "
    "once callback startup gate prerequisites are satisfied "
    "(guest-main running, module ready, ring ready, bootstrap/runtime activity "
    "observed).",
    "GPU");
DEFINE_bool(
    gpu_no_force_callback_startup_prereq_hard_gate, true,
    "When gpu_no_force_run_profile is enabled, block callback recovery "
    "progression until startup_bridge_ready, callback_startup_gate_ready, and "
    "bootstrap_activity are all satisfied.",
    "GPU");
DEFINE_bool(
    gpu_vblank_callback_startup_gate, true,
    "Enable a vblank-stage callback startup gate diagnostic that reports why "
    "interrupt callback recovery remains blocked.",
    "GPU");
DEFINE_uint32(
    gpu_vblank_callback_startup_gate_log_interval_ms, 1000,
    "Minimum interval in milliseconds between callback startup gate wait/ready "
    "logs while callback remains unset (0 = every check).",
    "GPU");
DEFINE_bool(
    gpu_callback_missing_hard_gate_fallback, false,
    "When callback startup is stuck in all-<never> bootstrap/runtime state, "
    "run mandatory fallback initialization/recovery attempts before allowing "
    "GPU startup progress.",
    "GPU");
DEFINE_uint32(
    gpu_callback_missing_hard_gate_after_vblank, 2,
    "Start callback hard-gate fallback attempts after this many vblanks with "
    "callback still missing (0 = immediate).",
    "GPU");
DEFINE_uint32(
    gpu_callback_missing_hard_gate_interval_ms, 1000,
    "Minimum interval between callback hard-gate fallback attempts while "
    "callback remains missing.",
    "GPU");
DEFINE_uint32(
    gpu_debug_interrupt_missing_dump_after_vblank, 5,
    "Debug: After this many vblanks with no GPU interrupt callback set, dump "
    "no-swap state for early diagnosis (0 disables).",
    "GPU");
DEFINE_uint32(
    gpu_debug_interrupt_missing_dump_interval_ms, 5000,
    "Debug: Minimum interval between missing-interrupt callback dumps.", "GPU");
DEFINE_bool(
    gpu_debug_force_interrupt_callback_if_missing, false,
    "Debug: If the GPU interrupt callback is never set, force delivery of "
    "vblank interrupts via a stub callback for experimentation.",
    "GPU");
DEFINE_bool(
    gpu_defer_interrupt_dispatch_during_module_load, true,
    "Safety: Defer GPU interrupt callback dispatch while LoadUserModule is "
    "inflight and no executable user module is loaded yet. Prevents prelaunch "
    "dispatch races against module decode/load.",
    "GPU");
DEFINE_uint32(
    gpu_defer_interrupt_dispatch_keepalive_every_n, 0,
    "While module-load interrupt deferral is active, allow every Nth interrupt "
    "dispatch through as a keepalive (0 disables count-based keepalive).",
    "GPU");
DEFINE_uint32(
    gpu_defer_interrupt_dispatch_max_silence_ms, 0,
    "While module-load interrupt deferral is active, force a keepalive "
    "interrupt dispatch if no dispatch has happened for this many "
    "milliseconds (0 disables silence-based keepalive).",
    "GPU");
#if XE_PLATFORM_MAC
DEFINE_bool(
    gpu_allow_forced_interrupt_callback_fallback, false,
    "Compatibility-only: allow host-generated fallback GPU interrupt callback "
    "injection. Disabled by default to avoid masking missing guest callback "
    "bootstrap.",
    "GPU");
#else
DEFINE_bool(
    gpu_allow_forced_interrupt_callback_fallback, false,
    "Compatibility-only: allow host-generated fallback GPU interrupt callback "
    "injection. Disabled by default to avoid masking missing guest bootstrap.",
    "GPU");
#endif
DEFINE_uint32(
    gpu_debug_force_interrupt_callback_after_vblank, 5,
    "Debug: Start forcing GPU interrupt callbacks after this many vblanks "
    "without a registered callback.",
    "GPU");
DEFINE_uint32(gpu_debug_force_interrupt_callback_interval_ms, 1000,
              "Debug: Minimum interval between forced GPU interrupt callbacks.",
              "GPU");
DEFINE_bool(
    gpu_force_init_if_missing, false,
    "Force GPU init (ring buffer + interrupt callback) if Vd* calls "
    "are missing. Disabled by default so real guest bootstrap issues are "
    "visible; enable only for compatibility experiments.",
    "GPU");
DEFINE_bool(gpu_compat_proactive_bootstrap, false,
            "Debug compatibility mode: proactively initialize GPU bootstrap "
            "(ring buffer/callback nudges) before guest-driven Vd*/MMIO setup.",
            "GPU");
      DEFINE_bool(
        gpu_force_ring_init_ack_on_startup,
        false,
        "Force ring buffer initialization on startup when missing and wait briefly "
        "for worker acknowledgment to avoid bootstrap gate deadlock.",
        "GPU");
// Diagnostic: Warn if guest never writes CP_RB_BASE/CP_RB_CNTL/CP_RB_WPTR after timeout

DEFINE_bool(gpu_dispatch_initial_interrupt_on_set, false,
            "Debug compatibility mode: dispatch one immediate interrupt when "
            "VdSetGraphicsInterruptCallback registers a callback.",
            "GPU");
DEFINE_uint32(gpu_force_init_after_vblank, 120,
              "Force GPU init after this many vblanks (~2 s at 60 Hz) if Vd* "
              "calls are missing.",
              "GPU");
DEFINE_uint32(
    gpu_force_init_ring_buffer_size_log2, 18,
    "Debug: Ring buffer size_log2 for forced GPU init (bytes = 2^(log2+3)).",
    "GPU");
DEFINE_bool(gpu_debug_sync_profile, false,
            "Debug: Log GPU/CPU sync state at regular intervals during vblank.",
            "GPU");
DEFINE_uint32(gpu_debug_sync_profile_interval_ms, 2000,
              "Debug: Minimum interval between GPU/CPU sync profile logs.",
              "GPU");
DEFINE_bool(gpu_debug_sync_profile_dump_threads, false,
            "Debug: Include guest thread dump in GPU/CPU sync profile logs.",
            "GPU");
DEFINE_uint32(
    gpu_log_no_swap_threads_interval_ms, 10000,
    "Debug: Minimum interval between guest thread dumps when no swap packets "
    "are seen.",
    "GPU");
DEFINE_uint32(gpu_log_no_swap_threads_max, 64,
              "Debug: Maximum number of guest threads to dump per no-swap "
              "thread dump.",
              "GPU");
DEFINE_uint32(internal_display_resolution_y, 720,
              "Custom height. See internal_display_resolution. Range 1-1080.\n",
              "Video");

DEFINE_bool(
    store_shaders, true,
    "Store shaders persistently and load them when loading games to avoid "
    "runtime spikes and freezes when playing the game not for the first time.",
    "GPU");
DEFINE_bool(gpu_log_interrupts, false,
            "Debug: Log GPU interrupt callback registration and dispatches.",
            "GPU");
DEFINE_bool(gpu_log_register_reads, false, "Debug: Log all GPU register reads.",
            "GPU");
DEFINE_int32(
    gpu_log_interrupts_every_n, 1,
    "Debug: Log every N GPU interrupt callbacks when logging is enabled.",
    "GPU");
DEFINE_bool(gpu_log_vblank, false,
            "Debug: Log vblank events and interrupt dispatches.", "GPU");
DEFINE_int32(gpu_log_vblank_every_n, 1,
             "Debug: Log every N vblank events when logging is enabled.",
             "GPU");
DEFINE_bool(
    gpu_log_vblank_only_on_activity, false,
    "Debug: When enabled, suppress vblank logs while the ring buffer is idle.",
    "GPU");
DEFINE_uint32(
    gpu_log_vblank_idle_suppress_ms, 2000,
    "Debug: Minimum interval (ms) between vblank logs while idle when "
    "gpu_log_vblank_only_on_activity is enabled.",
    "GPU");
DEFINE_bool(
    gpu_log_interrupts_require_callback, false,
    "Debug: Suppress interrupt dispatch logs when no callback is registered.",
    "GPU");
DEFINE_uint32(
    gpu_log_no_swap_after_ms, 5000,
    "Debug: Warn if no swap packets are observed after this many milliseconds "
    "since the first vblank.",
    "GPU");
DEFINE_uint32(gpu_log_no_swap_interval_ms, 5000,
              "Debug: Minimum interval (ms) between no-swap warnings.", "GPU");
DEFINE_bool(
  gpu_startup_watch, true,
  "Debug: Emit periodic GPU startup progress logs before first guest-driven "
  "swap lifecycle is established.",
  "GPU");
DEFINE_uint32(
  gpu_startup_watch_interval_ms, 1000,
  "Debug: Minimum interval between GPU startup watch logs.", "GPU");
DEFINE_uint32(
  gpu_startup_starvation_timeout_ms, 5000,
  "Debug: Emit one-shot startup starvation diagnostics after this many "
  "milliseconds without bootstrap readiness.",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_bootstrap_nudge, false,
  "Debug: If guest main thread is still not launched and there is no Vd/MMIO "
  "bootstrap activity, issue a one-time GPU bootstrap nudge (fallback init + "
  "host-side MMIO bootstrap injection).",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_bootstrap_nudge_after_ms, 1500,
  "Debug: Minimum prelaunch age before issuing the one-time prelaunch GPU "
  "bootstrap nudge.",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_bootstrap_nudge_in_setup, false,
  "Debug: Issue an immediate one-time GPU bootstrap nudge at the end of "
  "GraphicsSystem::Setup (before prelaunch vblank diagnostics), to avoid "
  "waiting for prelaunch watchdog timing.",
  "GPU");
DEFINE_bool(
  gpu_bootstrap_probe_prime_ring_if_missing, false,
  "Debug: When bootstrap probes are running and ring init is still missing, "
  "allocate/initialize a startup ring buffer so no-force MMIO bootstrap "
  "injection can run before worker loop progress checks.",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_module_nudge, false,
  "Debug: If executable module is unset in prelaunch, attempt to wire it "
  "from the first loaded executable user module before startup-watch "
  "snapshotting.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_module_nudge_after_ms, 0,
  "Debug: Minimum prelaunch age before attempting executable module wiring "
  "nudge (0 = immediate).",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_force_module_load_by_path, false,
  "Debug: If no executable user module is visible in prelaunch and no "
  "LoadUserModule call is currently inflight, queue a forced "
  "KernelState::LoadUserModule using gpu_prelaunch_force_module_load_path.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_force_module_load_after_ms, 250,
  "Debug: Minimum prelaunch age before attempting forced LoadUserModule "
  "fallback by path.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_force_module_load_retry_interval_ms, 1000,
  "Debug: Minimum interval between forced prelaunch LoadUserModule path "
  "fallback attempts.",
  "GPU");
DEFINE_string(
  gpu_prelaunch_force_module_load_path, "game:\\default.xex",
  "Debug: Guest path used by forced prelaunch LoadUserModule fallback "
  "(for example game:\\default.xex).",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_force_module_load_require_load_call, true,
  "Debug: Require at least one observed KernelState::LoadUserModule call "
  "before prelaunch force-load nudges can queue their own LoadUserModule "
  "fallback attempt.",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_guest_main_launch_nudge, false,
  "Debug: If guest main thread is still not launched during prelaunch and "
  "an executable module is present, attempt KernelState::LaunchModule "
  "nudges until one succeeds.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_guest_main_launch_nudge_after_ms, 0,
  "Debug: Minimum prelaunch age before attempting guest main launch "
  "nudge.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_guest_main_launch_nudge_retry_interval_ms, 1000,
  "Debug: Minimum interval between prelaunch guest-main launch retries while "
  "launch still fails.",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_guest_main_resume_nudge, false,
  "Debug: If guest main thread exists but is not running, attempt throttled "
  "Resume() nudges during startup.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_guest_main_resume_nudge_after_ms, 150,
  "Debug: Minimum startup age before first guest-main Resume() nudge.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_guest_main_resume_nudge_interval_ms, 1000,
  "Debug: Minimum interval between guest-main Resume() nudges while still "
  "not running.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_guest_main_resume_nudge_max_attempts, 12,
  "Debug: Maximum number of guest-main Resume() nudges (0 = unlimited).",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_module_load_stall_after_ms, 5000,
  "Debug: If LoadUserModule is inflight and no module-load heartbeat progress "
  "is observed for this many milliseconds, emit a suspected-stall warning.",
  "GPU");
DEFINE_bool(
  gpu_prelaunch_suspend_during_module_load, true,
  "Safety: While guest main is not ready and LoadUserModule is inflight with "
  "no executable module visible yet, suspend non-essential prelaunch GPU "
  "watchdogs/diagnostics/nudges until load completes or a stall is detected.",
  "GPU");
DEFINE_uint32(
  gpu_prelaunch_suspend_log_interval_ms, 1000,
  "Minimum interval between logs emitted by the prelaunch module-load "
  "suspend gate (0 = every check).",
  "GPU");
DEFINE_bool(
  gpu_vblank_suspend_until_module_load_success, true,
  "Strict startup gate: suspend MarkVblank prelaunch processing until an "
  "executable user module is visible / LoadUserModule success is observed.",
  "GPU");
DEFINE_uint32(
  gpu_vblank_suspend_module_gate_log_interval_ms, 1000,
  "Heartbeat interval in milliseconds while strict MarkVblank module-load "
  "gate is active.",
  "GPU");
DEFINE_bool(
  gpu_auto_recover_interrupt_callback_missing, false,
  "Debug: If ring init/ack is ready but interrupt callback is still unset, "
  "auto-install a host-defined interrupt callback to recover guest "
  "interrupt lifecycle after startup starvation.",
  "GPU");
DEFINE_uint32(
  gpu_auto_recover_interrupt_callback_after_ms, 500,
  "Debug: Minimum startup age before attempting automatic missing-callback "
  "recovery.",
  "GPU");
DEFINE_bool(
  gpu_auto_recover_interrupt_callback_once, true,
  "Debug: If true, perform automatic missing-callback recovery only once.",
  "GPU");
DEFINE_bool(gpu_log_no_swap_dump_fetch, true,
            "Debug: Dump texture fetch 0 when no swap packets are observed.",
            "GPU");
DEFINE_bool(
    gpu_log_no_swap_dump_ring, true,
    "Debug: Dump a ring buffer window when no swap packets are observed.",
    "GPU");
DEFINE_uint32(gpu_log_no_swap_dump_ring_dwords, 64,
              "Debug: Number of ring buffer dwords to dump when no swap "
              "packets are observed.",
              "GPU");
DEFINE_uint32(
    gpu_debug_force_swap_after_ms, 0,
    "Debug: If >0, schedule a host-side swap injection after this many "
    "milliseconds with no swap packets.",
    "GPU");
DEFINE_uint32(gpu_debug_force_swap_interval_ms, 2000,
              "Debug: Minimum interval (ms) between host-side swap injections.",
              "GPU");
DEFINE_bool(gpu_debug_force_swap_once, false,
            "Debug: If true, only perform a single host-side swap injection.",
            "GPU");
DEFINE_uint32(
  gpu_debug_dead_pipeline_recover_after_ms, 0,
  "Debug: If >0 and guest thread is running but Vd/swap/refresh activity "
  "stays fully zero for this long, run emergency recovery (force ring init "
  "+ host-side swap injection) to unblock guest-output mailbox diagnostics.",
  "GPU");
DEFINE_bool(
  gpu_debug_dead_pipeline_recover_once, true,
  "Debug: If true, run dead-pipeline emergency recovery only once.",
  "GPU");
DEFINE_bool(
  gpu_debug_guest_bootstrap_inject_if_stalled, false,
  "Debug: When ring is initialized but only host debug swaps are observed, "
  "inject guest-style MMIO ring bootstrap writes and synthetic Vd markers "
  "to validate downstream polling/readback behavior.",
  "GPU");
DEFINE_uint32(
  gpu_debug_guest_bootstrap_inject_after_ms, 2500,
  "Debug: Earliest no-swap age (ms) before guest-style bootstrap/MMIO "
  "injection may run.",
  "GPU");
DEFINE_uint32(
  gpu_debug_guest_bootstrap_inject_interval_ms, 1000,
  "Debug: Minimum interval (ms) between guest-style bootstrap injections.",
  "GPU");
DEFINE_bool(
  gpu_debug_guest_bootstrap_inject_once, true,
  "Debug: If true, perform guest-style bootstrap injection once per run.",
  "GPU");
DEFINE_uint32(
  gpu_postlaunch_bootstrap_probe_after_ms, 0,
  "Debug: After guest main thread is running, if Vd/MMIO bootstrap activity "
  "is still zero for this many ms, inject guest-style bootstrap probe.",
  "GPU");
DEFINE_uint32(
  gpu_postlaunch_bootstrap_probe_interval_ms, 1000,
  "Debug: Minimum interval between post-launch guest-style bootstrap probes.",
  "GPU");
DEFINE_bool(
  gpu_postlaunch_bootstrap_probe_once, true,
  "Debug: If true, run post-launch guest-style bootstrap probe only once.",
  "GPU");
DEFINE_uint32(
  gpu_postmodule_bootstrap_probe_after_ms, 0,
  "Debug: After module-load end is observed, if guest-main launch is still "
  "not ready and Vd/MMIO bootstrap activity remains zero for this many ms, "
  "inject guest-style bootstrap probe.",
  "GPU");
DEFINE_uint32(
  gpu_postmodule_bootstrap_probe_interval_ms, 1000,
  "Debug: Minimum interval between post-module guest-style bootstrap "
  "probes while guest-main is still not ready.",
  "GPU");
DEFINE_bool(
  gpu_postmodule_bootstrap_probe_once, true,
  "Debug: If true, run post-module guest-style bootstrap probe only once.",
  "GPU");
DEFINE_uint32(
  gpu_debug_guest_bootstrap_keepalive_ms, 0,
  "Debug: If >0, while host-debug-swap-only state persists and ring is "
  "idle (read==write), reinject guest-style swap packets at this interval "
  "to reduce prolonged worker idle waits during diagnosis.",
  "GPU");
DEFINE_bool(
  gpu_debug_probe_cp_regs_when_mmio_silent, false,
  "Debug: When MMIO tracking remains silent, poll CP_RB/FETCH0 register_file "
  "state and replay detected writes through the MMIO thunk so ring/bootstrap "
  "telemetry and handlers still observe them.",
  "GPU");
DEFINE_uint32(
  gpu_debug_probe_cp_regs_interval_ms, 250,
  "Debug: Minimum interval in milliseconds between silent-MMIO CP register "
  "fallback probes.",
  "GPU");

namespace xe {
namespace gpu {

namespace {
void ForcedGpuInterruptCallback(xe::cpu::ppc::PPCContext* ppc_context,
                                void* arg0, void* arg1) {
  (void)arg1;
  auto* graphics_system = reinterpret_cast<GraphicsSystem*>(arg0);
  if (!graphics_system) {
    return;
  }
  static std::atomic<uint64_t> call_count{0};
  uint64_t call_id = call_count.fetch_add(1) + 1;
  if (call_id <= 5 || (call_id % 120) == 0) {
    uint64_t source = ppc_context ? ppc_context->r[3] : 0;
    uint64_t user_data = ppc_context ? ppc_context->r[4] : 0;
    XELOGW(
        "DEBUG: Forced GPU interrupt callback invoked (count={}, source={}, "
        "user_data=0x{:X})",
        call_id, source, user_data);
  }
}

bool ShouldLogEvery(uint64_t count, int every_n) {
  if (count <= 5) {
    return true;
  }
  if (every_n <= 0) {
    return false;
  }
  return (count % static_cast<uint64_t>(every_n)) == 0;
}

const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat*
FindGpuBootstrapOrdinalRuntimeStat(
    const kernel::xboxkrnl::GpuBootstrapImportLookupStats& stats,
    uint16_t ordinal) {
  for (const auto& stat : stats.ordinal_runtime_stats) {
    if (stat.ordinal == ordinal) {
      return &stat;
    }
  }
  return nullptr;
}

uint64_t GetGpuBootstrapOrdinalRuntimeTotal(
    const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* stat) {
  if (!stat) {
    return 0;
  }
  return stat->dynamic_lookup_calls + stat->static_thunk_calls +
         stat->export_entry_calls;
}

uint64_t GetGpuBootstrapOrdinalRuntimeTotal(
    const kernel::xboxkrnl::GpuBootstrapImportLookupStats& stats,
    uint16_t ordinal) {
  return GetGpuBootstrapOrdinalRuntimeTotal(
      FindGpuBootstrapOrdinalRuntimeStat(stats, ordinal));
}

enum class DispatchPathEvidenceCase {
  kNone = 0,
  kGuestMainDispatchBridge = 1,
  kCriticalGpuRuntimeDispatch = 2,
};

struct DispatchPathEvidence {
  DispatchPathEvidenceCase evidence_case = DispatchPathEvidenceCase::kNone;
  uint64_t guest_main_dispatch_bridge_total = 0;
  uint64_t critical_runtime_dispatch_total = 0;
  uint64_t combined_runtime_dispatch_total = 0;
};

DispatchPathEvidence ClassifyDispatchPathEvidence(
    const kernel::xboxkrnl::GpuBootstrapImportLookupStats& stats) {
  DispatchPathEvidence evidence;
  evidence.guest_main_dispatch_bridge_total =
      stats.guest_main_dispatch_bridge_calls_total;
  evidence.critical_runtime_dispatch_total =
      GetGpuBootstrapOrdinalRuntimeTotal(stats, 0x01C3) +
      GetGpuBootstrapOrdinalRuntimeTotal(stats, 0x01D5) +
      GetGpuBootstrapOrdinalRuntimeTotal(stats, 0x025B);
  evidence.combined_runtime_dispatch_total =
      evidence.guest_main_dispatch_bridge_total +
      evidence.critical_runtime_dispatch_total;
  if (evidence.critical_runtime_dispatch_total != 0) {
    evidence.evidence_case = DispatchPathEvidenceCase::kCriticalGpuRuntimeDispatch;
  } else if (evidence.guest_main_dispatch_bridge_total != 0) {
    evidence.evidence_case = DispatchPathEvidenceCase::kGuestMainDispatchBridge;
  }
  return evidence;
}

const char* DispatchPathEvidenceCaseName(
    DispatchPathEvidenceCase evidence_case) {
  switch (evidence_case) {
    case DispatchPathEvidenceCase::kNone:
      return "none";
    case DispatchPathEvidenceCase::kGuestMainDispatchBridge:
      return "guest_main_dispatch_bridge";
    case DispatchPathEvidenceCase::kCriticalGpuRuntimeDispatch:
      return "critical_gpu_runtime_dispatch";
  }
  return "unknown";
}

// The first few dispatch samples after processor entry often sit in
// __savegprlr_* before the title settles into its real startup path. Treat
// that as a warm-up window, not as callback starvation.
constexpr uint64_t kStartupPrologHelperGraceWindowMs = 125;
constexpr uint32_t kStartupPrologHelperGraceRepeatSamples = 6;

bool IsStartupPrologHelperGraceWindow(const std::string& state,
                                      uint64_t age_ms,
                                      uint32_t repeat_samples) {
  return state == "startup_prolog_helper_only" &&
         age_ms < kStartupPrologHelperGraceWindowMs &&
         repeat_samples <= kStartupPrologHelperGraceRepeatSamples;
}

enum class XexCheckExecutablePrivilegeRuntimeState {
  kNoStaticThunkObserved = 0,
  kStaticThunkWithoutExportEntry = 1,
  kExportEntryObserved = 2,
  kExportEntryWithoutStaticThunkObservation = 3,
};

XexCheckExecutablePrivilegeRuntimeState
ClassifyXexCheckExecutablePrivilegeRuntimeState(
    const kernel::xboxkrnl::GpuBootstrapImportLookupStats& stats) {
  const bool export_entry_seen =
      stats.xex_check_executable_privilege_export_entry_calls != 0 ||
      stats.xex_check_executable_privilege_calls != 0;
  const bool static_thunk_seen =
      stats.xex_check_executable_privilege_static_thunk_calls != 0;
  if (export_entry_seen) {
    return static_thunk_seen
               ? XexCheckExecutablePrivilegeRuntimeState::kExportEntryObserved
               : XexCheckExecutablePrivilegeRuntimeState::
                     kExportEntryWithoutStaticThunkObservation;
  }
  if (static_thunk_seen) {
    return XexCheckExecutablePrivilegeRuntimeState::
        kStaticThunkWithoutExportEntry;
  }
  return XexCheckExecutablePrivilegeRuntimeState::kNoStaticThunkObserved;
}

const char* XexCheckExecutablePrivilegeRuntimeStateName(
    XexCheckExecutablePrivilegeRuntimeState state) {
  switch (state) {
    case XexCheckExecutablePrivilegeRuntimeState::kNoStaticThunkObserved:
      return "no_static_thunk_observed";
    case XexCheckExecutablePrivilegeRuntimeState::
        kStaticThunkWithoutExportEntry:
      return "static_thunk_without_export_entry";
    case XexCheckExecutablePrivilegeRuntimeState::kExportEntryObserved:
      return "export_entry_observed";
    case XexCheckExecutablePrivilegeRuntimeState::
        kExportEntryWithoutStaticThunkObservation:
      return "export_entry_without_static_thunk_observation";
  }
  return "unknown";
}

enum class CriticalGpuOrdinalPipelineState {
  kStaticImportWiringMissing = 0,
  kStaticImportProbeInvalid = 1,
  kAwaitingDispatchPath = 2,
  kDispatchBridgeBeforePrivilegeExportEntry = 3,
  kPrivilegeThunkBeforeExportEntry = 4,
  kPrivilegeExportEntryBeforeLoaderOrStaticDispatch = 5,
  kDispatchBridgeBeforeLoaderOrStaticDispatch = 6,
  kLoaderOrLookupSeenButOrdinalIdle = 7,
  kLookupPendingResolution = 8,
  kLookupMissing = 9,
  kLookupInvalidModule = 10,
  kLookupResolvedWithoutRuntimeEntry = 11,
  kThunkDispatchWithoutExportEntry = 12,
  kExportEntryWithoutGuestEffect = 13,
  kRuntimeValidated = 14,
};

struct CriticalGpuOrdinalFoundationState {
  CriticalGpuOrdinalPipelineState state =
      CriticalGpuOrdinalPipelineState::kStaticImportWiringMissing;
  const char* role = "<unknown>";
  const char* expected_effect = "<unknown>";
  const char* next_transition = "unknown";
  bool runtime_valid = false;
};

const char* GetCriticalGpuOrdinalRoleName(uint16_t ordinal) {
  switch (ordinal) {
    case 0x01C2:
      return "engine_init";
    case 0x01C3:
      return "ring_init";
    case 0x01D5:
      return "interrupt_callback";
    case 0x025B:
      return "swap_producer";
    default:
      return "bootstrap";
  }
}

const char* GetCriticalGpuOrdinalExpectedEffectName(uint16_t ordinal) {
  switch (ordinal) {
    case 0x01C2:
      return "engine_init_progress";
    case 0x01C3:
      return "ring_init_or_mmio_setup";
    case 0x01D5:
      return "interrupt_callback_registered";
    case 0x025B:
      return "swap_or_wptr_progress";
    default:
      return "guest_side_effect";
  }
}

const char* CriticalGpuOrdinalPipelineStateName(
    CriticalGpuOrdinalPipelineState state) {
  switch (state) {
    case CriticalGpuOrdinalPipelineState::kStaticImportWiringMissing:
      return "static_import_wiring_missing";
    case CriticalGpuOrdinalPipelineState::kStaticImportProbeInvalid:
      return "static_import_probe_invalid";
    case CriticalGpuOrdinalPipelineState::kAwaitingDispatchPath:
      return "awaiting_dispatch_path";
    case CriticalGpuOrdinalPipelineState::
        kDispatchBridgeBeforePrivilegeExportEntry:
      return "dispatch_bridge_before_xexcheck_entry";
    case CriticalGpuOrdinalPipelineState::kPrivilegeThunkBeforeExportEntry:
      return "xexcheck_thunk_before_export_entry";
    case CriticalGpuOrdinalPipelineState::
        kPrivilegeExportEntryBeforeLoaderOrStaticDispatch:
      return "xexcheck_entry_before_loader_or_static_dispatch";
    case CriticalGpuOrdinalPipelineState::
        kDispatchBridgeBeforeLoaderOrStaticDispatch:
      return "dispatch_bridge_before_loader_or_static_dispatch";
    case CriticalGpuOrdinalPipelineState::kLoaderOrLookupSeenButOrdinalIdle:
      return "loader_or_lookup_seen_but_ordinal_idle";
    case CriticalGpuOrdinalPipelineState::kLookupPendingResolution:
      return "lookup_pending_resolution";
    case CriticalGpuOrdinalPipelineState::kLookupMissing:
      return "lookup_missing";
    case CriticalGpuOrdinalPipelineState::kLookupInvalidModule:
      return "lookup_invalid_module";
    case CriticalGpuOrdinalPipelineState::
        kLookupResolvedWithoutRuntimeEntry:
      return "lookup_resolved_without_runtime_entry";
    case CriticalGpuOrdinalPipelineState::kThunkDispatchWithoutExportEntry:
      return "thunk_dispatch_without_export_entry";
    case CriticalGpuOrdinalPipelineState::kExportEntryWithoutGuestEffect:
      return "export_entry_without_guest_effect";
    case CriticalGpuOrdinalPipelineState::kRuntimeValidated:
      return "runtime_validated";
  }
  return "unknown";
}

const char* CriticalGpuOrdinalNextTransitionName(
    CriticalGpuOrdinalPipelineState state) {
  switch (state) {
    case CriticalGpuOrdinalPipelineState::kStaticImportWiringMissing:
      return "static_import_wiring";
    case CriticalGpuOrdinalPipelineState::kStaticImportProbeInvalid:
      return "static_import_probe";
    case CriticalGpuOrdinalPipelineState::kAwaitingDispatchPath:
      return "dispatch_path";
    case CriticalGpuOrdinalPipelineState::
        kDispatchBridgeBeforePrivilegeExportEntry:
      return "bridge_to_xexcheck_entry";
    case CriticalGpuOrdinalPipelineState::kPrivilegeThunkBeforeExportEntry:
      return "xexcheck_thunk_to_export_entry";
    case CriticalGpuOrdinalPipelineState::
        kPrivilegeExportEntryBeforeLoaderOrStaticDispatch:
      return "xexcheck_to_loader_or_static_dispatch";
    case CriticalGpuOrdinalPipelineState::
        kDispatchBridgeBeforeLoaderOrStaticDispatch:
      return "bridge_to_loader_or_static_dispatch";
    case CriticalGpuOrdinalPipelineState::kLoaderOrLookupSeenButOrdinalIdle:
      return "ordinal_runtime_dispatch";
    case CriticalGpuOrdinalPipelineState::kLookupPendingResolution:
      return "lookup_resolution";
    case CriticalGpuOrdinalPipelineState::kLookupMissing:
      return "lookup_resolution_missing";
    case CriticalGpuOrdinalPipelineState::kLookupInvalidModule:
      return "lookup_module_validation";
    case CriticalGpuOrdinalPipelineState::
        kLookupResolvedWithoutRuntimeEntry:
      return "runtime_entry_after_lookup";
    case CriticalGpuOrdinalPipelineState::kThunkDispatchWithoutExportEntry:
      return "export_entry_after_thunk";
    case CriticalGpuOrdinalPipelineState::kExportEntryWithoutGuestEffect:
      return "guest_side_effect_after_export";
    case CriticalGpuOrdinalPipelineState::kRuntimeValidated:
      return "complete";
  }
  return "unknown";
}

CriticalGpuOrdinalFoundationState ClassifyCriticalGpuOrdinalFoundation(
    uint16_t ordinal, bool static_import_wiring_found, bool import_probe_ready,
    const kernel::xboxkrnl::GpuBootstrapImportLookupStats& proc_stats,
    const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* stat,
    DispatchPathEvidenceCase dispatch_evidence_case, bool side_effect_seen,
    bool privilege_branch_only_seen) {
  CriticalGpuOrdinalFoundationState foundation;
  foundation.role = GetCriticalGpuOrdinalRoleName(ordinal);
  foundation.expected_effect = GetCriticalGpuOrdinalExpectedEffectName(ordinal);
  XexCheckExecutablePrivilegeRuntimeState xexcheck_runtime_state =
      ClassifyXexCheckExecutablePrivilegeRuntimeState(proc_stats);

  bool loader_or_lookup_activity_seen =
      proc_stats.loader_module_handle_calls_total != 0 ||
      proc_stats.loader_proc_lookup_calls_total != 0 ||
      proc_stats.proc_lookup_calls != 0 || proc_stats.proc_lookup_resolved != 0 ||
      proc_stats.proc_lookup_missing != 0 ||
      proc_stats.proc_lookup_invalid_module != 0 ||
      proc_stats.proc_lookup_fallback_hits != 0;
  uint64_t runtime_total = GetGpuBootstrapOrdinalRuntimeTotal(stat);

  if (!static_import_wiring_found) {
    foundation.state =
        CriticalGpuOrdinalPipelineState::kStaticImportWiringMissing;
  } else if (!import_probe_ready) {
    foundation.state =
        CriticalGpuOrdinalPipelineState::kStaticImportProbeInvalid;
  } else if (runtime_total == 0) {
    switch (dispatch_evidence_case) {
      case DispatchPathEvidenceCase::kNone:
        foundation.state =
            CriticalGpuOrdinalPipelineState::kAwaitingDispatchPath;
        break;
      case DispatchPathEvidenceCase::kGuestMainDispatchBridge:
        if (xexcheck_runtime_state ==
                XexCheckExecutablePrivilegeRuntimeState::
                    kExportEntryObserved ||
            xexcheck_runtime_state ==
                XexCheckExecutablePrivilegeRuntimeState::
                    kExportEntryWithoutStaticThunkObservation) {
          foundation.state = CriticalGpuOrdinalPipelineState::
              kPrivilegeExportEntryBeforeLoaderOrStaticDispatch;
        } else if (privilege_branch_only_seen &&
                   xexcheck_runtime_state ==
                       XexCheckExecutablePrivilegeRuntimeState::
                           kStaticThunkWithoutExportEntry) {
          foundation.state =
              CriticalGpuOrdinalPipelineState::kPrivilegeThunkBeforeExportEntry;
        } else if (privilege_branch_only_seen) {
          foundation.state = CriticalGpuOrdinalPipelineState::
              kDispatchBridgeBeforePrivilegeExportEntry;
        } else {
          foundation.state = loader_or_lookup_activity_seen
                                 ? CriticalGpuOrdinalPipelineState::
                                       kLoaderOrLookupSeenButOrdinalIdle
                                 : CriticalGpuOrdinalPipelineState::
                                       kDispatchBridgeBeforeLoaderOrStaticDispatch;
        }
        break;
      case DispatchPathEvidenceCase::kCriticalGpuRuntimeDispatch:
        foundation.state =
            CriticalGpuOrdinalPipelineState::kLoaderOrLookupSeenButOrdinalIdle;
        break;
    }
  } else if (stat && stat->dynamic_lookup_calls != 0 &&
             stat->dynamic_lookup_resolved_calls == 0 &&
             stat->dynamic_lookup_missing_calls == 0 &&
             stat->dynamic_lookup_invalid_module_calls == 0 &&
             stat->static_thunk_calls == 0 && stat->export_entry_calls == 0) {
    foundation.state =
        CriticalGpuOrdinalPipelineState::kLookupPendingResolution;
  } else if (stat && stat->dynamic_lookup_missing_calls != 0) {
    foundation.state = CriticalGpuOrdinalPipelineState::kLookupMissing;
  } else if (stat && stat->dynamic_lookup_invalid_module_calls != 0) {
    foundation.state = CriticalGpuOrdinalPipelineState::kLookupInvalidModule;
  } else if (stat && stat->static_thunk_calls != 0 &&
             stat->export_entry_calls == 0) {
    foundation.state =
        CriticalGpuOrdinalPipelineState::kThunkDispatchWithoutExportEntry;
  } else if (stat && stat->dynamic_lookup_resolved_calls != 0 &&
             stat->export_entry_calls == 0) {
    foundation.state = CriticalGpuOrdinalPipelineState::
        kLookupResolvedWithoutRuntimeEntry;
  } else if (stat && stat->export_entry_calls != 0 && !side_effect_seen) {
    foundation.state =
        CriticalGpuOrdinalPipelineState::kExportEntryWithoutGuestEffect;
  } else {
    foundation.state = CriticalGpuOrdinalPipelineState::kRuntimeValidated;
  }

  foundation.next_transition =
      CriticalGpuOrdinalNextTransitionName(foundation.state);
  foundation.runtime_valid =
      foundation.state == CriticalGpuOrdinalPipelineState::kRuntimeValidated;
  return foundation;
}

std::string FormatStartupGateUnmetConditions(bool startup_bridge_ready,
                                             bool callback_gate_ready,
                                             bool bootstrap_activity_seen) {
  std::string unmet;
  auto append = [&unmet](const char* condition) {
    if (!unmet.empty()) {
      unmet += ",";
    }
    unmet += condition;
  };
  if (!startup_bridge_ready) {
    append("startup_bridge_ready");
  }
  if (!callback_gate_ready) {
    append("callback_startup_gate_ready");
  }
  if (!bootstrap_activity_seen) {
    append("bootstrap_activity");
  }
  return unmet.empty() ? "none" : unmet;
}

void LogNoForceProfileSuppressed(const char* scope, uint64_t phase_age_ms,
                                 uint64_t now_ms,
                                 std::atomic<uint64_t>* last_log_ms) {
  if (last_log_ms) {
    uint64_t interval_ms = static_cast<uint64_t>(
        cvars::gpu_no_force_run_profile_log_interval_ms);
    uint64_t prev_log_ms = last_log_ms->load(std::memory_order_relaxed);
    if (prev_log_ms != 0 && interval_ms != 0 && now_ms >= prev_log_ms &&
        now_ms - prev_log_ms < interval_ms) {
      return;
    }
    last_log_ms->store(now_ms, std::memory_order_relaxed);
  }
  XELOGW("DEBUG: {} suppressed by gpu_no_force_run_profile (age={}ms)", scope,
         phase_age_ms);
}

struct ForceFallbackDelaySnapshot {
  kernel::KernelState::UserModuleLoadStats load_stats;
  bool executable_module_present = false;
  bool executable_module_ready = false;
  uint32_t executable_entry_point = 0;
  const char* reason = "disabled";
};

bool ShouldDeferForceFallback(kernel::KernelState* kernel_state,
                              ForceFallbackDelaySnapshot* out_snapshot = nullptr) {
  ForceFallbackDelaySnapshot snapshot;
  if (cvars::gpu_no_force_run_profile) {
    snapshot.reason = "no_force_profile";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }
  if (!cvars::gpu_suspend_force_fallback_until_module_ready) {
    snapshot.reason = "gate_disabled";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return false;
  }
  if (!kernel_state) {
    snapshot.reason = "kernel_state_unavailable";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }

  snapshot.load_stats = kernel_state->GetUserModuleLoadStats();
  auto executable_module = kernel_state->GetExecutableModule();
  snapshot.executable_module_present = executable_module != nullptr;
  snapshot.executable_entry_point =
      executable_module ? executable_module->entry_point() : 0;
  snapshot.executable_module_ready =
      snapshot.executable_module_present &&
      snapshot.executable_entry_point != 0;

  if (snapshot.executable_module_ready) {
    snapshot.reason = "executable_ready";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return false;
  }
  if (snapshot.load_stats.load_inflight) {
    snapshot.reason = "load_inflight";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }
  if (snapshot.load_stats.load_calls == 0) {
    snapshot.reason = "awaiting_first_load_call";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }
  if (snapshot.load_stats.loaded_executable_user_modules == 0) {
    snapshot.reason = "no_loaded_executable_modules";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }
  if (!snapshot.executable_module_present) {
    snapshot.reason = "executable_not_wired";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }
  if (snapshot.executable_entry_point == 0) {
    snapshot.reason = "executable_entry_zero";
    if (out_snapshot) {
      *out_snapshot = snapshot;
    }
    return true;
  }

  snapshot.reason = "module_ready";
  if (out_snapshot) {
    *out_snapshot = snapshot;
  }
  return false;
}

void LogForceFallbackDeferred(const char* scope, uint64_t phase_age_ms,
                              uint64_t now_ms,
                              const ForceFallbackDelaySnapshot& snapshot,
                              std::atomic<uint64_t>* last_log_ms) {
  if (last_log_ms) {
    uint64_t interval_ms =
        static_cast<uint64_t>(cvars::gpu_suspend_force_fallback_log_interval_ms);
    uint64_t prev_log_ms = last_log_ms->load(std::memory_order_relaxed);
    if (prev_log_ms != 0 && interval_ms != 0 && now_ms >= prev_log_ms &&
        now_ms - prev_log_ms < interval_ms) {
      return;
    }
    last_log_ms->store(now_ms, std::memory_order_relaxed);
  }
  bool load_begin_seen = snapshot.load_stats.last_begin_ms != 0;
  bool load_end_seen = snapshot.load_stats.last_end_ms != 0;
  bool load_progress_seen = snapshot.load_stats.last_progress_ms != 0;
  uint64_t since_load_begin_ms =
      load_begin_seen && now_ms >= snapshot.load_stats.last_begin_ms
          ? now_ms - snapshot.load_stats.last_begin_ms
          : 0;
  uint64_t since_load_end_ms =
      load_end_seen && now_ms >= snapshot.load_stats.last_end_ms
          ? now_ms - snapshot.load_stats.last_end_ms
          : 0;
  uint64_t since_load_progress_ms =
      load_progress_seen && now_ms >= snapshot.load_stats.last_progress_ms
          ? now_ms - snapshot.load_stats.last_progress_ms
          : 0;
  XELOGW(
      "DEBUG: {} deferred by safety/profile gate: reason={} age={}ms "
      "load_inflight={} load_calls={} load_success={} load_fail={} "
      "loaded_user_modules={} loaded_executable_user_modules={} "
      "load_begin_seen={} since_load_begin={}ms load_end_seen={} "
      "since_load_end={}ms load_progress_seen={} since_load_progress={}ms "
      "load_progress_count={} executable_present={} executable_entry={:08X}",
      scope, snapshot.reason, phase_age_ms,
      snapshot.load_stats.load_inflight ? "YES" : "NO",
      snapshot.load_stats.load_calls, snapshot.load_stats.load_success,
      snapshot.load_stats.load_fail, snapshot.load_stats.loaded_user_modules,
      snapshot.load_stats.loaded_executable_user_modules,
      load_begin_seen ? "YES" : "NO", since_load_begin_ms,
      load_end_seen ? "YES" : "NO", since_load_end_ms,
      load_progress_seen ? "YES" : "NO", since_load_progress_ms,
      snapshot.load_stats.load_progress_count,
      snapshot.executable_module_present ? "YES" : "NO",
      snapshot.executable_entry_point);
}

enum class WptrUpdateSource : uint32_t {
  kUnknown = 0,
  kMmio = 1,
  kCpPacket = 2,
  kDebugSwapInject = 3,
  kOther = 4,
};

WptrUpdateSource ClassifyWptrUpdateSource(const char* source) {
  if (!source || source[0] == '\0') {
    return WptrUpdateSource::kUnknown;
  }
  std::string_view source_sv(source);
  if (source_sv == "CP_RB_WPTR") {
    return WptrUpdateSource::kMmio;
  }
  if (source_sv == "CP packet CP_RB_WPTR") {
    return WptrUpdateSource::kCpPacket;
  }
  if (source_sv == "DebugSwapInject") {
    return WptrUpdateSource::kDebugSwapInject;
  }
  return WptrUpdateSource::kOther;
}

const char* WptrUpdateSourceName(uint32_t source_id) {
  switch (static_cast<WptrUpdateSource>(source_id)) {
    case WptrUpdateSource::kMmio:
      return "mmio";
    case WptrUpdateSource::kCpPacket:
      return "cp_packet";
    case WptrUpdateSource::kDebugSwapInject:
      return "debug_swap";
    case WptrUpdateSource::kOther:
      return "other";
    default:
      return "unknown";
  }
}

void DumpFetch0(const xenos::xe_gpu_texture_fetch_t& fetch) {
  XELOGI("   fetch0 dword_0=0x{:08X}", fetch.dword_0);
  XELOGI("   fetch0 dword_1=0x{:08X}", fetch.dword_1);
  XELOGI("   fetch0 dword_2=0x{:08X}", fetch.dword_2);
  XELOGI("   fetch0 dword_3=0x{:08X}", fetch.dword_3);
  XELOGI("   fetch0 dword_4=0x{:08X}", fetch.dword_4);
  XELOGI("   fetch0 dword_5=0x{:08X}", fetch.dword_5);
  XELOGI("   fetch0 format=0x{:X} endianness=0x{:X} swizzle=0x{:X}",
         static_cast<uint32_t>(fetch.format),
         static_cast<uint32_t>(fetch.endianness), fetch.swizzle);
  XELOGI("   fetch0 base_address=0x{:08X} mip_address=0x{:08X}",
         fetch.base_address, fetch.mip_address);
  XELOGI("   fetch0 size_2d width={} height={} stack_depth={} pitch=0x{:X}",
         fetch.size_2d.width, fetch.size_2d.height, fetch.size_2d.stack_depth,
         fetch.pitch);
  XELOGI("   fetch0 tiled={} packed_mips={} dimension=0x{:X}",
         fetch.tiled ? "YES" : "NO", fetch.packed_mips ? "YES" : "NO",
         static_cast<uint32_t>(fetch.dimension));
}

void DumpRingWindow(const uint8_t* buffer, uint32_t ring_dwords,
                    uint32_t start_dword, uint32_t count) {
  if (!buffer || ring_dwords == 0 || count == 0) {
    XELOGI("   ring buffer window: <unavailable>");
    return;
  }
  uint32_t clamped = std::min<uint32_t>(count, ring_dwords);
  XELOGI("   ring buffer window (start={}, count={}, ring_dwords={}):",
         start_dword, clamped, ring_dwords);
  for (uint32_t i = 0; i < clamped; ++i) {
    uint32_t index = (start_dword + i) % ring_dwords;
    uint32_t value =
      ::xe::load_and_swap<uint32_t>(buffer + index * sizeof(uint32_t));
    XELOGI("      rb[{:04X}] = {:08X}", index, value);
  }
}

const char* GetBootstrapMmioRegName(uint32_t reg) {
  switch (reg) {
    case 0x01C0:
      return "CP_RB_BASE";
    case 0x01C1:
      return "CP_RB_CNTL";
    case 0x01C3:
      return "CP_RB_RPTR_ADDR";
    case 0x01C4:
      return "CP_RB_RPTR";
    case 0x01C5:
      return "CP_RB_WPTR";
    case 0x01C6:
      return "CP_RB_WPTR_DELAY";
    case 0x01C7:
      return "CP_RB_RPTR_WR";
    default:
      break;
  }
  if (reg >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      reg <= (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 5)) {
    return "FETCH0_WORD";
  }
  return nullptr;
}

bool IsCpRingAddressPlausible(uint32_t value, uint32_t alignment_mask) {
  if (value == 0) {
    return false;
  }
  return (value & alignment_mask) == 0;
}

bool IsCpRingControlPlausible(uint32_t value) {
  // CP_RB_CNTL[13:8] stores a ring size exponent used by Xenia as size_log2.
  // For a configured ring, this should be in a small bounded range.
  uint32_t size_log2 = (value >> 8) & 0x3Fu;
  return size_log2 > 0 && size_log2 < 32;
}

struct CpMmioNormalizationResult {
  uint32_t normalized_value = 0;
  uint32_t swapped_value = 0;
  bool swapped = false;
  bool raw_plausible = true;
  bool swapped_plausible = false;
  const char* reason = "raw";
};

CpMmioNormalizationResult NormalizeCpMmioWriteValue(uint32_t reg,
                                                    uint32_t raw_value) {
  CpMmioNormalizationResult result;
  result.normalized_value = raw_value;
  result.swapped_value = ::xe::byte_swap(raw_value);
  switch (reg) {
    case 0x01C0:  // CP_RB_BASE
      result.raw_plausible = IsCpRingAddressPlausible(raw_value, 0x0FFFu);
      result.swapped_plausible =
          IsCpRingAddressPlausible(result.swapped_value, 0x0FFFu);
      if (!result.raw_plausible && result.swapped_plausible) {
        result.normalized_value = result.swapped_value;
        result.swapped = true;
        result.reason = "cp_rb_base_alignment";
      }
      break;
    case 0x01C1:  // CP_RB_CNTL
      result.raw_plausible = IsCpRingControlPlausible(raw_value);
      result.swapped_plausible = IsCpRingControlPlausible(result.swapped_value);
      if (!result.raw_plausible && result.swapped_plausible) {
        result.normalized_value = result.swapped_value;
        result.swapped = true;
        result.reason = "cp_rb_cntl_size_log2";
      }
      break;
    case 0x01C3:  // CP_RB_RPTR_ADDR
      result.raw_plausible = IsCpRingAddressPlausible(raw_value, 0x3u);
      result.swapped_plausible =
          IsCpRingAddressPlausible(result.swapped_value, 0x3u);
      if (!result.raw_plausible && result.swapped_plausible) {
        result.normalized_value = result.swapped_value;
        result.swapped = true;
        result.reason = "cp_rb_rptr_addr_alignment";
      }
      break;
    case 0x01C4:  // CP_RB_RPTR
    case 0x01C5:  // CP_RB_WPTR
    case 0x01C6:  // CP_RB_WPTR_DELAY
    case 0x01C7:  // CP_RB_RPTR_WR
      result.raw_plausible = (raw_value & 0x3u) == 0;
      result.swapped_plausible = (result.swapped_value & 0x3u) == 0;
      if (!result.raw_plausible && result.swapped_plausible) {
        result.normalized_value = result.swapped_value;
        result.swapped = true;
        result.reason = "cp_rb_pointer_alignment";
      }
      break;
    default:
      result.raw_plausible = true;
      result.swapped_plausible = false;
      break;
  }
  return result;
}

bool IsGpuBootstrapOrdinal(uint32_t ordinal) {
  switch (ordinal) {
    case 0x01B6:  // VdEnableRingBufferRPtrWriteBack
    case 0x01BC:  // VdGetGraphicsAsicID
    case 0x01BD:  // VdGetSystemCommandBuffer
    case 0x01BE:  // VdGlobalDevice
    case 0x01BF:  // VdGlobalXamDevice
    case 0x01C2:  // VdInitializeEngines
    case 0x01C3:  // VdInitializeRingBuffer
    case 0x01C6:  // VdIsHSIOTrainingSucceeded
    case 0x01D5:  // VdSetGraphicsInterruptCallback
    case 0x025B:  // VdSwap
    case 0x0268:  // VdInitializeEDRAM
    case 0x0269:  // VdRetrainEDRAM
    case 0x026A:  // VdRetrainEDRAMWorker
      return true;
    default:
      return false;
  }
}

const char* GetGpuBootstrapOrdinalName(uint32_t ordinal) {
  switch (ordinal) {
    case 0x01B6:
      return "VdEnableRingBufferRPtrWriteBack";
    case 0x01BC:
      return "VdGetGraphicsAsicID";
    case 0x01BD:
      return "VdGetSystemCommandBuffer";
    case 0x01BE:
      return "VdGlobalDevice";
    case 0x01BF:
      return "VdGlobalXamDevice";
    case 0x01C2:
      return "VdInitializeEngines";
    case 0x01C3:
      return "VdInitializeRingBuffer";
    case 0x01C6:
      return "VdIsHSIOTrainingSucceeded";
    case 0x01D5:
      return "VdSetGraphicsInterruptCallback";
    case 0x025B:
      return "VdSwap";
    case 0x0268:
      return "VdInitializeEDRAM";
    case 0x0269:
      return "VdRetrainEDRAM";
    case 0x026A:
      return "VdRetrainEDRAMWorker";
    default:
      return "<unknown>";
  }
}

const char* GetBootstrapStageNameFromId(uint32_t stage_id) {
  switch (stage_id) {
    case 0:
      return "PARTIAL_BOOTSTRAP_NO_ACK";
    case 1:
      return "BOOTSTRAP_NEVER_ENTERED";
    case 2:
      return "PRE_RING_VIDEO_BOOTSTRAP_STALLED";
    case 3:
      return "VD_INIT_CALLED_RING_NOT_READY";
    case 4:
      return "RING_INIT_WITHOUT_ACK";
    case 5:
      return "NO_CP_MMIO_RING_SETUP";
    default:
      return "UNKNOWN";
  }
}

struct BootstrapGateSnapshot {
  uint32_t address = 0x821F4898;
  uint32_t value = 0;
  bool committed = false;
  bool translated = false;
};

BootstrapGateSnapshot ReadBootstrapGateSnapshot(
    kernel::KernelState* kernel_state) {
  BootstrapGateSnapshot snapshot;
  auto* memory = kernel_state ? kernel_state->memory() : nullptr;
  if (!memory) {
    return snapshot;
  }
  auto heap = memory->LookupHeap(snapshot.address);
  xe::HeapAllocationInfo info = {};
  if (!heap || !heap->QueryRegionInfo(snapshot.address, &info) ||
      !(info.state & xe::kMemoryAllocationCommit)) {
    return snapshot;
  }
  snapshot.committed = true;
  auto translated_ptr =
      memory->TranslateVirtualSafe<::xe::be<uint32_t>*>(snapshot.address);
  if (translated_ptr.success && translated_ptr.pointer) {
    snapshot.value = ::xe::load_and_swap<uint32_t>(translated_ptr.pointer);
    snapshot.translated = true;
  }
  return snapshot;
}

bool ReadGuestWordSnapshot(xe::Memory* memory, uint32_t address, uint32_t* out_value,
                           bool* out_committed, bool* out_translated) {
  uint32_t value = 0;
  bool committed = false;
  bool translated = false;
  if (memory) {
    auto* heap = memory->LookupHeap(address);
    xe::HeapAllocationInfo info = {};
    if (heap && heap->QueryRegionInfo(address, &info) &&
        (info.state & xe::kMemoryAllocationCommit)) {
      committed = true;
    }
    auto safe = memory->TranslateVirtualSafe<::xe::be<uint32_t>*>(address);
    if (safe.success && safe.pointer) {
      value = ::xe::load_and_swap<uint32_t>(safe.pointer);
      translated = true;
    }
  }
  if (out_value) {
    *out_value = value;
  }
  if (out_committed) {
    *out_committed = committed;
  }
  if (out_translated) {
    *out_translated = translated;
  }
  return translated;
}

const char* GetKthreadStateName(uint8_t state) {
  switch (state) {
    case kernel::KTHREAD_STATE_INITIALIZED:
      return "INITIALIZED";
    case kernel::KTHREAD_STATE_READY:
      return "READY";
    case kernel::KTHREAD_STATE_RUNNING:
      return "RUNNING";
    case kernel::KTHREAD_STATE_STANDBY:
      return "STANDBY";
    case kernel::KTHREAD_STATE_TERMINATED:
      return "TERMINATED";
    case kernel::KTHREAD_STATE_WAITING:
      return "WAITING";
    case kernel::KTHREAD_STATE_UNKNOWN:
      return "UNKNOWN";
    default:
      return "UNMAPPED";
  }
}

const char* GetWaitTypeName(uint16_t wait_type_raw) {
  uint16_t wait_type = wait_type_raw & 0xFF;
  switch (wait_type) {
    case kernel::WAIT_ALL:
      return "WAIT_ALL";
    case kernel::WAIT_ANY:
      return "WAIT_ANY";
    default:
      return "UNMAPPED";
  }
}

const char* GetDispatcherTypeName(uint8_t type) {
  switch (type) {
    case kernel::DISPATCHER_MANUAL_RESET_EVENT:
      return "MANUAL_RESET_EVENT";
    case kernel::DISPATCHER_AUTO_RESET_EVENT:
      return "AUTO_RESET_EVENT";
    case kernel::DISPATCHER_MUTANT:
      return "MUTANT";
    case kernel::DISPATCHER_QUEUE:
      return "QUEUE";
    case kernel::DISPATCHER_SEMAPHORE:
      return "SEMAPHORE";
    case kernel::DISPATCHER_THREAD:
      return "THREAD";
    case kernel::DISPATCHER_MANUAL_RESET_TIMER:
      return "MANUAL_RESET_TIMER";
    case kernel::DISPATCHER_AUTO_RESET_TIMER:
      return "AUTO_RESET_TIMER";
    default:
      return "UNMAPPED";
  }
}

const char* GetKernelObjectTypeName(kernel::XObject::Type type) {
  switch (type) {
    case kernel::XObject::Type::Event:
      return "Event";
    case kernel::XObject::Type::Semaphore:
      return "Semaphore";
    case kernel::XObject::Type::Mutant:
      return "Mutant";
    case kernel::XObject::Type::Thread:
      return "Thread";
    case kernel::XObject::Type::Timer:
      return "Timer";
    case kernel::XObject::Type::File:
      return "File";
    case kernel::XObject::Type::IOCompletion:
      return "IOCompletion";
    case kernel::XObject::Type::Module:
      return "Module";
    default:
      return "Other";
  }
}

const char* ClassifyPpcInstructionWord(uint32_t value) {
  switch (value) {
    case 0x4E800420:
      return "bctr";
    case 0x4E800020:
      return "blr";
    case 0x60000000:
      return "nop";
    case 0x44000042:
      return "sc 2";
    default:
      return nullptr;
  }
}
}  // namespace

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if XE_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // XE_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : frame_limiter_worker_running_(false) {
  register_file_ = reinterpret_cast<RegisterFile*>(memory::AllocFixed(
      nullptr, sizeof(RegisterFile), memory::AllocationType::kReserveCommit,
      memory::PageAccess::kReadWrite));
}

GraphicsSystem::~GraphicsSystem() = default;

void GraphicsSystem::WarnIfRingBufferNeverInitialized(
    uint64_t since_first_vblank) {
  auto cp_dbg = command_processor_ ? command_processor_->GetRingBufferDebugState()
                                   : CommandProcessor::RingBufferDebugState{};
  bool host_ring_ready = cp_dbg.ring_buffer_initialized &&
                         cp_dbg.initialization_acknowledged &&
                         cp_dbg.primary_buffer_ptr != 0 &&
                         cp_dbg.primary_buffer_size != 0;
  if (mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed) == 0 &&
      mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed) == 0 &&
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed) == 0) {
    if (host_ring_ready) {
      XELOGE(
          "RING BUFFER: Guest never wrote CP_RB_BASE/CP_RB_CNTL/CP_RB_WPTR "
          "after {} ms.\n"
          "Host ring buffer is already initialized and acknowledged at "
          "{:08X} (size={:08X}), so the title is stalled before guest "
          "CP_RB programming or command submission.\n"
          "Keeping non-forcing diagnosis; inspect callback/bootstrap "
          "dispatch rather than host-side bootstrap fallback.",
          since_first_vblank, cp_dbg.primary_buffer_ptr,
          cp_dbg.primary_buffer_size);
    } else {
      XELOGE(
          "RING BUFFER: Guest never wrote CP_RB_BASE/CP_RB_CNTL/CP_RB_WPTR "
          "after {} ms.\n"
          "Guest GPU bootstrap never programmed the CP_RB registers.\n"
          "Keeping non-forcing diagnosis; inspect guest bootstrap and "
          "loader dispatch rather than enabling host-side bootstrap "
          "fallback.",
          since_first_vblank);
    }
  } else {
    XELOGI(
        "RING BUFFER: Guest wrote CP_RB_BASE/CP_RB_CNTL/CP_RB_WPTR after "
        "{} ms.",
        since_first_vblank);
  }
}

X_STATUS GraphicsSystem::Setup(cpu::Processor* processor,
                               kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context,
                               bool with_presentation) {
  memory_ = processor->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;

  scaled_aspect_x_ = 16;
  scaled_aspect_y_ = 9;

  auto custom_res_x = cvars::internal_display_resolution_x;
  auto custom_res_y = cvars::internal_display_resolution_y;
  if (!custom_res_x || custom_res_x > 1920 || !custom_res_y ||
      custom_res_y > 1080) {
    OVERRIDE_uint32(internal_display_resolution_x,
                    internal_display_resolution_entries[8].first);
    OVERRIDE_uint32(internal_display_resolution_y,
                    internal_display_resolution_entries[8].second);
    config::SaveConfig();
    xe::FatalError(fmt::format(
        "Invalid custom resolution specified: {}x{}\n"
        "Width must be between 1-1920.\nHeight must be between 1-1080.",
        custom_res_x, custom_res_y));
  }

  XELOGI("DEBUG: PRESENTER CREATION: Starting comprehensive debugging");
  XELOGI("   with_presentation: {}", with_presentation);
  XELOGI("   provider_ valid: {}", provider_ ? "YES" : "NO");
  XELOGI("   app_context_ valid: {}", app_context_ ? "YES" : "NO");

  if (with_presentation && provider_) {
    // Safe if either the UI thread call or the presenter creation fails.
    if (app_context_) {
      XELOGI("DEBUG: PRESENTER: All requirements met, proceeding with creation");

      // Verify app_context is actually functional
      XELOGI("DEBUG: Testing app_context functionality");
      XELOGI("   app_context_ address: {}", static_cast<void*>(app_context_));

      auto presenter_created_event =
          xe::threading::Event::CreateAutoResetEvent(false);
      XELOGI("DEBUG: Created presenter_created_event at {}",
             static_cast<void*>(presenter_created_event.get()));

      // Use a shared pointer to ensure event stays alive if we time out
      auto shared_event = std::shared_ptr<xe::threading::Event>(
          std::move(presenter_created_event));
      XELOGI("DEBUG: Event use_count after move: {}",
             shared_event.use_count());

      XELOGI("DEBUG: About to call UI thread for presenter creation");
      XELOGI("   app_context_: {}", static_cast<void*>(app_context_));
      XELOGI("   provider_: {}", static_cast<void*>(provider_.get()));

      // Check if we're on UI thread already
      bool is_ui_thread = app_context_->IsInUIThread();
      XELOGI("DEBUG: IsOnUIThread() returns: {}", is_ui_thread);

#if defined(__APPLE__)
      // On macOS, use asynchronous call to avoid potential deadlock with GTK
      XELOGI("DEBUG: Using async UI thread call (macOS)");
      XELOGI("DEBUG: UI THREAD: About to call CallInUIThread");

      // Add pre-call debugging
      XELOGI("DEBUG: PRE-CALL: Current thread ID: {}", std::this_thread::get_id());
      XELOGI("DEBUG: PRE-CALL: Event shared_ptr use_count: {}",
             shared_event.use_count());
      XELOGI("DEBUG: PRE-CALL: this pointer: {}", static_cast<void*>(this));

      bool callback_queued = app_context_->CallInUIThreadTagged(
          "GraphicsSystem::CreatePresenter", [this, shared_event]() {
            XELOGI("DEBUG: UI THREAD CALLBACK: Entry point reached!");
            XELOGI("DEBUG: UI THREAD: Thread ID: {}", std::this_thread::get_id());
            XELOGI("DEBUG: UI THREAD: this pointer: {}", static_cast<void*>(this));
            XELOGI("DEBUG: UI THREAD: shared_event use_count: {}",
                   shared_event.use_count());
            XELOGI("DEBUG: UI THREAD: IsInUIThread() = {}",
                   app_context_->IsInUIThread());

            // Test event functionality
            XELOGI("DEBUG: UI THREAD: Testing event state before creation");
            bool is_signaled =
                xe::threading::Wait(shared_event.get(), false,
                                    std::chrono::milliseconds(0)) ==
                xe::threading::WaitResult::kSuccess;
            XELOGI("DEBUG: UI THREAD: Event signaled immediately: {}", is_signaled);
            try {
              XELOGI("DEBUG: UI THREAD: Callback entered successfully");
              XELOGI("DEBUG: UI Thread callback executing");
              XELOGI("   this pointer: {}", static_cast<void*>(this));
              XELOGI("   shared_event use_count: {}", shared_event.use_count());


              XELOGI("DEBUG: About to call provider_->CreatePresenter()");
              presenter_ = provider_->CreatePresenter(
                  [this](bool is_responsible, bool statically_from_ui_thread) {
                    XELOGI(
                        "DEBUG: Host GPU loss callback invoked - "
                        "is_responsible: {}, statically_from_ui_thread: {}",
                        is_responsible, statically_from_ui_thread);
                    OnHostGpuLossFromAnyThread(is_responsible);
                  });

              XELOGI("DEBUG: UI THREAD: provider_->CreatePresenter() completed");
              XELOGI("[graphics_system] IN UI THREAD: presenter created: {}",
                     static_cast<void*>(presenter_.get()));

              XELOGI("DEBUG: About to set event");
              shared_event->Set();
              XELOGI(
                  "DEBUG: UI THREAD: Event set, callback completed successfully");

            } catch (const std::exception& e) {
              XELOGE("DEBUG: UI THREAD: Exception during presenter creation: {}",
                     e.what());
              shared_event
                  ->Set();  // Still set the event to avoid infinite timeout
            } catch (...) {
              XELOGE(
                  "DEBUG: UI THREAD: Unknown exception during presenter creation");
              shared_event
                  ->Set();  // Still set the event to avoid infinite timeout
            }
          });

      XELOGI("DEBUG: POST-CALL ANALYSIS: CallInUIThread returned {}",
             callback_queued ? "TRUE" : "FALSE");
      XELOGI("DEBUG: POST-CALL: Current thread ID: {}", std::this_thread::get_id());
      XELOGI("DEBUG: POST-CALL: Event shared_ptr use_count: {}",
             shared_event.use_count());
      XELOGI("DEBUG: POST-CALL: Event pointer: {}",
             static_cast<void*>(shared_event.get()));

      if (!callback_queued) {
        XELOGE("DEBUG: FAILED to queue UI thread callback");

        // Emergency debugging - try to understand app_context state
        XELOGI("DEBUG: EMERGENCY: app_context_ still valid: {}",
               app_context_ ? "YES" : "NO");
        if (app_context_) {
          XELOGI("DEBUG: EMERGENCY: Testing app_context state");
          XELOGI("DEBUG: EMERGENCY: app_context address: {}",
                 static_cast<void*>(app_context_));
        }
      } else {
        XELOGI("DEBUG: UI thread callback queued successfully");
        XELOGI("DEBUG: Waiting for UI thread callback to execute...");
      }

      // Immediately test if event gets signaled (to detect immediate callback
      // execution)
      XELOGI("DEBUG: IMMEDIATE CHECK: Testing if callback already executed");
      auto immediate_result = xe::threading::Wait(
          shared_event.get(), false, std::chrono::milliseconds(100));
      XELOGI("DEBUG: IMMEDIATE CHECK: Event wait result: {}",
             static_cast<int>(immediate_result));
#else
      app_context_->CallInUIThreadSynchronous([this, shared_event]() {
        presenter_ = provider_->CreatePresenter(
            [this](bool is_responsible, bool statically_from_ui_thread) {
              OnHostGpuLossFromAnyThread(is_responsible);
            });
        XELOGI("[graphics_system] IN UI THREAD: presenter created: {}",
               static_cast<void*>(presenter_.get()));
        shared_event->Set();
      });
#endif

#if defined(__APPLE__)
      // On macOS, wait for presenter creation to complete with a generous
      // timeout

      // Try waiting with shorter timeout first, then retry if needed
      constexpr int MAX_RETRIES = 2;
      constexpr std::chrono::milliseconds INITIAL_TIMEOUT_MS(
          6000);                                                   // 6 seconds
      constexpr std::chrono::milliseconds RETRY_TIMEOUT_MS(4000);  // 4 seconds
      constexpr std::chrono::milliseconds NUDGE_GRACE_MS(250);

      auto wait_result = xe::threading::WaitResult::kTimeout;
      int retry_count = 0;

      do {
        XELOGI("DEBUG: Presenter creation attempt {}/{}", retry_count + 1,
               MAX_RETRIES + 1);
        auto current_timeout =
            (retry_count == 0) ? INITIAL_TIMEOUT_MS : RETRY_TIMEOUT_MS;
        wait_result =
            xe::threading::Wait(shared_event.get(), false, current_timeout);

        if (wait_result == xe::threading::WaitResult::kSuccess) {
          XELOGI("DEBUG: Presenter creation completed successfully on attempt {}",
                 retry_count + 1);
          break;
        } else if (wait_result == xe::threading::WaitResult::kTimeout) {
          if (presenter_) {
            XELOGI("DEBUG: Presenter created despite timeout on attempt {}",
                   retry_count + 1);
            wait_result = xe::threading::WaitResult::kSuccess;
            break;
          }

          // Nudge the UI loop in case pending work is stuck behind an event
          if (app_context_) {
            XELOGI("DEBUG: Nudging UI loop after presenter creation timeout");
            bool nudge_queued = app_context_->CallInUIThreadDeferredTagged(
                "GraphicsSystem::PresenterNudge", []() {});
            XELOGI("DEBUG: UI loop nudge queued: {}", nudge_queued ? "YES" : "NO");
            std::this_thread::sleep_for(NUDGE_GRACE_MS);
            if (presenter_) {
              XELOGI("DEBUG: Presenter created after UI loop nudge");
              wait_result = xe::threading::WaitResult::kSuccess;
              break;
            }
          }

          XELOGW("DEBUG: Presenter creation timeout on attempt {}/{}",
                 retry_count + 1, MAX_RETRIES + 1);
          if (retry_count < MAX_RETRIES) {
            XELOGI("DEBUG: Retrying presenter creation... ({}/{} attempts)",
                   retry_count + 2, MAX_RETRIES + 1);

            // Check if UI callback was actually queued but taking too long
            // Try to create a new event and attempt the UI thread call again
            auto retry_event =
                xe::threading::Event::CreateAutoResetEvent(false);
            auto shared_retry_event =
                std::shared_ptr<xe::threading::Event>(std::move(retry_event));

            bool retry_callback_queued = app_context_->CallInUIThreadTagged(
                "GraphicsSystem::CreatePresenterRetry",
                [this, shared_retry_event, retry_count]() {
                  try {
                    XELOGI(
                        "DEBUG: RETRY UI THREAD: Creating presenter (attempt "
                        "{})...",
                        retry_count + 2);
                    presenter_ = provider_->CreatePresenter(
                        [this](bool is_responsible,
                               bool statically_from_ui_thread) {
                          OnHostGpuLossFromAnyThread(is_responsible);
                        });
                    XELOGI("DEBUG: RETRY UI THREAD: Presenter created: {}",
                           static_cast<void*>(presenter_.get()));
                    shared_retry_event->Set();
                  } catch (const std::exception& e) {
                    XELOGE("DEBUG: RETRY UI THREAD: Exception: {}", e.what());
                    shared_retry_event->Set();
                  } catch (...) {
                    XELOGE("DEBUG: RETRY UI THREAD: Unknown exception");
                    shared_retry_event->Set();
                  }
                });

            if (retry_callback_queued) {
              // Wait for retry with shorter timeout
              auto retry_wait_result = xe::threading::Wait(
                  shared_retry_event.get(), false, RETRY_TIMEOUT_MS);
              if (retry_wait_result == xe::threading::WaitResult::kSuccess &&
                  presenter_) {
                wait_result = xe::threading::WaitResult::kSuccess;
                break;
              }
            } else {
              XELOGE("DEBUG: Failed to queue retry UI thread callback");
            }
          }
        } else {
          XELOGE("DEBUG: Unexpected wait result: {}",
                 static_cast<int>(wait_result));
          break;
        }

        retry_count++;
      } while (retry_count <= MAX_RETRIES);

      if (wait_result == xe::threading::WaitResult::kTimeout) {
        XELOGE(
            "DEBUG: TIMEOUT waiting for presenter creation in UI thread - "
            "attempting fallback");

        // Fallback: Try synchronous presenter creation on current thread
        try {
          XELOGI(
              "DEBUG: FALLBACK: Creating presenter synchronously on current "
              "thread");
          presenter_ = provider_->CreatePresenter(
              [this](bool is_responsible, bool statically_from_ui_thread) {
                XELOGI(
                    "DEBUG: FALLBACK: Host GPU loss callback invoked - "
                    "is_responsible: {}, statically_from_ui_thread: {}",
                    is_responsible, statically_from_ui_thread);
                OnHostGpuLossFromAnyThread(is_responsible);
              });

          if (presenter_) {
            XELOGI("DEBUG: FALLBACK: Presenter created successfully: {}",
                   static_cast<void*>(presenter_.get()));
          } else {
            XELOGE("DEBUG: FALLBACK: Presenter creation returned nullptr");
          }
        } catch (const std::exception& e) {
          XELOGE("DEBUG: FALLBACK: Exception during presenter creation: {}",
                 e.what());
        } catch (...) {
          XELOGE("DEBUG: FALLBACK: Unknown exception during presenter creation");
        }

      } else if (wait_result == xe::threading::WaitResult::kSuccess) {
        XELOGI("DEBUG: Presenter creation completed successfully via UI thread");
      } else {
        XELOGE("DEBUG: Unexpected wait result: {}", static_cast<int>(wait_result));
      }
#endif
    } else {
      // May be needed for offscreen use, such as capturing the guest output
      // image.
      presenter_ = provider_->CreatePresenter(
          [this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
      XELOGI("[graphics_system] Presenter created: {}",
             static_cast<void*>(presenter_.get()));
    }
  }

  // Create command processor (but do not start worker thread yet)
  XELOGI(
      "DEBUG: About to create GPU Commands worker thread (handle 01000010)");
  XELOGI("   This is the critical step where GPU Commands thread is created");
  XELOGI("   Graphics system: {}", static_cast<void*>(this));
  XELOGI("   Kernel state: {}", static_cast<void*>(kernel_state_));

  command_processor_ = CreateCommandProcessor();
  XELOGI("DEBUG: CommandProcessor object created");
  XELOGI("DEBUG: Command processor created: {}",
         static_cast<void*>(command_processor_.get()));
  XELOGI(
      "   Next step: Proactive ring buffer initialization before worker thread "
      "starts");

  if (cvars::gpu_compat_proactive_bootstrap) {
    if (cvars::gpu_no_force_run_profile) {
      static std::atomic<uint64_t> preworker_proactive_suppressed_last_log_ms{
          0};
      LogNoForceProfileSuppressed(
          "PROACTIVE bootstrap (pre-worker)", 0,
          Clock::QueryHostUptimeMillis(),
          &preworker_proactive_suppressed_last_log_ms);
    } else {
    // PROACTIVE RING BUFFER INITIALIZATION (before worker thread starts)
    // Compatibility-only mode for debugging titles with no guest bootstrap.
    XELOGI("DEBUG: PROACTIVE INIT: Compatibility mode enabled (pre-worker)");
    try {
      const uint32_t DEFAULT_RB_SIZE_LOG2 = 18;
      const uint32_t DEFAULT_RB_SIZE = uint32_t(1)
                                       << (DEFAULT_RB_SIZE_LOG2 + 3);
      XELOGI("   Allocating {} MB ring buffer (size_log2={}, size_bytes={})...",
             DEFAULT_RB_SIZE / (1024 * 1024), DEFAULT_RB_SIZE_LOG2,
             DEFAULT_RB_SIZE);
      uint32_t rb_ptr = kernel_state_->memory()->SystemHeapAlloc(
          DEFAULT_RB_SIZE, 4096, kSystemHeapPhysical);
      if (rb_ptr != 0) {
        XELOGI("   Ring buffer allocated at physical address: {:08X}", rb_ptr);
        command_processor_->InitializeRingBuffer(rb_ptr, DEFAULT_RB_SIZE_LOG2);
        XELOGI("DEBUG: PROACTIVE INIT: Ring buffer initialized successfully");
      } else {
        XELOGE("DEBUG: PROACTIVE INIT: Failed to allocate ring buffer memory");
      }
    } catch (const std::exception& e) {
      XELOGE("DEBUG: PROACTIVE INIT: Exception during ring buffer setup: {}",
             e.what());
    }
    }
  } else if (cvars::gpu_prelaunch_bootstrap_nudge_in_setup) {
    auto rb_state_before = command_processor_->GetRingBufferDebugState();
    bool rb_ready_before = rb_state_before.ring_buffer_initialized &&
                           rb_state_before.primary_buffer_ptr != 0 &&
                           rb_state_before.primary_buffer_size != 0;
    if (!rb_ready_before) {
      uint64_t now_ms = Clock::QueryHostUptimeMillis();
      ForceFallbackDelaySnapshot delay_snapshot;
      bool defer_force_fallback =
          ShouldDeferForceFallback(kernel_state_, &delay_snapshot);
      if (defer_force_fallback) {
        static std::atomic<uint64_t> preworker_force_deferred_last_log_ms{0};
        LogForceFallbackDeferred("PRE-WORKER bootstrap nudge", 0, now_ms,
                                 delay_snapshot,
                                 &preworker_force_deferred_last_log_ms);
      } else {
        XELOGW(
            "DEBUG: PRE-WORKER bootstrap nudge: forcing fallback init before "
            "worker startup (ring_init={} rb_base={:08X} rb_size={:08X})",
            rb_state_before.ring_buffer_initialized ? "YES" : "NO",
            rb_state_before.primary_buffer_ptr, rb_state_before.primary_buffer_size);
        command_processor_->ForceFallbackInitialization();
        auto rb_state_after = command_processor_->GetRingBufferDebugState();
        XELOGI(
            "DEBUG: PRE-WORKER bootstrap result: ring_init={} init_ack={} "
            "rb_base={:08X} rb_size={:08X}",
            rb_state_after.ring_buffer_initialized ? "YES" : "NO",
            rb_state_after.initialization_acknowledged ? "YES" : "NO",
            rb_state_after.primary_buffer_ptr, rb_state_after.primary_buffer_size);
      }
    } else {
      XELOGI(
          "DEBUG: PRE-WORKER bootstrap nudge not needed: ring already ready "
          "(rb_base={:08X} rb_size={:08X})",
          rb_state_before.primary_buffer_ptr, rb_state_before.primary_buffer_size);
    }
  } else if (cvars::trace_gpu_bootstrap) {
    XELOGI("DEBUG: Bootstrap trace: proactive pre-worker ring init disabled");
  }

  constexpr uint32_t kStartupSystemCommandBufferMinSize = 0x2000;
  XELOGI(
      "DEBUG: Startup gate: ensuring system command buffer is ready before GPU "
      "worker loop #1 (min_size=0x{:X})",
      kStartupSystemCommandBufferMinSize);
  if (!EnsureSystemCommandBufferReady(
          kStartupSystemCommandBufferMinSize,
          "GraphicsSystem::Setup.pre_worker_loop_gate")) {
    XELOGE(
        "DEBUG: Startup gate failed: system command buffer is not ready before "
        "GPU worker startup");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Now start the worker thread (Initialize)
  fflush(stderr);
  XELOGI("DEBUG: About to call CommandProcessor::Initialize()");
  XELOGI("   This is where handle 01000010 gets created and registered");
  XELOGI("   The XHostThread will be created inside this call");

  if (!command_processor_->Initialize()) {
    XELOGE("Unable to initialize command processor");
    XELOGI("DEBUG: CommandProcessor::Initialize() FAILED!");
    XELOGI("   Handle 01000010 was NOT created successfully");
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGI("DEBUG: CommandProcessor::Initialize() SUCCEEDED!");

  bool startup_ring_handshake_gate_enabled =
      cvars::gpu_force_ring_init_ack_on_startup ||
      cvars::gpu_suspend_nonessential_until_ring_init_ack;
  if (startup_ring_handshake_gate_enabled && command_processor_) {
    auto has_bootstrap_activity = [this]() -> bool {
      return vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed) !=
           0 ||
         vd_set_interrupt_callback_state_.count.load(
           std::memory_order_relaxed) != 0 ||
         vd_swap_state_.count.load(std::memory_order_relaxed) != 0 ||
         mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed) != 0 ||
         mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed) != 0 ||
         mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed) != 0;
    };

    auto startup_state = command_processor_->GetRingBufferDebugState();
    if (!startup_state.ring_buffer_initialized ||
        startup_state.primary_buffer_ptr == 0) {
      uint32_t size_log2 = cvars::gpu_force_init_ring_buffer_size_log2;
      uint32_t size_bytes = uint32_t(1) << (size_log2 + 3);
      if (!forced_ring_buffer_ptr_) {
        forced_ring_buffer_ptr_ = kernel_state_->memory()->SystemHeapAlloc(
            size_bytes, 4096, kSystemHeapPhysical);
        forced_ring_buffer_size_log2_ = size_log2;
      }
      if (forced_ring_buffer_ptr_) {
        XELOGW(
            "RING BUFFER: startup ring-init gate allocating/using ring buffer "
            "ptr={:08X} size_log2={} bytes={} (force_cvar={} gate_cvar={})",
            forced_ring_buffer_ptr_, forced_ring_buffer_size_log2_, size_bytes,
            cvars::gpu_force_ring_init_ack_on_startup ? "ON" : "OFF",
            cvars::gpu_suspend_nonessential_until_ring_init_ack ? "ON" : "OFF");
        command_processor_->InitializeRingBuffer(forced_ring_buffer_ptr_,
                                                 forced_ring_buffer_size_log2_);

        constexpr int kBootstrapActivityWaitMs = 250;
        constexpr int kAckWaitAfterBootstrapMs = 250;
        bool startup_handshake_ready = false;
        bool startup_bootstrap_activity_seen = has_bootstrap_activity();
        CommandProcessor::RingBufferDebugState ack_state =
            command_processor_->GetRingBufferDebugState();

        for (int i = 0; i < kBootstrapActivityWaitMs; ++i) {
          ack_state = command_processor_->GetRingBufferDebugState();
          if (ack_state.ring_buffer_initialized &&
              ack_state.initialization_acknowledged) {
            startup_handshake_ready = true;
            XELOGW(
                "RING BUFFER: startup ring-init gate handshake ready "
                "(ring_init=YES init_ack=YES rb_base={:08X} rb_size={:08X})",
                ack_state.primary_buffer_ptr, ack_state.primary_buffer_size);
            break;
          }
          startup_bootstrap_activity_seen =
              startup_bootstrap_activity_seen || has_bootstrap_activity();
          if (startup_bootstrap_activity_seen) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (!startup_handshake_ready && startup_bootstrap_activity_seen) {
          for (int i = 0; i < kAckWaitAfterBootstrapMs; ++i) {
            ack_state = command_processor_->GetRingBufferDebugState();
            if (ack_state.ring_buffer_initialized &&
                ack_state.initialization_acknowledged) {
              startup_handshake_ready = true;
              XELOGW(
                  "RING BUFFER: startup ring-init gate handshake ready "
                  "(ring_init=YES init_ack=YES rb_base={:08X} rb_size={:08X})",
                  ack_state.primary_buffer_ptr, ack_state.primary_buffer_size);
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }

        if (!startup_handshake_ready) {
          if (!startup_bootstrap_activity_seen) {
            uint64_t vd_init_count =
                vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
            uint64_t vd_cb_count = vd_set_interrupt_callback_state_.count.load(
                std::memory_order_relaxed);
            uint64_t vd_swap_count =
                vd_swap_state_.count.load(std::memory_order_relaxed);
            uint64_t mmio_rb_base_writes =
                mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
            uint64_t mmio_rb_cntl_writes =
                mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
            uint64_t mmio_rb_wptr_writes =
                mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
            XELOGW(
                "RING BUFFER: startup ring-init gate ack wait deferred "
                "(pre-bootstrap: ring_init={} init_ack={} rb_base={:08X} "
                "rb_size={:08X} vd_init={} vd_cb={} vd_swap={} "
                "mmio_rb_base={} mmio_rb_cntl={} mmio_rb_wptr={})",
                ack_state.ring_buffer_initialized ? "YES" : "NO",
                ack_state.initialization_acknowledged ? "YES" : "NO",
                ack_state.primary_buffer_ptr, ack_state.primary_buffer_size,
                vd_init_count, vd_cb_count, vd_swap_count, mmio_rb_base_writes,
                mmio_rb_cntl_writes, mmio_rb_wptr_writes);
          } else {
            XELOGW(
                "RING BUFFER: startup ring-init gate ack wait timeout "
                "(ring_init={} init_ack={} rb_base={:08X} rb_size={:08X})",
                ack_state.ring_buffer_initialized ? "YES" : "NO",
                ack_state.initialization_acknowledged ? "YES" : "NO",
                ack_state.primary_buffer_ptr, ack_state.primary_buffer_size);
          }
        }
      } else {
        XELOGE(
            "RING BUFFER: startup ring-init gate failed to allocate ring buffer "
            "(size_log2={} bytes={})",
            size_log2, size_bytes);
      }
    } else if (!startup_state.initialization_acknowledged) {
      if (has_bootstrap_activity()) {
        XELOGW(
            "RING BUFFER: startup ring already initialized but ack missing; "
            "worker should acknowledge shortly (rb_base={:08X} rb_size={:08X})",
            startup_state.primary_buffer_ptr, startup_state.primary_buffer_size);
      } else {
        XELOGW(
            "RING BUFFER: startup ring initialized pre-bootstrap; ack gate "
            "is deferred until guest bootstrap activity is observed "
            "(rb_base={:08X} rb_size={:08X})",
            startup_state.primary_buffer_ptr, startup_state.primary_buffer_size);
      }
    } else {
      XELOGI(
          "RING BUFFER: startup ring handshake already ready "
          "(ring_init=YES init_ack=YES rb_base={:08X} rb_size={:08X})",
          startup_state.primary_buffer_ptr, startup_state.primary_buffer_size);
    }
  }

  // Verify GPU Commands thread is accessible
  auto* gpu_thread = kernel::XThread::GetCurrentThread();
  if (gpu_thread && gpu_thread->handle() == 0x01000010) {
    XELOGI(
        "DEBUG: GPU Commands thread (01000010) verified and accessible in Setup "
        "context");
  } else {
    auto gpu_thread_by_id = kernel_state_->GetThreadByID(1);
    if (gpu_thread_by_id && gpu_thread_by_id->handle() == 0x01000010) {
      XELOGI(
          "DEBUG: GPU Commands thread exists (handle=01000010, thread_id=1) but "
          "is not the current thread (current handle={:08X})",
          gpu_thread ? gpu_thread->handle() : 0);
    } else {
      XELOGW(
          "DEBUG: WARNING: GPU Commands thread not accessible via GetCurrentThread "
          "or GetThreadByID(1)");
    }
  }

  // Let the processor know we want register access callbacks.
  XELOGI(
      "DEBUG: BREADCRUMB: GraphicsSystem registering MMIO range 0x7FC80000/0xFFFF");
  bool mmio_ok = memory_->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<cpu::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<cpu::MMIOWriteCallback>(WriteRegisterThunk));
  XELOGI("DEBUG: BREADCRUMB: GraphicsSystem MMIO range registered result={}",
         mmio_ok ? "OK" : "FAIL");
    bool cp_rb_base_handler_registered =
      memory_->LookupVirtualMappedRange(0x7FC80700u) != nullptr;
    XELOGI(
      "DEBUG: MMIO handler probe CP_RB_BASE addr={:08X} handler_registered={}",
      0x7FC80700u, cp_rb_base_handler_registered ? "YES" : "NO");
    if (!cp_rb_base_handler_registered) {
      XELOGE(
          "DEBUG: MMIO setup failed: CP_RB_BASE write handler is not registered; "
          "cannot continue GPU bootstrap safely");
      return X_STATUS_UNSUCCESSFUL;
    }

  XELOGI(
      "DEBUG: GPU bootstrap cvars: gpu_compat_proactive_bootstrap={} "
      "gpu_force_init_if_missing={} "
      "gpu_suspend_nonessential_until_ring_init_ack={} "
      "gpu_suspend_force_fallback_until_module_ready={} "
      "gpu_no_force_run_profile={} "
      "gpu_prelaunch_suspend_during_module_load={}",
      cvars::gpu_compat_proactive_bootstrap ? "ON" : "OFF",
      cvars::gpu_force_init_if_missing ? "ON" : "OFF",
      cvars::gpu_suspend_nonessential_until_ring_init_ack ? "ON" : "OFF",
      cvars::gpu_suspend_force_fallback_until_module_ready ? "ON" : "OFF",
      cvars::gpu_no_force_run_profile ? "ON" : "OFF",
      cvars::gpu_prelaunch_suspend_during_module_load ? "ON" : "OFF");

  // Ensure the MMIO callback page is backed with committed RW memory so
  // host-side bootstrap probes don't report false uncommitted anomalies.
  {
    constexpr uint32_t kGpuMmioPageBase = 0x7FC80000u;
    constexpr uint32_t kGpuMmioPageSize = 0x00010000u;
    auto* mmio_heap = memory_->LookupHeap(kGpuMmioPageBase);
    if (!mmio_heap) {
      void* mmio_host = memory_->TranslateVirtual(kGpuMmioPageBase);
      bool host_commit_ok = false;
      if (mmio_host) {
        host_commit_ok =
            xe::memory::AllocFixed(mmio_host, kGpuMmioPageSize,
                                   xe::memory::AllocationType::kCommit,
                                   xe::memory::PageAccess::kNoAccess) != nullptr;
      }
      XELOGW(
          "DEBUG: MMIO page ensure (host fallback) addr={:08X} host={} "
          "result={} mapped_range={}",
          kGpuMmioPageBase, mmio_host, host_commit_ok ? "OK" : "FAIL",
          memory_->LookupVirtualMappedRange(kGpuMmioPageBase) ? "YES" : "NO");
    } else {
      xe::HeapAllocationInfo mmio_info{};
      bool has_region_info =
          mmio_heap->QueryRegionInfo(kGpuMmioPageBase, &mmio_info);
      bool committed =
          has_region_info && (mmio_info.state & xe::kMemoryAllocationCommit) != 0;
      if (!committed) {
        bool alloc_ok = mmio_heap->AllocFixed(
            kGpuMmioPageBase, kGpuMmioPageSize, 0,
            xe::kMemoryAllocationCommit,
            xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
        if (!alloc_ok) {
          uint32_t mmio_page_size = std::max<uint32_t>(mmio_heap->page_size(), 4096);
          std::array<uint32_t, 3> critical_mmio_addrs = {
              0x7FC80700u, 0x7FC80704u, 0x7FC80714u};
          for (uint32_t mmio_addr : critical_mmio_addrs) {
            uint32_t mmio_page = mmio_addr & ~(mmio_page_size - 1);
            if (mmio_heap->AllocFixed(
                    mmio_page, mmio_page_size, mmio_page_size,
                    xe::kMemoryAllocationCommit,
                    xe::kMemoryProtectRead | xe::kMemoryProtectWrite)) {
              alloc_ok = true;
            }
          }
        }
        XELOGW(
            "DEBUG: MMIO page ensure commit addr={:08X} size={:08X} result={}",
            kGpuMmioPageBase, kGpuMmioPageSize, alloc_ok ? "OK" : "FAIL");
      }

      has_region_info = mmio_heap->QueryRegionInfo(kGpuMmioPageBase, &mmio_info);
      XELOGI(
          "DEBUG: MMIO page state addr={:08X} committed={} protect=0x{:X} "
          "heap_type={}",
          kGpuMmioPageBase,
          (has_region_info &&
           (mmio_info.state & xe::kMemoryAllocationCommit) != 0)
              ? "YES"
              : "NO",
          has_region_info ? mmio_info.protect : 0,
          static_cast<int>(mmio_heap->heap_type()));
    }
  }

  // Keep MMIO pages committed and explicitly readable/writable while MMIO
  // callbacks are still dispatched via LookupVirtualMappedRange fast-paths.
  auto ensure_mmio_rw = [&](uint32_t addr, const char* name) {
    void* host_ptr = memory_->TranslateVirtual(addr);
    if (!host_ptr) {
      XELOGW(
          "DEBUG: MMIO RW probe {} addr={:08X}: TranslateVirtual=null; cannot "
          "query/protect",
          name ? name : "<unknown>", addr);
      return;
    }
    xe::memory::PageAccess access = xe::memory::PageAccess::kNoAccess;
    size_t page_len = xe::memory::page_size();
    uintptr_t host_addr = reinterpret_cast<uintptr_t>(host_ptr);
    uintptr_t host_page_addr = host_addr;
    if (page_len != 0) {
      host_page_addr &= ~(static_cast<uintptr_t>(page_len) - 1);
    }
    void* host_page_ptr = reinterpret_cast<void*>(host_page_addr);
    xe::memory::QueryProtect(host_page_ptr, page_len, access);
    XELOGI(
        "DEBUG: MMIO RW probe {} addr={:08X}: host={} host_page={} page_access={}",
        name ? name : "<unknown>", addr, host_ptr, host_page_ptr,
        access == xe::memory::PageAccess::kNoAccess
            ? "NO_ACCESS"
            : (access == xe::memory::PageAccess::kReadOnly
                   ? "READ_ONLY"
                   : (access == xe::memory::PageAccess::kReadWrite
                          ? "READ_WRITE"
                          : "OTHER")));
    if (access != xe::memory::PageAccess::kReadWrite) {
      bool protect_ok = xe::memory::Protect(
          host_page_ptr, page_len, xe::memory::PageAccess::kReadWrite);
      XELOGI(
          "DEBUG: MMIO RW probe {} addr={:08X}: set page_access=READ_WRITE "
          "host_page={} result={}",
          name ? name : "<unknown>", addr, host_page_ptr,
          protect_ok ? "OK" : "FAIL");
    }

    auto* heap = memory_->LookupHeap(addr);
    if (heap) {
      xe::HeapAllocationInfo info{};
      if (heap->QueryRegionInfo(addr, &info)) {
        bool committed = (info.state & xe::kMemoryAllocationCommit) != 0;
        bool read_write =
            (info.protect & (xe::kMemoryProtectRead | xe::kMemoryProtectWrite)) ==
            (xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
        XELOGI(
            "DEBUG: MMIO RW probe {} addr={:08X}: committed={} protect=0x{:X} "
            "vm_prot_read_write={}",
            name ? name : "<unknown>", addr, committed ? "YES" : "NO",
            info.protect, read_write ? "YES" : "NO");
      }
    }
  };
  ensure_mmio_rw(0x7FC80700u, "CP_RB_BASE");
  ensure_mmio_rw(0x7FC80704u, "CP_RB_CNTL");
  ensure_mmio_rw(0x7FC80714u, "CP_RB_WPTR");

  // Frame limiter thread.
  frame_limiter_worker_running_ = true;
  frame_limiter_worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            uint64_t normalized_framerate_limit =
                std::max<uint64_t>(0, cvars::framerate_limit);

            // If VSYNC is enabled, but frames are not limited,
            // lock framerate at default value of 60
            if (normalized_framerate_limit == 0 && cvars::vsync)
              normalized_framerate_limit = 60;

            const double vsync_duration_d =
                cvars::vsync
                    ? std::max<double>(5.0,
                                       1000.0 / static_cast<double>(
                                                    normalized_framerate_limit))
                    : 1.0;
            uint64_t last_frame_time = Clock::QueryGuestTickCount();
            // Sleep for 90% of the vblank duration, spin for 10%
            constexpr double duration_scalar = 0.90;

            while (frame_limiter_worker_running_) {
              register_file()->values[XE_GPU_REG_D1MODE_V_COUNTER] +=
                  GetInternalDisplayResolution().second;

              if (cvars::vsync) {
                const uint64_t current_time = Clock::QueryGuestTickCount();
                const uint64_t tick_freq = Clock::guest_tick_frequency();
                const uint64_t time_delta = current_time - last_frame_time;
                const double elapsed_d =
                    static_cast<double>(time_delta) /
                    (static_cast<double>(tick_freq) / 1000.0);
                if (elapsed_d >= vsync_duration_d) {
                  last_frame_time = current_time;

                  // TODO(disjtqz): should recalculate the remaining time to a
                  // vblank after MarkVblank, no idea how long the guest code
                  // normally takes
                  MarkVblank();
                  if (cvars::vsync) {
                    const uint64_t estimated_nanoseconds =
                        static_cast<uint64_t>(
                            (vsync_duration_d * 1000000.0) *
                            duration_scalar);  // 1000 microseconds = 1 ms

                    threading::NanoSleep(estimated_nanoseconds);
                  }
                }
              }

              if (!cvars::vsync) {
                MarkVblank();
                if (normalized_framerate_limit > 0) {
                  // framerate_limit is over 0, vsync disabled
                  //  - No VSYNC + limited frames defined by user
                  uint64_t framerate_limited_sleep_time =
                      1000000000 / normalized_framerate_limit;
                  xe::threading::NanoSleep(framerate_limited_sleep_time);
                } else {
                  // framerate_limit is 0, vsync disabled
                  //  - No VSYNC + unlimited frames
                  xe::threading::Sleep(std::chrono::milliseconds(1));
                }
              }
            }
            return 0;
          },
          kernel_state->GetIdleProcess()));

  // As we run vblank interrupts the debugger must be able to suspend us.
  frame_limiter_worker_thread_->set_can_debugger_suspend(true);
  frame_limiter_worker_thread_->set_name("GPU Frame limiter");

  frame_limiter_worker_thread_->Create();

  frame_limiter_worker_thread_->thread()->set_priority(
      threading::ThreadPriority::kLowest);

  if (cvars::trace_gpu_stream) {
    BeginTracing();
  }

  if (cvars::gpu_compat_proactive_bootstrap) {
    if (cvars::gpu_no_force_run_profile) {
      static std::atomic<uint64_t> postworker_proactive_suppressed_last_log_ms{
          0};
      LogNoForceProfileSuppressed(
          "PROACTIVE bootstrap (post-worker)", 0,
          Clock::QueryHostUptimeMillis(),
          &postworker_proactive_suppressed_last_log_ms);
    } else {
    // PROACTIVE RING BUFFER INITIALIZATION (post-worker)
    if (command_processor_ && command_processor_->is_worker_thread_running()) {
      XELOGI("DEBUG: PROACTIVE INIT: Compatibility mode enabled (post-worker)");
      try {
        const uint32_t DEFAULT_RB_SIZE_LOG2 = 18;
        const uint32_t DEFAULT_RB_SIZE = uint32_t(1)
                                         << (DEFAULT_RB_SIZE_LOG2 + 3);

        XELOGI(
            "   Allocating {} MB ring buffer (size_log2={}, size_bytes={})...",
            DEFAULT_RB_SIZE / (1024 * 1024), DEFAULT_RB_SIZE_LOG2,
            DEFAULT_RB_SIZE);

        uint32_t rb_ptr = kernel_state_->memory()->SystemHeapAlloc(
            DEFAULT_RB_SIZE, 4096, kSystemHeapPhysical);

        if (rb_ptr != 0) {
          XELOGI("   Ring buffer allocated at physical address: {:08X}",
                 rb_ptr);
          command_processor_->InitializeRingBuffer(rb_ptr,
                                                   DEFAULT_RB_SIZE_LOG2);
          XELOGI("DEBUG: PROACTIVE INIT: Ring buffer initialized successfully");
        } else {
          XELOGE("DEBUG: PROACTIVE INIT: Failed to allocate ring buffer memory");
        }
      } catch (const std::exception& e) {
        XELOGE("DEBUG: PROACTIVE INIT: Exception during ring buffer setup: {}",
               e.what());
      }
    } else {
      XELOGW("DEBUG: PROACTIVE INIT: Command processor not ready");
    }
    }
  } else if (cvars::trace_gpu_bootstrap) {
    XELOGI("DEBUG: Bootstrap trace: proactive post-worker ring init disabled");
  }


  // Comprehensive graphics system verification
  XELOGI("===============================================================");
  XELOGI("DEBUG: GRAPHICS SYSTEM VERIFICATION");
  XELOGI("===============================================================");

  bool all_checks_passed = true;

  // Check 1: Provider initialized
  if (provider_) {
    XELOGI("DEBUG: Graphics Provider: INITIALIZED");
    XELOGI("   Provider type: {}", name());
  } else {
    XELOGE("DEBUG: Graphics Provider: FAILED");
    all_checks_passed = false;
  }

  // Check 2: Presenter created (if with_presentation)
  if (with_presentation) {
    if (presenter_) {
      XELOGI("DEBUG: Presenter: CREATED");
      XELOGI("   Presenter address: {}", static_cast<void*>(presenter_.get()));
    } else {
      XELOGE("DEBUG: Presenter: FAILED");
      all_checks_passed = false;
    }
  } else {
    XELOGI("DEBUG: Presenter: SKIPPED (headless mode)");
  }

  // Check 3: Command processor initialized
  if (command_processor_) {
    XELOGI("DEBUG: Command Processor: INITIALIZED");

    // Check GPU Commands thread
    auto* kernel_state = kernel_state_;
    if (kernel_state) {
      auto gpu_thread =
          kernel_state->GetThreadByID(1);  // GPU Commands thread is ID 1
      if (gpu_thread && gpu_thread->handle() == 0x01000010) {
        XELOGI("DEBUG: GPU Commands Thread (01000010): RUNNING");
        XELOGI("   Thread ID: {}", gpu_thread->thread_id());
      } else {
        XELOGW("DEBUG: GPU Commands Thread: NOT VERIFIED");
      }
    }
  } else {
    XELOGE("DEBUG: Command Processor: FAILED");
    all_checks_passed = false;
  }

  // Check 4: Ring buffer state
  uint32_t rb_ptr = command_processor_->primary_buffer_ptr();
  uint32_t rb_size = command_processor_->primary_buffer_size();
  if (rb_ptr != 0 && rb_size != 0) {
    XELOGI("DEBUG: Ring Buffer: ALLOCATED");
    XELOGI("   Physical address: {:08X}", rb_ptr);
    XELOGI("   Size: {} bytes ({} MB)", rb_size, rb_size / (1024 * 1024));
  } else {
    XELOGI("DEBUG: Ring Buffer: NOT YET ALLOCATED (will be initialized by game)");
  }

  // Check 5: Memory system
  if (memory_) {
    XELOGI("DEBUG: Memory System: CONNECTED");
  } else {
    XELOGE("DEBUG: Memory System: FAILED");
    all_checks_passed = false;
  }

  // Final status
  XELOGI("===============================================================");
  if (all_checks_passed) {
    XELOGI("DEBUG: GRAPHICS SYSTEM: OPERATIONAL");
    XELOGI("   All critical components initialized successfully");
    XELOGI("   Ready to process GPU commands and render frames");
  } else {
    XELOGE("DEBUG: GRAPHICS SYSTEM: FAILED");
    XELOGE("   One or more critical components failed to initialize");
  }
  XELOGI("===============================================================");

    if (cvars::gpu_prelaunch_bootstrap_nudge_in_setup && command_processor_) {
    uint64_t vd_init_ring_count =
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_set_cb_count =
      vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_swap_count = vd_swap_state_.count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_base_writes =
      mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_cntl_writes =
      mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_wptr_writes =
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
    bool bootstrap_activity_seen =
      vd_init_ring_count > 0 || vd_set_cb_count > 0 || vd_swap_count > 0 ||
      mmio_rb_base_writes > 0 || mmio_rb_cntl_writes > 0 ||
      mmio_rb_wptr_writes > 0;
    bool ring_initialized = command_processor_->IsRingBufferInitialized();

    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    ForceFallbackDelaySnapshot setup_delay_snapshot;
    bool defer_setup_force_fallback =
        ShouldDeferForceFallback(kernel_state_, &setup_delay_snapshot);
    auto ensure_probe_ring_configured = [&](const char* scope) {
      auto rb_state = command_processor_->GetRingBufferDebugState();
      bool ring_configured = rb_state.ring_buffer_initialized &&
                             rb_state.primary_buffer_ptr != 0 &&
                             rb_state.primary_buffer_size != 0;
      if (ring_configured ||
          !cvars::gpu_bootstrap_probe_prime_ring_if_missing) {
        return ring_configured;
      }
      uint32_t size_log2 = cvars::gpu_force_init_ring_buffer_size_log2;
      if (size_log2 == 0 || size_log2 > 24) {
        size_log2 = 18;
      }
      uint32_t size_bytes = uint32_t(1) << (size_log2 + 3);
      if (!forced_ring_buffer_ptr_ && kernel_state_) {
        forced_ring_buffer_ptr_ = kernel_state_->memory()->SystemHeapAlloc(
            size_bytes, 4096, kSystemHeapPhysical);
        forced_ring_buffer_size_log2_ = size_log2;
      }
      if (!forced_ring_buffer_ptr_) {
        XELOGW(
            "DEBUG: {} ring-prime skipped: failed to allocate ring buffer "
            "(size_log2={} bytes={})",
            scope ? scope : "setup", size_log2, size_bytes);
        return false;
      }
      XELOGW(
          "DEBUG: {} ring-prime: initializing ring for bootstrap probe "
          "(rb_base={:08X} size_log2={} bytes={})",
          scope ? scope : "setup", forced_ring_buffer_ptr_,
          forced_ring_buffer_size_log2_, size_bytes);
      command_processor_->InitializeRingBuffer(forced_ring_buffer_ptr_,
                                               forced_ring_buffer_size_log2_);
      rb_state = command_processor_->GetRingBufferDebugState();
      ring_configured = rb_state.ring_buffer_initialized &&
                        rb_state.primary_buffer_ptr != 0 &&
                        rb_state.primary_buffer_size != 0;
      XELOGI(
          "DEBUG: {} ring-prime result: ring_init={} init_ack={} rb_base={:08X} "
          "rb_size={:08X}",
          scope ? scope : "setup",
          rb_state.ring_buffer_initialized ? "YES" : "NO",
          rb_state.initialization_acknowledged ? "YES" : "NO",
          rb_state.primary_buffer_ptr, rb_state.primary_buffer_size);
      return ring_configured;
    };

    if (!bootstrap_activity_seen && defer_setup_force_fallback) {
      static std::atomic<uint64_t> setup_bootstrap_deferred_last_log_ms{0};
      LogForceFallbackDeferred("SETUP bootstrap nudge", 0, now_ms,
                               setup_delay_snapshot,
                               &setup_bootstrap_deferred_last_log_ms);
      if (cvars::gpu_no_force_run_profile) {
      bool setup_ring_configured = ensure_probe_ring_configured(
          "SETUP no-force bootstrap probe");
      if (setup_ring_configured &&
        !prelaunch_bootstrap_nudge_done_.exchange(
          true, std::memory_order_relaxed)) {
        uint64_t nudge_id =
          prelaunch_bootstrap_nudge_count_.fetch_add(
            1, std::memory_order_relaxed) +
          1;
        last_prelaunch_bootstrap_nudge_ms_.store(
          now_ms, std::memory_order_relaxed);
        XELOGW(
          "DEBUG: SETUP bootstrap no-force probe #{}: injecting guest-style "
          "MMIO ring activity (without loop-time forcing) because bootstrap "
          "activity is still zero",
          nudge_id);
        InjectGuestBootstrapDebugActivity("setup-no-force-bootstrap-probe",
                        0);
        TryAutoRecoverMissingInterruptCallback(
            now_ms, 0, "setup-no-force-bootstrap-probe");
      }
      }
    } else if (!bootstrap_activity_seen &&
               !prelaunch_bootstrap_nudge_done_.exchange(
                   true, std::memory_order_relaxed)) {
      uint64_t nudge_id =
        prelaunch_bootstrap_nudge_count_.fetch_add(1,
                             std::memory_order_relaxed) +
        1;
      last_prelaunch_bootstrap_nudge_ms_.store(now_ms,
                           std::memory_order_relaxed);
      XELOGW(
        "DEBUG: SETUP bootstrap nudge #{}: issuing immediate fallback init + "
        "guest bootstrap injection in setup section "
        "(ring_init={}, vd_init={}, vd_set_cb={}, vd_swap={}, "
        "mmio_rb_base={}, mmio_rb_cntl={}, mmio_rb_wptr={})",
        nudge_id, ring_initialized ? "YES" : "NO", vd_init_ring_count,
        vd_set_cb_count, vd_swap_count, mmio_rb_base_writes,
        mmio_rb_cntl_writes, mmio_rb_wptr_writes);
      command_processor_->ForceFallbackInitialization();
      InjectGuestBootstrapDebugActivity("setup-bootstrap-nudge", 0);
      if (interrupt_callback_ == 0) {
        XELOGW(
            "DEBUG: SETUP bootstrap nudge: interrupt callback still unset after "
            "ring/bootstrap injection; attempting fallback callback path");
        AttemptFallbackInitialization();
        if (interrupt_callback_ != 0) {
          XELOGI(
              "DEBUG: SETUP bootstrap nudge: fallback callback set to {:08X}",
              interrupt_callback_);
        } else {
          XELOGW(
              "DEBUG: SETUP bootstrap nudge: callback remains unset "
              "(gpu_allow_forced_interrupt_callback_fallback={})",
              cvars::gpu_allow_forced_interrupt_callback_fallback ? "true"
                                                                   : "false");
        }
      }
    } else if (!bootstrap_activity_seen) {
      XELOGI(
        "DEBUG: SETUP bootstrap nudge skipped: already issued earlier "
        "(nudge_count={})",
        prelaunch_bootstrap_nudge_count_.load(std::memory_order_relaxed));
    } else {
      XELOGI(
        "DEBUG: SETUP bootstrap nudge not needed: bootstrap activity already "
        "observed (vd_init={}, vd_set_cb={}, vd_swap={}, mmio_rb_base={}, "
        "mmio_rb_cntl={}, mmio_rb_wptr={})",
        vd_init_ring_count, vd_set_cb_count, vd_swap_count,
        mmio_rb_base_writes, mmio_rb_cntl_writes, mmio_rb_wptr_writes);
    }
    }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (frame_limiter_worker_thread_) {
    frame_limiter_worker_running_ = false;
    frame_limiter_worker_thread_->Wait(0, 0, 0, nullptr);
    frame_limiter_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
#if defined(__APPLE__)
      // On macOS, use asynchronous call to avoid potential deadlock with GTK
      app_context_->CallInUIThreadTagged("GraphicsSystem::ResetPresenterAsync",
                                         [this]() { presenter_.reset(); });
#else
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
#endif
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(
    [[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  xe::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context,
                                           GraphicsSystem* gs, uint32_t addr) {
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  if (cvars::trace_gpu_bootstrap) {
    static std::atomic<uint64_t> mmio_read_count{0};
    uint64_t count = mmio_read_count.fetch_add(1) + 1;
    uint32_t r = (addr & 0xFFFF) / 4;
    XELOGI("DEBUG: MMIO READ #{} addr=0x{:08X} reg={:04X} thread={}", count, addr,
           r, oss.str());
  }
  uint32_t reg = (addr & 0xFFFF) / 4;
  if (cvars::gpu_log_mmio_reads_cp_rb &&
      (reg == 0x01C0 || reg == 0x01C1 || reg == 0x01C3 || reg == 0x01C4 ||
       reg == 0x01C5 || reg == 0x01C6 || reg == 0x01C7)) {
    auto* ctx = reinterpret_cast<cpu::ppc::PPCContext*>(ppc_context);
    uint32_t pc = ctx ? ctx->current_pc : 0;
    XELOGI("DEBUG: MMIO READ(CP_RB_*) reg={:04X} pc={:08X} thread={}", reg, pc,
           oss.str());
  }
  else {
    XELOGI("DEBUG: MMIO READ reg={:04X} thread={}", reg, oss.str());
  }
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                        uint32_t addr, uint32_t value) {
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  if (cvars::trace_gpu_bootstrap) {
    static std::atomic<uint64_t> mmio_write_count{0};
    uint64_t count = mmio_write_count.fetch_add(1) + 1;
    uint32_t r = (addr & 0xFFFF) / 4;
    XELOGI("DEBUG: MMIO WRITE #{} addr=0x{:08X} reg={:04X} value={:08X} thread={}",
           count, addr, r, value, oss.str());
  }

  uint32_t reg = (addr & 0xFFFF) / 4;
  const char* bootstrap_reg_name = GetBootstrapMmioRegName(reg);
  bool is_cp_rb_reg =
      reg == 0x01C0 || reg == 0x01C1 || reg == 0x01C3 || reg == 0x01C4 ||
      reg == 0x01C5 || reg == 0x01C6 || reg == 0x01C7;
  bool is_fetch0_reg = reg >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
                       reg <= (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 5);
  uint64_t any_write_count =
      gs->mmio_any_write_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t expected_first_reg = 0xFFFFFFFFu;
  gs->mmio_any_first_reg_.compare_exchange_strong(
      expected_first_reg, reg, std::memory_order_relaxed);
  gs->mmio_any_last_reg_.store(reg, std::memory_order_relaxed);
  gs->mmio_any_last_value_.store(value, std::memory_order_relaxed);

  auto* ctx = reinterpret_cast<cpu::ppc::PPCContext*>(ppc_context);
  bool guest_origin = ctx != nullptr;
  uint32_t pc = ctx ? ctx->current_pc : 0;
  uint32_t thread_id = 0;
  if (auto* current_thread = kernel::XThread::GetCurrentThread()) {
    thread_id = current_thread->thread_id();
  } else {
    // Host-originated debug MMIO writes may execute without a bound XThread
    // TLS context. Keep telemetry alive without dereferencing guest KTHREAD.
    thread_id = xe::threading::current_thread_id();
  }
  gs->mmio_any_last_pc_.store(pc, std::memory_order_relaxed);
  gs->mmio_any_last_thread_id_.store(thread_id, std::memory_order_relaxed);
  if (!is_cp_rb_reg && !is_fetch0_reg) {
    gs->mmio_non_cp_write_count_.fetch_add(1, std::memory_order_relaxed);
  }
  if (is_cp_rb_reg) {
    if (guest_origin) {
      gs->mmio_cp_guest_write_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
      gs->mmio_cp_host_write_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  uint32_t raw_value = value;
  uint32_t normalized_value = value;
  if (is_cp_rb_reg) {
    CpMmioNormalizationResult normalization =
        NormalizeCpMmioWriteValue(reg, raw_value);
    bool apply_swap =
        cvars::gpu_mmio_cp_endian_autofix && normalization.swapped;
    if (apply_swap) {
      normalized_value = normalization.normalized_value;
      uint64_t fix_count = gs->mmio_cp_endian_autofix_count_.fetch_add(
                               1, std::memory_order_relaxed) +
                           1;
      gs->mmio_cp_endian_last_reg_.store(reg, std::memory_order_relaxed);
      gs->mmio_cp_endian_last_raw_value_.store(raw_value,
                                               std::memory_order_relaxed);
      gs->mmio_cp_endian_last_normalized_value_.store(
          normalized_value, std::memory_order_relaxed);
      XELOGW(
          "RING BUFFER: MMIO endian autofix applied reg={} raw={:08X} "
          "normalized={:08X} reason={} count={}",
          bootstrap_reg_name ? bootstrap_reg_name : "<unknown>", raw_value,
          normalized_value, normalization.reason ? normalization.reason : "raw",
          fix_count);
    } else if (cvars::gpu_mmio_cp_endian_log &&
               (any_write_count <= 8 || !normalization.raw_plausible ||
                !normalization.swapped_plausible)) {
      XELOGI(
          "RING BUFFER: MMIO endian check reg={} raw={:08X} "
          "raw_plausible={} swapped={:08X} swapped_plausible={} "
          "decision={}",
          bootstrap_reg_name ? bootstrap_reg_name : "<unknown>", raw_value,
          normalization.raw_plausible ? "YES" : "NO",
          normalization.swapped_value,
          normalization.swapped_plausible ? "YES" : "NO",
          apply_swap ? "SWAPPED" : "RAW");
    }
  }

  bool snapshot_cp_rb_base =
      reg == 0x01C0 &&
      gs->mmio_cp_rb_base_.count.load(std::memory_order_relaxed) == 0;
  bool snapshot_cp_rb_cntl =
      reg == 0x01C1 &&
      gs->mmio_cp_rb_cntl_.count.load(std::memory_order_relaxed) == 0;
  bool snapshot_cp_rb_wptr =
      reg == 0x01C5 &&
      gs->mmio_cp_rb_wptr_.count.load(std::memory_order_relaxed) == 0;
  switch (reg) {
    case 0x01C0:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_base_, normalized_value, pc,
                               thread_id, guest_origin,
                               "CP_RB_BASE");
      break;
    case 0x01C1:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_cntl_, normalized_value, pc,
                               thread_id, guest_origin,
                               "CP_RB_CNTL");
      break;
    case 0x01C3:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_rptr_addr_, normalized_value, pc,
                               thread_id, guest_origin, "CP_RB_RPTR_ADDR");
      break;
    case 0x01C4:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_rptr_, normalized_value, pc,
                               thread_id, guest_origin,
                               "CP_RB_RPTR");
      break;
    case 0x01C5:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_wptr_, normalized_value, pc,
                               thread_id, guest_origin,
                               "CP_RB_WPTR");
      break;
    case 0x01C6:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_wptr_delay_, normalized_value, pc,
                               thread_id, guest_origin, "CP_RB_WPTR_DELAY");
      break;
    case 0x01C7:
      gs->UpdateMmioWriteState(gs->mmio_cp_rb_rptr_wr_, normalized_value, pc,
                               thread_id, guest_origin, "CP_RB_RPTR_WR");
      break;
    default:
      break;
  }
  if (is_fetch0_reg) {
    gs->UpdateMmioWriteState(gs->mmio_fetch0_, normalized_value, pc, thread_id,
                             guest_origin,
                             "FETCH0_WORD");
  }
  if (snapshot_cp_rb_base || snapshot_cp_rb_cntl || snapshot_cp_rb_wptr) {
    gs->LogBootstrapMmioFirstWriteSnapshot(
        bootstrap_reg_name ? bootstrap_reg_name : "<unknown>", reg, addr,
      normalized_value, pc, thread_id, guest_origin);
  }
  if (cvars::trace_gpu_bootstrap && bootstrap_reg_name) {
    XELOGI(
        "DEBUG: GPU bootstrap MMIO dispatch: reg={} value={:08X} raw={:08X} "
        "pc={:08X} thread_id={} handler_reached=NO",
        bootstrap_reg_name, normalized_value, raw_value, pc, thread_id);
  }
  if (cvars::gpu_log_mmio_writes_cp_rb &&
      is_cp_rb_reg) {
    XELOGI(
        "DEBUG: MMIO WRITE(CP_RB_*) reg={:04X} value={:08X} raw={:08X} "
        "pc={:08X} thread={}",
        reg, normalized_value, raw_value, pc, oss.str());
  }
  if (cvars::gpu_log_mmio_writes_fetch && is_fetch0_reg) {
    XELOGI(
        "DEBUG: MMIO WRITE(FETCH0) reg={:04X} word={} value={:08X} raw={:08X} "
        "pc={:08X} thread={}",
        reg, reg - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, normalized_value,
        raw_value, pc, oss.str());
  }
  gs->WriteRegister(addr, normalized_value);
  if (is_cp_rb_reg) {
    static std::atomic<uint64_t> cp_state_cross_check_count{0};
    uint64_t cross_check_id =
        cp_state_cross_check_count.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t register_file_value = gs->register_file()->values[reg];
    bool register_file_match = register_file_value == normalized_value;
    const char* cp_field_name = "n/a";
    uint32_t cp_field_value = 0;
    bool cp_field_valid = false;
    bool cp_field_match = true;
    if (gs->command_processor_) {
      auto cp_state = gs->command_processor_->GetRingBufferDebugState();
      switch (reg) {
        case 0x01C0:
          cp_field_name = "primary_buffer_ptr";
          cp_field_value = cp_state.primary_buffer_ptr;
          cp_field_valid = true;
          cp_field_match = cp_field_value == normalized_value;
          break;
        case 0x01C5:
          cp_field_name = "write_ptr_index";
          cp_field_value = cp_state.write_ptr_index;
          cp_field_valid = true;
          cp_field_match = cp_field_value == normalized_value;
          break;
        default:
          break;
      }
    }
    bool log_cross_check = cross_check_id <= 10 || !register_file_match ||
                           (cp_field_valid && !cp_field_match);
    if (log_cross_check) {
      XELOGI(
          "RING BUFFER: MMIO state cross-check #{} reg={} raw={:08X} "
          "normalized={:08X} register_file={:08X} rf_match={} cp_field={} "
          "cp_value={:08X} cp_match={}",
          cross_check_id, bootstrap_reg_name ? bootstrap_reg_name : "<unknown>",
          raw_value, normalized_value, register_file_value,
          register_file_match ? "YES" : "NO", cp_field_name, cp_field_value,
          cp_field_valid ? (cp_field_match ? "YES" : "NO") : "N/A");
    }
  }
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x1951:  // interrupt status
      return 1;   // vblank
    case 0x1961:  // AVIVO_D1MODE_VIEWPORT_SIZE
                  // Screen res - 1280x720
                  // maximum [width(0x0FFF), height(0x0FFF)]
      return 0x050002D0;
    default:
      if (!register_file()->IsValidRegister(r)) {
        XELOGE("GPU: Read from unknown register ({:04X})", r);
      } else {
        // Log reads if requested or if it looks like a polling loop
        if (cvars::gpu_log_register_reads) {
          static std::atomic<uint64_t> read_count{0};
          uint64_t count = read_count.fetch_add(1) + 1;
          if (count <= 10 || (count % 256) == 0) {
            XELOGI("DEBUG: REGISTER READ: {:04X} -> {:08X} (count={})", r,
                   register_file()->values[r], count);
          }
        }
      }
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file()->values[r];
}

void GraphicsSystem::RecordWritePointerUpdate(uint32_t value,
                                              const char* source) {
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  uint64_t update_count =
      wptr_update_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  WptrUpdateSource source_id = ClassifyWptrUpdateSource(source);
  last_wptr_source_id_.store(static_cast<uint32_t>(source_id),
                             std::memory_order_relaxed);
  switch (source_id) {
    case WptrUpdateSource::kMmio:
      wptr_source_mmio_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case WptrUpdateSource::kCpPacket:
      wptr_source_cp_packet_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case WptrUpdateSource::kDebugSwapInject:
      wptr_source_debug_swap_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    case WptrUpdateSource::kOther:
      wptr_source_other_count_.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      break;
  }
  last_wptr_update_ms_.store(now_ms, std::memory_order_relaxed);
  last_wptr_value_.store(value, std::memory_order_relaxed);
  if ((source && cvars::trace_gpu_bootstrap) || update_count <= 3 ||
      (update_count % 256) == 0) {
    XELOGI(
        "DEBUG: WPTR update recorded #{} source={} value={:08X} at {} ms",
        update_count, WptrUpdateSourceName(static_cast<uint32_t>(source_id)),
        value, now_ms);
  }
}

GraphicsSystem::PipelinePrereqSnapshot
GraphicsSystem::GetPipelinePrereqSnapshot() const {
  PipelinePrereqSnapshot snapshot;
  snapshot.mmio_any_writes =
      mmio_any_write_count_.load(std::memory_order_relaxed);
  snapshot.mmio_cp_rb_base_writes =
      mmio_cp_rb_base_.count.load(std::memory_order_relaxed);
  snapshot.mmio_cp_rb_cntl_writes =
      mmio_cp_rb_cntl_.count.load(std::memory_order_relaxed);
  snapshot.mmio_cp_rb_wptr_writes =
      mmio_cp_rb_wptr_.count.load(std::memory_order_relaxed);
  snapshot.mmio_cp_rb_base_guest_writes =
      mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
  snapshot.mmio_cp_rb_cntl_guest_writes =
      mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
  snapshot.mmio_cp_rb_wptr_guest_writes =
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
  snapshot.mmio_cp_guest_writes =
      mmio_cp_guest_write_count_.load(std::memory_order_relaxed);
  snapshot.mmio_cp_host_writes =
      mmio_cp_host_write_count_.load(std::memory_order_relaxed);

  snapshot.vd_swap_calls =
      vd_swap_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_initialize_ring_buffer_calls =
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_set_interrupt_callback_calls =
      vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_get_system_command_buffer_calls =
      vd_get_system_cmd_buffer_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_enable_rptr_writeback_calls =
      vd_enable_rptr_writeback_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_initialize_edram_calls =
      vd_init_edram_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_retrain_edram_calls =
      vd_retrain_edram_state_.count.load(std::memory_order_relaxed);
  snapshot.vd_hsio_training_succeeded_calls =
      vd_hsio_training_succeeded_state_.count.load(std::memory_order_relaxed);

  snapshot.interrupt_callback = interrupt_callback_;
  snapshot.interrupt_callback_set = interrupt_callback_ != 0;
  snapshot.last_interrupt_callback_set_ms =
      last_interrupt_callback_set_ms_.load(std::memory_order_relaxed);
  snapshot.last_interrupt_dispatch_ms =
      last_interrupt_dispatch_ms_.load(std::memory_order_relaxed);
  snapshot.interrupt_dispatch_seen =
      snapshot.last_interrupt_dispatch_ms != 0;

  snapshot.no_force_profile = cvars::gpu_no_force_run_profile;
  snapshot.suspend_force_fallback_until_module_ready =
      cvars::gpu_suspend_force_fallback_until_module_ready;

  snapshot.user_module_load_event_present =
      kernel_state_ && kernel_state_->user_module_load_event() != nullptr;
  if (kernel_state_) {
    auto load_stats = kernel_state_->GetUserModuleLoadStats();
    snapshot.module_load_inflight = load_stats.load_inflight;
    snapshot.module_load_calls = load_stats.load_calls;
    snapshot.module_load_success = load_stats.load_success;
    snapshot.module_load_fail = load_stats.load_fail;
    snapshot.module_load_progress_count = load_stats.load_progress_count;
    snapshot.loaded_user_modules = load_stats.loaded_user_modules;
    snapshot.loaded_executable_user_modules =
        load_stats.loaded_executable_user_modules;
    snapshot.module_last_begin_ms = load_stats.last_begin_ms;
    snapshot.module_last_end_ms = load_stats.last_end_ms;
    snapshot.module_last_progress_ms = load_stats.last_progress_ms;

    auto executable_module = kernel_state_->GetExecutableModule();
    snapshot.executable_module_present = executable_module != nullptr;
    snapshot.executable_module_entry =
        executable_module ? executable_module->entry_point() : 0;

    if (auto* emulator = kernel_state_->emulator()) {
      uint64_t now_ms = Clock::QueryHostUptimeMillis();
      snapshot.guest_main_thread_ready = emulator->HasGuestMainThread();
      snapshot.guest_main_thread_running =
          snapshot.guest_main_thread_ready &&
          emulator->IsGuestMainThreadRunning();
      uint64_t stall_sample_ms = 0;
      emulator->GetLastGpuDispatchStallSnapshot(
          &snapshot.guest_main_dispatch_stall_state, nullptr, nullptr, nullptr,
          &snapshot.guest_main_dispatch_stall_repeat_samples,
          &stall_sample_ms);
      if (stall_sample_ms != 0 && now_ms >= stall_sample_ms) {
        snapshot.guest_main_dispatch_stall_age_ms = now_ms - stall_sample_ms;
      }
    }
  }

  snapshot.prelaunch_bootstrap_nudge_count =
      prelaunch_bootstrap_nudge_count_.load(std::memory_order_relaxed);
  snapshot.prelaunch_bootstrap_nudge_done =
      prelaunch_bootstrap_nudge_done_.load(std::memory_order_relaxed);
  snapshot.guest_main_resume_nudge_count =
      guest_main_resume_nudge_count_.load(std::memory_order_relaxed);
  snapshot.dead_pipeline_recovery_count =
      dead_pipeline_recovery_count_.load(std::memory_order_relaxed);
  snapshot.auto_callback_recovery_count =
      auto_callback_recovery_count_.load(std::memory_order_relaxed);

  return snapshot;
}

void GraphicsSystem::UpdateMmioWriteState(MmioWriteState& state, uint32_t value,
                                          uint32_t pc, uint32_t thread_id,
                                          bool guest_origin,
                                          const char* source) {
  uint64_t count = state.count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (guest_origin) {
    state.guest_count.fetch_add(1, std::memory_order_relaxed);
  } else {
    state.host_count.fetch_add(1, std::memory_order_relaxed);
  }
  state.value.store(value, std::memory_order_relaxed);
  state.pc.store(pc, std::memory_order_relaxed);
  state.thread_id.store(thread_id, std::memory_order_relaxed);
  state.time_ms.store(Clock::QueryHostUptimeMillis(),
                      std::memory_order_relaxed);
  if (source && (count <= 5 || (count % 512) == 0)) {
    XELOGW(
        "RING BUFFER: MMIO write observed reg={} count={} value={:08X} "
        "pc={:08X} thread_id={}",
        source, count, value, pc, thread_id);
  }
}

void GraphicsSystem::LogBootstrapMmioFirstWriteSnapshot(const char* reg_name,
                                                        uint32_t reg,
                                                        uint32_t addr,
                                                        uint32_t value,
                                                        uint32_t pc,
                                                        uint32_t thread_id,
                                                        bool guest_origin) {
  if (!kernel_state_) {
    return;
  }
  auto* memory = kernel_state_->memory();
  if (!memory) {
    return;
  }

  auto* thread = kernel::XThread::GetCurrentThread();
  uint32_t lr = 0;
  uint32_t r1 = 0;
  uint32_t thread_handle = thread ? thread->handle() : 0;
  bool thread_main = thread && thread->main_thread();
  if (thread && thread->thread_state() && thread->thread_state()->context()) {
    auto* ctx = thread->thread_state()->context();
    lr = static_cast<uint32_t>(ctx->lr);
    r1 = static_cast<uint32_t>(ctx->r[1]);
  }

  uint32_t mmio_page = addr & ~uint32_t(0xFFF);
  bool mmio_page_committed = false;
  bool mmio_page_commit_known = false;
  bool mmio_heap_backed = false;
  int mmio_heap_type = -1;
  uint32_t mmio_page_protect = 0;
  bool mmio_has_virtual_mapping = memory->LookupVirtualMappedRange(addr) != nullptr;
  auto* heap = memory->LookupHeap(mmio_page);
  if (heap) {
    mmio_heap_backed = true;
    mmio_heap_type = static_cast<int>(heap->heap_type());
    xe::HeapAllocationInfo mmio_info{};
    if (heap->QueryRegionInfo(mmio_page, &mmio_info)) {
      mmio_page_commit_known = true;
      mmio_page_committed = (mmio_info.state & xe::kMemoryAllocationCommit) != 0;
      mmio_page_protect = mmio_info.protect;
    }
    if (mmio_has_virtual_mapping && !mmio_page_committed) {
      uint32_t mmio_page_size = std::max<uint32_t>(heap->page_size(), 4096);
      bool commit_ok = heap->AllocFixed(
          mmio_page, mmio_page_size, mmio_page_size, xe::kMemoryAllocationCommit,
          xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
      xe::HeapAllocationInfo committed_info{};
      if (commit_ok && heap->QueryRegionInfo(mmio_page, &committed_info)) {
        mmio_page_commit_known = true;
        mmio_page_committed =
            (committed_info.state & xe::kMemoryAllocationCommit) != 0;
        mmio_page_protect = committed_info.protect;
      }
      XELOGW(
          "RING BUFFER: MMIO first-write auto-commit reg={} page={:08X} "
          "result={} committed={} protect=0x{:X}",
          reg_name ? reg_name : "<unknown>", mmio_page,
          commit_ok ? "OK" : "FAIL", mmio_page_committed ? "YES" : "NO",
          mmio_page_protect);
    }
  } else if (mmio_has_virtual_mapping) {
    void* mmio_host = memory->TranslateVirtual(mmio_page);
    bool commit_ok = false;
    if (mmio_host) {
      size_t host_page_len = xe::memory::page_size();
      uintptr_t host_page_addr = reinterpret_cast<uintptr_t>(mmio_host);
      if (host_page_len != 0) {
        host_page_addr &= ~(static_cast<uintptr_t>(host_page_len) - 1);
      }
      void* host_page_ptr = reinterpret_cast<void*>(host_page_addr);
      commit_ok = xe::memory::AllocFixed(
                      host_page_ptr, host_page_len,
                      xe::memory::AllocationType::kCommit,
                      xe::memory::PageAccess::kNoAccess) != nullptr;
    }
    mmio_page_commit_known = true;
    mmio_page_committed = commit_ok;
    XELOGI(
        "RING BUFFER: MMIO first-write host-commit reg={} page={:08X} "
        "result={} (mapping is callback-backed, not heap-backed)",
        reg_name ? reg_name : "<unknown>", mmio_page,
        commit_ok ? "OK" : "FAIL");
  }
  uint32_t physical_address = memory->GetPhysicalAddress(addr);
  const char* committed_str = mmio_page_commit_known
                                  ? (mmio_page_committed ? "YES" : "NO")
                                  : "UNKNOWN";

  XELOGW(
      "RING BUFFER: MMIO first-write snapshot reg={} reg_idx=0x{:04X} "
      "addr={:08X} value={:08X} pc={:08X} lr={:08X} thread_id={} "
      "thread_handle={:08X} main={} origin={} r1={:08X}",
      reg_name ? reg_name : "<unknown>", reg, addr, value, pc, lr, thread_id,
      thread_handle, thread_main ? "YES" : "NO",
      guest_origin ? "guest" : "host", r1);
  XELOGW(
      "RING BUFFER: MMIO first-write mapping reg={} mmio_page={:08X} "
      "mapped_range={} heap_backed={} heap_type={} committed={} "
      "protect=0x{:X} physical={:08X} physical_valid={}",
      reg_name ? reg_name : "<unknown>", mmio_page,
      mmio_has_virtual_mapping ? "YES" : "NO",
      mmio_heap_backed ? "YES" : "NO", mmio_heap_type, committed_str,
      mmio_page_protect, physical_address,
      physical_address == 0xFFFFFFFFu ? "NO" : "YES");
  if (mmio_has_virtual_mapping && mmio_heap_backed && mmio_page_commit_known &&
      !mmio_page_committed) {
    if (guest_origin) {
      XELOGW(
          "RING BUFFER: MMIO mapping anomaly for reg={} "
          "(virtual mapping exists but page is not committed)",
          reg_name ? reg_name : "<unknown>");
    } else {
      XELOGI(
          "RING BUFFER: MMIO mapping note for host-origin reg={} "
          "(virtual mapping exists but page is not committed)",
          reg_name ? reg_name : "<unknown>");
    }
  } else if (mmio_has_virtual_mapping && !mmio_heap_backed) {
    XELOGI(
        "RING BUFFER: MMIO mapping note for reg={} "
        "(callback-backed range; heap commit/protect telemetry is not "
        "applicable)",
        reg_name ? reg_name : "<unknown>");
  }

  // Host-origin writes (fallback/debug injection) have no reliable guest PC
  // execution context. Deep guest stack/disassembly probing here can touch
  // guarded pages and stall the GPU worker during recovery logging.
  if (!guest_origin) {
    XELOGI(
        "RING BUFFER: MMIO first-write forensic stack/disasm skipped for "
        "host-origin write reg={}",
        reg_name ? reg_name : "<unknown>");
    return;
  }

  auto log_ppc_disasm_window = [&](uint32_t center_pc, uint32_t bytes_before,
                                   uint32_t bytes_after, const char* label) {
    if (center_pc == 0) {
      return;
    }
    uint32_t start = center_pc >= bytes_before ? center_pc - bytes_before : 0;
    uint32_t bytes_span = bytes_before + bytes_after;
    uint32_t instr_count = bytes_span / 4 + 1;
    XELOGW(
        "RING BUFFER: MMIO first-write disasm reg={} {} center={:08X} "
        "window={:08X}-{:08X} instrs={}",
        reg_name ? reg_name : "<unknown>", label ? label : "pc", center_pc,
        start, start + (instr_count - 1) * 4, instr_count);
    xe::StringBuffer sb;
    for (uint32_t i = 0; i < instr_count; ++i) {
      uint32_t addr_i = start + i * 4;
      auto safe = memory->TranslateVirtualSafe<uint32_t*>(addr_i);
      if (!safe.success || !safe.pointer) {
        XELOGW("RING BUFFER: MMIO first-write disasm {:08X}: <invalid> ({})",
               addr_i, safe.error);
        continue;
      }
      uint32_t code = ::xe::load_and_swap<uint32_t>(safe.pointer);
      sb.Reset();
      xe::cpu::ppc::DisasmPPC(addr_i, code, &sb);
      XELOGW("RING BUFFER: MMIO first-write disasm {:08X}: {:08X} {}", addr_i,
             code, sb.to_string());
    }
  };

  log_ppc_disasm_window(pc, 0x20, 0x20, "pc");
  if (lr != 0) {
    log_ppc_disasm_window(lr, 0x10, 0x10, "lr");
  }

  if (r1 != 0) {
    auto stack_safe = memory->TranslateVirtualSafe<uint32_t*>(r1);
    if (stack_safe.success && stack_safe.pointer) {
      XELOGW(
          "RING BUFFER: MMIO first-write stack reg={} r1={:08X} "
          "backchain(max=8)",
          reg_name ? reg_name : "<unknown>", r1);
      uint32_t frame_sp = r1;
      for (uint32_t i = 0; i < 8; ++i) {
        auto frame_safe = memory->TranslateVirtualSafe<uint32_t*>(frame_sp);
        if (!frame_safe.success || !frame_safe.pointer) {
          XELOGW("RING BUFFER: MMIO first-write frame[{}] sp={:08X} <invalid> "
                 "({})",
                 i, frame_sp, frame_safe.error);
          break;
        }
        uint32_t next_sp = ::xe::load_and_swap<uint32_t>(frame_safe.pointer + 0);
        uint32_t saved_lr = ::xe::load_and_swap<uint32_t>(frame_safe.pointer + 2);
        XELOGW(
            "RING BUFFER: MMIO first-write frame[{}] sp={:08X} next_sp={:08X} "
            "saved_lr={:08X}",
            i, frame_sp, next_sp, saved_lr);
        if (next_sp == 0 || next_sp <= frame_sp) {
          break;
        }
        frame_sp = next_sp;
      }
    } else {
      XELOGW("RING BUFFER: MMIO first-write stack unavailable r1={:08X} ({})",
             r1, stack_safe.error);
    }
  }
}

void GraphicsSystem::UpdateVdCallState(VdCallState& state, uint32_t pc,
                                       uint32_t thread_id,
                                       const char* source, uint32_t lr) {
  uint64_t count = state.count.fetch_add(1, std::memory_order_relaxed) + 1;
  state.pc.store(pc, std::memory_order_relaxed);
  state.lr.store(lr, std::memory_order_relaxed);
  state.thread_id.store(thread_id, std::memory_order_relaxed);
  state.time_ms.store(Clock::QueryHostUptimeMillis(),
                      std::memory_order_relaxed);
  if (source && (count <= 5 || (count % 120) == 0)) {
    XELOGW(
        "RING BUFFER: {} call count={} pc={:08X} lr={:08X} thread_id={}",
        source, count, pc, lr, thread_id);
  }
}

void GraphicsSystem::ProbeCpBootstrapRegistersWhenMmioSilent(
    uint64_t now_ms, uint64_t since_first_vblank) {
  if (!cvars::gpu_debug_probe_cp_regs_when_mmio_silent || !register_file_) {
    return;
  }
  uint64_t interval_ms =
      static_cast<uint64_t>(cvars::gpu_debug_probe_cp_regs_interval_ms);
  uint64_t last_probe =
      last_silent_mmio_probe_ms_.load(std::memory_order_relaxed);
  if (last_probe != 0 && interval_ms != 0 && now_ms - last_probe < interval_ms) {
    return;
  }
  last_silent_mmio_probe_ms_.store(now_ms, std::memory_order_relaxed);

  uint64_t mmio_any_writes =
      mmio_any_write_count_.load(std::memory_order_relaxed);
  if (mmio_any_writes != 0) {
    return;
  }

  uint32_t rb_base = register_file_->values[0x01C0];
  uint32_t rb_cntl = register_file_->values[0x01C1];
  uint32_t rb_rptr_addr = register_file_->values[0x01C3];
  uint32_t rb_rptr = register_file_->values[0x01C4];
  uint32_t rb_wptr = register_file_->values[0x01C5];
  uint32_t rb_wptr_delay = register_file_->values[0x01C6];
  uint32_t rb_rptr_wr = register_file_->values[0x01C7];

  std::array<uint32_t, 6> fetch0{};
  bool fetch0_nonzero = false;
  for (uint32_t i = 0; i < fetch0.size(); ++i) {
    fetch0[i] =
        register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i];
    fetch0_nonzero |= fetch0[i] != 0;
  }

  bool any_signal = rb_base || rb_cntl || rb_rptr_addr || rb_rptr || rb_wptr ||
                    rb_wptr_delay || rb_rptr_wr || fetch0_nonzero;
  if (!any_signal) {
    return;
  }

  bool changed = !silent_mmio_probe_initialized_ ||
                 silent_mmio_probe_last_cp_rb_base_ != rb_base ||
                 silent_mmio_probe_last_cp_rb_cntl_ != rb_cntl ||
                 silent_mmio_probe_last_cp_rb_rptr_addr_ != rb_rptr_addr ||
                 silent_mmio_probe_last_cp_rb_rptr_ != rb_rptr ||
                 silent_mmio_probe_last_cp_rb_wptr_ != rb_wptr ||
                 silent_mmio_probe_last_cp_rb_wptr_delay_ != rb_wptr_delay ||
                 silent_mmio_probe_last_cp_rb_rptr_wr_ != rb_rptr_wr ||
                 silent_mmio_probe_last_fetch0_ != fetch0;
  if (!changed) {
    return;
  }

  silent_mmio_probe_initialized_ = true;
  silent_mmio_probe_last_cp_rb_base_ = rb_base;
  silent_mmio_probe_last_cp_rb_cntl_ = rb_cntl;
  silent_mmio_probe_last_cp_rb_rptr_addr_ = rb_rptr_addr;
  silent_mmio_probe_last_cp_rb_rptr_ = rb_rptr;
  silent_mmio_probe_last_cp_rb_wptr_ = rb_wptr;
  silent_mmio_probe_last_cp_rb_wptr_delay_ = rb_wptr_delay;
  silent_mmio_probe_last_cp_rb_rptr_wr_ = rb_rptr_wr;
  silent_mmio_probe_last_fetch0_ = fetch0;

  XELOGW(
      "RING BUFFER: MMIO fallback probe replay (mmio_any_writes=0) age={}ms "
      "rb_base={:08X} rb_cntl={:08X} rptr_addr={:08X} rptr={:08X} "
      "wptr={:08X}",
      since_first_vblank, rb_base, rb_cntl, rb_rptr_addr, rb_rptr, rb_wptr);

  auto replay_reg = [this](uint32_t reg, uint32_t value) {
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + reg * 4u, value);
  };

  replay_reg(0x01C0, rb_base);
  replay_reg(0x01C1, rb_cntl);
  replay_reg(0x01C3, rb_rptr_addr);
  replay_reg(0x01C4, rb_rptr);
  replay_reg(0x01C5, rb_wptr);
  replay_reg(0x01C6, rb_wptr_delay);
  replay_reg(0x01C7, rb_rptr_wr);
  for (uint32_t i = 0; i < fetch0.size(); ++i) {
    replay_reg(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i, fetch0[i]);
  }
}

bool GraphicsSystem::TryAutoRecoverMissingInterruptCallback(
    uint64_t now_ms, uint64_t since_first_vblank, const char* reason) {
  bool guest_main_thread_running = false;
  bool module_fully_ready = false;
  if (kernel_state_) {
    auto load_stats = kernel_state_->GetUserModuleLoadStats();
    auto executable_module = kernel_state_->GetExecutableModule();
    module_fully_ready = executable_module != nullptr &&
                         executable_module->entry_point() != 0 &&
                         !load_stats.load_inflight;
    if (auto* emulator = kernel_state_->emulator()) {
      guest_main_thread_running =
          emulator->HasGuestMainThread() && emulator->IsGuestMainThreadRunning();
    }
  }
    if (cvars::gpu_no_force_run_profile) {
      bool allow_no_force_recovery = command_processor_ != nullptr;
    bool startup_gate_hard_mode =
        cvars::gpu_no_force_callback_startup_prereq_hard_gate;
    bool startup_bridge_ready = false;
    bool callback_startup_gate_ready = false;
    bool observed_bootstrap_activity = false;
    bool startup_prolog_helper_grace = false;
    std::string unmet_startup_conditions = "startup_bridge_ready,"
                                           "callback_startup_gate_ready,"
                                           "bootstrap_activity";
    if (allow_no_force_recovery) {
      auto no_force_dbg = command_processor_->GetRingBufferDebugState();
      bool ring_configured = no_force_dbg.ring_buffer_initialized &&
                             no_force_dbg.primary_buffer_ptr != 0 &&
                             no_force_dbg.primary_buffer_size != 0;
      bool ring_ready = ring_configured &&
                        no_force_dbg.initialization_acknowledged;
      uint64_t observed_vd_init_count =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t observed_vd_set_cb_count =
          vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
      uint64_t observed_vd_swap_count =
          vd_swap_state_.count.load(std::memory_order_relaxed);
      uint64_t observed_mmio_rb_base_count =
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
      uint64_t observed_mmio_rb_cntl_count =
          mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
      uint64_t observed_mmio_wptr_count =
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
      auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
      DispatchPathEvidence dispatch_evidence =
          ClassifyDispatchPathEvidence(proc_stats);
      uint64_t runtime_bootstrap_total =
          GetGpuBootstrapOrdinalRuntimeTotal(proc_stats, 0x01C2) +
          GetGpuBootstrapOrdinalRuntimeTotal(proc_stats, 0x01C3) +
          GetGpuBootstrapOrdinalRuntimeTotal(proc_stats, 0x01D5);
      uint64_t runtime_dispatch_total =
          dispatch_evidence.combined_runtime_dispatch_total;
      bool dispatch_path_gate_ready =
          dispatch_evidence.evidence_case != DispatchPathEvidenceCase::kNone;
      observed_bootstrap_activity =
          observed_vd_init_count != 0 || observed_vd_set_cb_count != 0 ||
          observed_vd_swap_count != 0 || observed_mmio_rb_base_count != 0 ||
          observed_mmio_rb_cntl_count != 0 || observed_mmio_wptr_count != 0 ||
          runtime_bootstrap_total != 0 || runtime_dispatch_total != 0 ||
          proc_stats.proc_lookup_calls != 0 ||
          proc_stats.proc_lookup_resolved != 0 ||
          proc_stats.proc_lookup_missing != 0;
      uint64_t observed_swap_packets = command_processor_->swap_packet_count();
      bool observed_gpu_submission_activity =
          observed_vd_swap_count != 0 || observed_mmio_wptr_count != 0 ||
          observed_swap_packets != 0;
      std::string startup_dispatch_stall_state;
      uint32_t startup_dispatch_stall_repeat_samples = 0;
      uint64_t startup_dispatch_stall_sample_ms = 0;
      uint64_t startup_dispatch_stall_age_ms = 0;
      if (kernel_state_) {
        if (auto* emulator = kernel_state_->emulator()) {
          if (emulator->GetLastGpuDispatchStallSnapshot(
                  &startup_dispatch_stall_state, nullptr, nullptr, nullptr,
                  &startup_dispatch_stall_repeat_samples,
                  &startup_dispatch_stall_sample_ms) &&
              startup_dispatch_stall_sample_ms != 0 &&
              now_ms >= startup_dispatch_stall_sample_ms) {
            startup_dispatch_stall_age_ms =
                now_ms - startup_dispatch_stall_sample_ms;
          }
          startup_prolog_helper_grace = IsStartupPrologHelperGraceWindow(
              startup_dispatch_stall_state, startup_dispatch_stall_age_ms,
              startup_dispatch_stall_repeat_samples);
        }
      }
      startup_bridge_ready =
          ring_configured && guest_main_thread_running && module_fully_ready;
      bool startup_bridge_phase_ready =
          startup_bridge_ready && !no_force_dbg.initialization_acknowledged;
      callback_startup_gate_ready =
          dispatch_path_gate_ready && ring_ready && startup_bridge_ready &&
          observed_bootstrap_activity && !startup_prolog_helper_grace;
      unmet_startup_conditions = FormatStartupGateUnmetConditions(
          startup_bridge_ready, callback_startup_gate_ready,
          observed_bootstrap_activity);
      if (!dispatch_path_gate_ready) {
        if (!unmet_startup_conditions.empty()) {
          unmet_startup_conditions.append(",");
        }
        unmet_startup_conditions.append("dispatch_path_gate_ready");
      }
      bool startup_prereq_ready = startup_bridge_ready &&
                                  callback_startup_gate_ready &&
                                  observed_bootstrap_activity;
      bool allow_startup_gate_recovery =
          cvars::gpu_no_force_allow_callback_recovery_on_startup_gate &&
          callback_startup_gate_ready;
      if (startup_gate_hard_mode) {
        allow_no_force_recovery = startup_prereq_ready;
      } else {
        allow_no_force_recovery =
            (ring_ready && observed_gpu_submission_activity) ||
            startup_bridge_phase_ready || allow_startup_gate_recovery;
      }
      if (!allow_no_force_recovery) {
        static std::atomic<uint64_t>
            auto_callback_no_force_waiting_last_log_ms{0};
        uint64_t interval_ms = static_cast<uint64_t>(
            cvars::gpu_no_force_run_profile_log_interval_ms);
        uint64_t last_log = auto_callback_no_force_waiting_last_log_ms.load(
            std::memory_order_relaxed);
        if (last_log == 0 || interval_ms == 0 || now_ms < last_log ||
            now_ms - last_log >= interval_ms) {
          auto_callback_no_force_waiting_last_log_ms.store(
              now_ms, std::memory_order_relaxed);
          XELOGW(
              "DEBUG: AUTO CALLBACK RECOVERY no-force guard snapshot "
              "({}) "
              "(ring_configured={} ring_ready={} startup_bridge_ready={} "
              "callback_startup_gate_ready={} startup_gate_recovery_enabled={} "
              "startup_prolog_helper_grace={} "
              "guest_main_running={} module_ready={} bootstrap_activity={} "
              "unmet_startup_conditions='{}' "
              "runtime_bootstrap_total={} runtime_dispatch_total={} "
              "critical_runtime_dispatch_total={} "
              "guest_main_dispatch_bridge_total={} "
              "proc_lookup_calls={} vd_init={} "
              "vd_set_cb={} vd_swap={} mmio_base={} mmio_cntl={} "
              "mmio_wptr={} swap_packets={})",
              startup_gate_hard_mode
                  ? "hard gate active; callback recovery blocked"
                  : "advisory only; generic recovery still allowed",
              ring_configured ? "YES" : "NO", ring_ready ? "YES" : "NO",
              startup_bridge_ready ? "YES" : "NO",
              callback_startup_gate_ready ? "YES" : "NO",
              cvars::gpu_no_force_allow_callback_recovery_on_startup_gate
                  ? "YES"
                  : "NO",
              startup_prolog_helper_grace ? "YES" : "NO",
              guest_main_thread_running ? "YES" : "NO",
              module_fully_ready ? "YES" : "NO",
              observed_bootstrap_activity ? "YES" : "NO",
              unmet_startup_conditions, runtime_bootstrap_total,
              runtime_dispatch_total,
              dispatch_evidence.critical_runtime_dispatch_total,
              dispatch_evidence.guest_main_dispatch_bridge_total,
              proc_stats.proc_lookup_calls,
              observed_vd_init_count, observed_vd_set_cb_count,
              observed_vd_swap_count, observed_mmio_rb_base_count,
              observed_mmio_rb_cntl_count, observed_mmio_wptr_count,
              observed_swap_packets);
        }
      }
    }
    if (!allow_no_force_recovery) {
      static std::atomic<uint64_t> auto_callback_no_force_advisory_last_log_ms{0};
      uint64_t interval_ms = static_cast<uint64_t>(
          cvars::gpu_no_force_run_profile_log_interval_ms);
      uint64_t last_log = auto_callback_no_force_advisory_last_log_ms.load(
          std::memory_order_relaxed);
      if (last_log == 0 || interval_ms == 0 || now_ms < last_log ||
          now_ms - last_log >= interval_ms) {
        auto_callback_no_force_advisory_last_log_ms.store(
            now_ms, std::memory_order_relaxed);
        if (startup_gate_hard_mode) {
          XELOGW(
              "DEBUG: AUTO CALLBACK RECOVERY no-force hard gate WAITING: startup "
              "bridge/gate prerequisites unmet; callback recovery remains "
              "blocked "
              "(unmet_startup_conditions='{}' guest_main_running={} "
              "module_ready={} startup_bridge_ready={} "
              "callback_startup_gate_ready={} bootstrap_activity={} "
              "startup_prolog_helper_grace={})",
              unmet_startup_conditions,
              guest_main_thread_running ? "YES" : "NO",
              module_fully_ready ? "YES" : "NO",
              startup_bridge_ready ? "YES" : "NO",
              callback_startup_gate_ready ? "YES" : "NO",
              observed_bootstrap_activity ? "YES" : "NO",
              startup_prolog_helper_grace ? "YES" : "NO");
        } else {
          XELOGW(
              "DEBUG: AUTO CALLBACK RECOVERY no-force advisory: startup "
              "bridge/gate conditions not fully met yet, continuing generic "
              "recovery checks (guest_main_running={} module_ready={} "
              "unmet_startup_conditions='{}' "
              "startup_prolog_helper_grace={})",
              guest_main_thread_running ? "YES" : "NO",
              module_fully_ready ? "YES" : "NO", unmet_startup_conditions,
              startup_prolog_helper_grace ? "YES" : "NO");
        }
        }
      } else {
        static std::atomic<uint64_t>
            auto_callback_no_force_exception_last_log_ms{0};
      uint64_t interval_ms = static_cast<uint64_t>(
          cvars::gpu_no_force_run_profile_log_interval_ms);
      uint64_t last_log =
          auto_callback_no_force_exception_last_log_ms.load(
              std::memory_order_relaxed);
      if (last_log == 0 || interval_ms == 0 || now_ms < last_log ||
          now_ms - last_log >= interval_ms) {
        auto_callback_no_force_exception_last_log_ms.store(
            now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: AUTO CALLBACK RECOVERY no-force readiness: startup "
            "bridge/gate conditions satisfied (startup_bridge_ready={} "
            "callback_startup_gate_ready={} bootstrap_activity={} "
            "guest_main_running={} module_ready={} "
            "startup_prolog_helper_grace={})",
            startup_bridge_ready ? "YES" : "NO",
            callback_startup_gate_ready ? "YES" : "NO",
            observed_bootstrap_activity ? "YES" : "NO",
            guest_main_thread_running ? "YES" : "NO",
            module_fully_ready ? "YES" : "NO",
            startup_prolog_helper_grace ? "YES" : "NO");
      }
    }

    if (startup_gate_hard_mode && !allow_no_force_recovery) {
      return false;
    }
  }
  if (!cvars::gpu_auto_recover_interrupt_callback_missing || interrupt_callback_ != 0) {
    return false;
  }
  uint64_t recovery_after_ms = static_cast<uint64_t>(
      cvars::gpu_auto_recover_interrupt_callback_after_ms);
  if (!command_processor_) {
    return false;
  }
  auto dbg = command_processor_->GetRingBufferDebugState();
  uint64_t swap_count = command_processor_->swap_packet_count();
  bool saw_real_swap_activity = swap_count != 0;
    uint64_t observed_vd_init_count =
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t observed_vd_set_cb_count =
      vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
    uint64_t observed_vd_swap_count =
      vd_swap_state_.count.load(std::memory_order_relaxed);
    uint64_t observed_mmio_rb_base_count =
      mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
    uint64_t observed_mmio_rb_cntl_count =
      mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
    uint64_t observed_mmio_rb_wptr_count =
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
    bool observed_bootstrap_activity =
      observed_vd_init_count != 0 || observed_vd_set_cb_count != 0 ||
      observed_vd_swap_count != 0 || observed_mmio_rb_base_count != 0 ||
      observed_mmio_rb_cntl_count != 0 || observed_mmio_rb_wptr_count != 0;
  bool ring_initialized = dbg.ring_buffer_initialized;
  bool ring_configured =
      ring_initialized && dbg.primary_buffer_ptr != 0 &&
      dbg.primary_buffer_size != 0;
  if (!ring_initialized) {
    return false;
  }
  bool startup_bridge_mode =
      cvars::gpu_no_force_run_profile && !dbg.initialization_acknowledged &&
      guest_main_thread_running && module_fully_ready;
  if (!ring_configured && !startup_bridge_mode) {
    return false;
  }
  if (!dbg.initialization_acknowledged && !startup_bridge_mode) {
    return false;
  }
  if (!saw_real_swap_activity && !observed_bootstrap_activity &&
      since_first_vblank < recovery_after_ms &&
      !startup_bridge_mode) {
    return false;
  }
  bool already_recovered =
      auto_callback_recovery_done_.load(std::memory_order_relaxed);
  if (cvars::gpu_auto_recover_interrupt_callback_once && already_recovered) {
    return false;
  }
  if (!processor_) {
    XELOGW(
        "DEBUG: AUTO CALLBACK RECOVERY skipped: no processor available "
        "(reason={}, age={}ms)",
        reason ? reason : "unknown", since_first_vblank);
    return false;
  }

  static std::atomic<uint64_t> non_force_waiting_last_log_ms{0};
  uint64_t interval_ms = static_cast<uint64_t>(
      cvars::gpu_no_force_run_profile_log_interval_ms);
  uint64_t last_log =
      non_force_waiting_last_log_ms.load(std::memory_order_relaxed);
  if (last_log == 0 || interval_ms == 0 || now_ms < last_log ||
      now_ms - last_log >= interval_ms) {
    non_force_waiting_last_log_ms.store(now_ms, std::memory_order_relaxed);
    XELOGW(
        "DEBUG: AUTO CALLBACK RECOVERY non-force hard gate: callback remains "
        "unset; waiting for guest callback registration "
        "(reason={} age={}ms rb_base={:08X} rb_size={:08X} read_ptr={:08X} "
        "write_ptr={:08X} swap_count={} startup_bridge={} "
        "bootstrap_activity={} vd_init={} vd_set_cb={} vd_swap={} "
        "mmio_base={} mmio_cntl={} mmio_wptr={})",
        reason ? reason : "unknown", since_first_vblank, dbg.primary_buffer_ptr,
        dbg.primary_buffer_size, dbg.read_ptr_index, dbg.write_ptr_index,
        swap_count, startup_bridge_mode ? "YES" : "NO",
        observed_bootstrap_activity ? "YES" : "NO", observed_vd_init_count,
        observed_vd_set_cb_count, observed_vd_swap_count,
        observed_mmio_rb_base_count, observed_mmio_rb_cntl_count,
        observed_mmio_rb_wptr_count);
  }
  return false;
}

void GraphicsSystem::NotifyVdSwapCall(uint32_t pc, uint32_t thread_id) {
  UpdateVdCallState(vd_swap_state_, pc, thread_id, "VdSwap");
}

void GraphicsSystem::NotifyVdInitializeEnginesCall(uint32_t pc,
                                                   uint32_t thread_id) {
  UpdateVdCallState(vd_init_engines_state_, pc, thread_id,
                    "VdInitializeEngines");
}

void GraphicsSystem::NotifyVdGetGraphicsAsicIDCall(uint32_t pc,
                                                   uint32_t thread_id) {
  UpdateVdCallState(vd_get_graphics_asic_id_state_, pc, thread_id,
                    "VdGetGraphicsAsicID");
}

void GraphicsSystem::NotifyVdInitializeRingBufferCall(uint32_t pc,
                                                      uint32_t thread_id) {
  UpdateVdCallState(vd_init_ring_buffer_state_, pc, thread_id,
                    "VdInitializeRingBuffer");
}

void GraphicsSystem::NotifyVdEnableRingBufferRPtrWriteBackCall(
    uint32_t pc, uint32_t thread_id) {
  UpdateVdCallState(vd_enable_rptr_writeback_state_, pc, thread_id,
                    "VdEnableRingBufferRPtrWriteBack");
}

void GraphicsSystem::NotifyVdSetGraphicsInterruptCallbackCall(
    uint32_t pc, uint32_t thread_id, uint32_t lr) {
  UpdateVdCallState(vd_set_interrupt_callback_state_, pc, thread_id,
                    "VdSetGraphicsInterruptCallback", lr);
}

void GraphicsSystem::NotifyVdGetSystemCommandBufferCall(uint32_t pc,
                                                        uint32_t thread_id) {
  UpdateVdCallState(vd_get_system_cmd_buffer_state_, pc, thread_id,
                    "VdGetSystemCommandBuffer");
}

void GraphicsSystem::NotifyVdInitializeEDRAMCall(uint32_t pc,
                                                 uint32_t thread_id) {
  UpdateVdCallState(vd_init_edram_state_, pc, thread_id, "VdInitializeEDRAM");
}

void GraphicsSystem::NotifyVdRetrainEDRAMCall(uint32_t pc, uint32_t thread_id) {
  UpdateVdCallState(vd_retrain_edram_state_, pc, thread_id, "VdRetrainEDRAM");
}

void GraphicsSystem::NotifyVdRetrainEDRAMWorkerCall(uint32_t pc,
                                                    uint32_t thread_id) {
  UpdateVdCallState(vd_retrain_edram_worker_state_, pc, thread_id,
                    "VdRetrainEDRAMWorker");
}

void GraphicsSystem::NotifyVdIsHSIOTrainingSucceededCall(uint32_t pc,
                                                         uint32_t thread_id) {
  UpdateVdCallState(vd_hsio_training_succeeded_state_, pc, thread_id,
                    "VdIsHSIOTrainingSucceeded");
}

void GraphicsSystem::InjectGuestBootstrapDebugActivity(
    const char* reason, uint64_t since_first_vblank) {
  if (!command_processor_) {
    return;
  }

  command_processor_->CallInThread([this, reason, since_first_vblank]() {
    auto dbg = command_processor_->GetRingBufferDebugState();

    uint32_t rb_base = dbg.primary_buffer_ptr;
    uint32_t rb_size = dbg.primary_buffer_size;
    if (!rb_base || !rb_size) {
      rb_base = 0x80000000;
      uint32_t size_log2 = cvars::gpu_force_init_ring_buffer_size_log2;
      if (size_log2 == 0 || size_log2 > 24) {
        size_log2 = 18;
      }
      rb_size = uint32_t(1) << (size_log2 + 3);
    }

    uint32_t rb_cntl = register_file_ ? register_file_->values[0x01C1] : 0;
    if (((rb_cntl >> 8) & 0x3F) == 0) {
      uint32_t rb_size_log2 = 0;
      uint32_t rb_size_tmp = rb_size;
      while (rb_size_tmp > 8) {
        rb_size_tmp >>= 1;
        ++rb_size_log2;
      }
      rb_cntl = (rb_size_log2 & 0x3F) << 8;
    }

    uint32_t current_wptr = dbg.write_ptr_index;
    uint32_t current_rptr = dbg.read_ptr_index;
    uint32_t injected_wptr = current_wptr;

    uint32_t frontbuffer_ptr = dbg.debug_host_swap_last_frontbuffer_ptr
                                   ? dbg.debug_host_swap_last_frontbuffer_ptr
                                   : dbg.last_swap_frontbuffer_ptr;
    uint32_t width = dbg.debug_host_swap_last_width
                         ? dbg.debug_host_swap_last_width
                         : dbg.last_swap_width;
    uint32_t height = dbg.debug_host_swap_last_height
                          ? dbg.debug_host_swap_last_height
                          : dbg.last_swap_height;
    if (!width || !height) {
      auto res = GraphicsSystem::GetInternalDisplayResolution();
      width = res.first;
      height = res.second;
    }

    if (memory_ && rb_base && rb_size) {
      auto* rb_host = reinterpret_cast<uint8_t*>(memory_->TranslatePhysical(rb_base));
      uint32_t ring_dwords = rb_size / sizeof(uint32_t);
      if (rb_host && ring_dwords && frontbuffer_ptr != 0) {
        uint32_t inject_index = current_wptr % ring_dwords;
        constexpr uint32_t kFetchDwords = 6;
        constexpr uint32_t kSwapPayloadDwords = 4;
        constexpr uint32_t kSwapPacketDwords =
            1 + kFetchDwords + 1 + kSwapPayloadDwords;

        xenos::xe_gpu_texture_fetch_t fetch{};
        uint32_t pitch = width;
        fetch.type = xenos::FetchConstantType::kTexture;
        fetch.clamp_x = xenos::ClampMode::kClampToEdge;
        fetch.clamp_y = xenos::ClampMode::kClampToEdge;
        fetch.clamp_z = xenos::ClampMode::kClampToEdge;
        fetch.pitch = pitch >> 5;
        fetch.tiled = 0;
        fetch.format = xenos::TextureFormat::k_8_8_8_8;
        fetch.endianness = xenos::Endian::kNone;
        fetch.base_address = frontbuffer_ptr >> 12;
        fetch.size_2d.width = width - 1;
        fetch.size_2d.height = height - 1;
        fetch.size_2d.stack_depth = 0;
        fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
        fetch.dimension = xenos::DataDimension::k2DOrStacked;
        fetch.mip_address = fetch.base_address;

        uint32_t packet[kSwapPacketDwords] = {};
        uint32_t offset = 0;
        packet[offset++] =
            xenos::MakePacketType0(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
                                   kFetchDwords);
        packet[offset++] = fetch.dword_0;
        packet[offset++] = fetch.dword_1;
        packet[offset++] = fetch.dword_2;
        packet[offset++] = fetch.dword_3;
        packet[offset++] = fetch.dword_4;
        packet[offset++] = fetch.dword_5;
        packet[offset++] =
            xenos::MakePacketType3(xenos::PM4_XE_SWAP, kSwapPayloadDwords);
        packet[offset++] = xenos::kSwapSignature;
        packet[offset++] = frontbuffer_ptr;
        packet[offset++] = width;
        packet[offset++] = height;

        for (uint32_t i = 0; i < kSwapPacketDwords; ++i) {
          uint32_t dword_index = (inject_index + i) % ring_dwords;
          xe::store_and_swap<uint32_t>(
              reinterpret_cast<uint32_t*>(rb_host + dword_index * 4),
              packet[i]);
        }
        injected_wptr = (inject_index + kSwapPacketDwords) % ring_dwords;
        injected_wptr &= ~uint32_t(0x3);
        if (injected_wptr == current_rptr) {
          injected_wptr = (injected_wptr + 4) % ring_dwords;
          injected_wptr &= ~uint32_t(0x3);
        }
      }

    }

    XELOGW(
        "DEBUG: GUEST DEBUG INJECTION: reason={} age={}ms rb_base={:08X} "
        "rb_size={:08X} rb_cntl={:08X} frontbuffer={:08X} size={}x{} "
        "read_ptr={:08X} write_ptr={:08X}->{:08X}",
        reason ? reason : "unknown", since_first_vblank, rb_base, rb_size,
        rb_cntl, frontbuffer_ptr, width, height, current_rptr, current_wptr,
        injected_wptr);

    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C0u * 4u, rb_base);
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C1u * 4u, rb_cntl);
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C3u * 4u,
                       dbg.read_ptr_writeback_ptr);
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C4u * 4u,
                       current_rptr);
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C5u * 4u,
                       injected_wptr);
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C6u * 4u,
                       dbg.read_ptr_update_freq);
    WriteRegisterThunk(nullptr, this, 0x7FC80000u + 0x01C7u * 4u,
                       current_rptr);

    // Populate FETCH0 via MMIO writes too so no-swap snapshot can show
    // frontbuffer setup as guest-visible register activity.
    xenos::xe_gpu_texture_fetch_t fetch{};
    uint32_t pitch = width ? width : 1280;
    fetch.type = xenos::FetchConstantType::kTexture;
    fetch.clamp_x = xenos::ClampMode::kClampToEdge;
    fetch.clamp_y = xenos::ClampMode::kClampToEdge;
    fetch.clamp_z = xenos::ClampMode::kClampToEdge;
    fetch.pitch = pitch >> 5;
    fetch.tiled = 0;
    fetch.format = xenos::TextureFormat::k_8_8_8_8;
    fetch.endianness = xenos::Endian::kNone;
    fetch.base_address = frontbuffer_ptr >> 12;
    fetch.size_2d.width = (width ? width : 1280) - 1;
    fetch.size_2d.height = (height ? height : 720) - 1;
    fetch.size_2d.stack_depth = 0;
    fetch.swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
    fetch.dimension = xenos::DataDimension::k2DOrStacked;
    fetch.mip_address = fetch.base_address;

    WriteRegisterThunk(nullptr, this,
                       0x7FC80000u + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 * 4u,
                       fetch.dword_0);
    WriteRegisterThunk(nullptr, this,
                       0x7FC80000u + (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1) * 4u,
                       fetch.dword_1);
    WriteRegisterThunk(nullptr, this,
                       0x7FC80000u + (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2) * 4u,
                       fetch.dword_2);
    WriteRegisterThunk(nullptr, this,
                       0x7FC80000u + (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 3) * 4u,
                       fetch.dword_3);
    WriteRegisterThunk(nullptr, this,
                       0x7FC80000u + (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 4) * 4u,
                       fetch.dword_4);
    WriteRegisterThunk(nullptr, this,
                       0x7FC80000u + (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 5) * 4u,
                       fetch.dword_5);

    // NOTE: Do NOT call NotifyVd*() here with fake pc=0/thread=0 values.
    // Those calls inflate the Vd counters used by the no_bootstrap_activity
    // guard in the prelaunch bootstrap watchdog (VblankInterruptCallback).
    // Calling them here poisons that guard and prevents any follow-up nudge
    // from ever firing, causing a permanent stall at GPU worker loop #4 with
    // pending_fns=0 and interrupt_callback_set=NO.
    // Vd counters must only be incremented by real guest VdXxx() calls.
  }, reason && reason[0] ? reason : "inject-guest-bootstrap-debug-activity");
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  static std::atomic<uint32_t> wptr_count{0};
  static std::atomic<uint32_t> wptr_delay_count{0};
  static std::atomic<uint32_t> rptr_addr_count{0};
  static std::atomic<uint32_t> rptr_count{0};
  static std::atomic<uint32_t> rptr_wr_count{0};
  static std::atomic<uint32_t> last_wptr{0};

  uint32_t r = (addr & 0xFFFF) / 4;
  bool is_fetch0_reg = r >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
                       r <= (XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 5);
  auto log_bootstrap_handler_hit = [&](const char* reg_name,
                                       const MmioWriteState& state) {
    if (!cvars::trace_gpu_bootstrap || !reg_name) {
      return;
    }
    XELOGI(
        "DEBUG: GPU bootstrap MMIO handler: reg={} value={:08X} pc={:08X} "
        "thread_id={} handler_reached=YES",
        reg_name, value, state.pc.load(std::memory_order_relaxed),
        state.thread_id.load(std::memory_order_relaxed));
  };
  switch (r) {
    case 0x01C0:  // CP_RB_BASE - Ring Buffer Base Address
      log_bootstrap_handler_hit("CP_RB_BASE", mmio_cp_rb_base_);
      XELOGI("DEBUG: REGISTER WRITE: CP_RB_BASE = {:08X}", value);
      // Game may initialize ring buffer via direct register writes
      // Check if we also have a valid size from CP_RB_CNTL
      {
        uint32_t rb_base = value;
        uint32_t rb_cntl = register_file()->values[0x01C1];
        uint32_t rb_size_log2 = (rb_cntl >> 8) & 0x3F;

        if (rb_size_log2 > 0 && rb_size_log2 < 32) {
          XELOGI("DEBUG: AUTO-INIT: Ring buffer from registers!");
          XELOGI("   base={:08X}, size_log2={}", rb_base, rb_size_log2);
          command_processor_->InitializeRingBuffer(rb_base, rb_size_log2);
        }
      }
      break;

    case 0x01C1:  // CP_RB_CNTL - Ring Buffer Control
      log_bootstrap_handler_hit("CP_RB_CNTL", mmio_cp_rb_cntl_);
      XELOGI("DEBUG: REGISTER WRITE: CP_RB_CNTL = {:08X}", value);
      {
        uint32_t rb_base = register_file()->values[0x01C0];
        uint32_t rb_size_log2 = (value >> 8) & 0x3F;

        if (rb_base != 0 && rb_size_log2 > 0 && rb_size_log2 < 32) {
          XELOGI("DEBUG: AUTO-INIT: Ring buffer from CP_RB_CNTL!");
          command_processor_->InitializeRingBuffer(rb_base, rb_size_log2);
        }

        // Also check for read pointer writeback
        uint32_t rb_rptr_addr = register_file()->values[0x01C3];
        if (rb_rptr_addr != 0) {
          uint32_t block_size_log2 = (value >> 8) & 0x3F;
          XELOGI("DEBUG: AUTO-INIT: Read pointer writeback from CP_RB_CNTL!");
          command_processor_->EnableReadPointerWriteBack(rb_rptr_addr,
                                                         block_size_log2);
        }
      }
      break;
    case 0x01C3: {  // CP_RB_RPTR_ADDR
      log_bootstrap_handler_hit("CP_RB_RPTR_ADDR", mmio_cp_rb_rptr_addr_);
      uint32_t count = rptr_addr_count.fetch_add(1) + 1;
      if (count <= 5 || (count % 256) == 0) {
        XELOGI("DEBUG: REGISTER WRITE: CP_RB_RPTR_ADDR = {:08X} (count={})", value,
               count);
      }
      {
        uint32_t rb_cntl = register_file()->values[0x01C1];
        uint32_t block_size_log2 = (rb_cntl >> 8) & 0x3F;
        command_processor_->EnableReadPointerWriteBack(value, block_size_log2);
      }
    } break;
    case 0x01C4: {  // CP_RB_RPTR
      log_bootstrap_handler_hit("CP_RB_RPTR", mmio_cp_rb_rptr_);
      uint32_t count = rptr_count.fetch_add(1) + 1;
      if (count <= 5 || (count % 256) == 0) {
        XELOGI("DEBUG: REGISTER WRITE: CP_RB_RPTR = {:08X} (count={})", value,
               count);
      }
    } break;
    case 0x01C5:  // CP_RB_WPTR
      {
        log_bootstrap_handler_hit("CP_RB_WPTR", mmio_cp_rb_wptr_);
        uint32_t count = wptr_count.fetch_add(1) + 1;
        uint32_t prev = last_wptr.exchange(value);
        bool wrapped = value < prev;
        if (count <= 10 || wrapped || (count % 512) == 0) {
          XELOGI(
              "DEBUG: REGISTER WRITE: CP_RB_WPTR = {:08X} (prev={:08X}, count={}, "
              "wrapped={})",
              value, prev, count, wrapped ? "YES" : "NO");
        }
      }
      RecordWritePointerUpdate(value, "CP_RB_WPTR");
      command_processor_->UpdateWritePointer(value, "MMIO_CP_RB_WPTR");
      break;
    case 0x01C6: {  // CP_RB_WPTR_DELAY
      log_bootstrap_handler_hit("CP_RB_WPTR_DELAY", mmio_cp_rb_wptr_delay_);
      uint32_t count = wptr_delay_count.fetch_add(1) + 1;
      if (count <= 5 || (count % 256) == 0) {
        XELOGI("DEBUG: REGISTER WRITE: CP_RB_WPTR_DELAY = {:08X} (count={})", value,
               count);
      }
    } break;
    case 0x01C7: {  // CP_RB_RPTR_WR
      log_bootstrap_handler_hit("CP_RB_RPTR_WR", mmio_cp_rb_rptr_wr_);
      uint32_t count = rptr_wr_count.fetch_add(1) + 1;
      if (count <= 5 || (count % 256) == 0) {
        XELOGI("DEBUG: REGISTER WRITE: CP_RB_RPTR_WR = {:08X} (count={})", value,
               count);
      }
    } break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      if (is_fetch0_reg) {
        log_bootstrap_handler_hit("FETCH0_WORD", mmio_fetch0_);
      } else {
        const RegisterInfo* reg_info = RegisterFile::GetRegisterInfo(r);
        if (reg_info) {
          std::string_view reg_name = reg_info->name ? reg_info->name : "";
          if (reg_name.starts_with("MC0_")) {
            // Memory-controller init registers are expected during early GPU
            // bootstrap. We currently don't emulate side effects for these
            // registers, but they are intentionally accepted and tracked.
            static std::atomic<uint64_t> mc_handled_log_count{0};
            uint64_t log_id = mc_handled_log_count.fetch_add(1) + 1;
            if (log_id <= 6 || (log_id % 256) == 0) {
              XELOGI(
                  "GPU init register write handled (no-op) {:04X} ({}) "
                  "value={:08X}",
                  r, reg_info->name, value);
            }
          } else {
            XELOGW("Unhandled GPU register {:04X} ({}) write: {:08X}", r,
                   reg_info->name, value);
          }
        } else {
          XELOGW("Unknown GPU register {:04X} write: {:08X}", r, value);
        }
      }
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  this->register_file()->values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr,
                                                uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback,
                                          uint32_t user_data) {
  static std::atomic<uint64_t> set_count{0};
  uint64_t call_id = set_count.fetch_add(1) + 1;
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  uint64_t prev_set_ms =
      last_interrupt_callback_set_ms_.load(std::memory_order_relaxed);
  uint64_t since_prev_set_ms =
      (prev_set_ms && now_ms >= prev_set_ms) ? (now_ms - prev_set_ms) : 0;
  bool log_interrupt =
      cvars::gpu_log_interrupts &&
      ShouldLogEvery(call_id, cvars::gpu_log_interrupts_every_n);
  if (log_interrupt) {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    XELOGI(
        "DEBUG: GPU interrupt callback set #{} callback={:08X} user_data={:08X} "
        "thread={}",
        call_id, callback, user_data, oss.str());
  }
  if (cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: GPU callback set: count={} callback={:08X} user_data={:08X} "
        "timestamp_ms={} since_prev_set_ms={}",
        call_id, callback, user_data, now_ms, since_prev_set_ms);
  }
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  last_interrupt_callback_set_ms_.store(now_ms, std::memory_order_relaxed);
  XELOGGPU("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  if (callback && cvars::gpu_dispatch_initial_interrupt_on_set) {
    if (cvars::trace_gpu_bootstrap) {
      XELOGW(
          "DEBUG: GPU callback set: dispatching immediate interrupt "
          "(compatibility mode)");
    }
    DispatchInterruptCallback(0, 2);
  }
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (interrupt_callback_ == 0) {
    if (cvars::trace_gpu_bootstrap) {
      XELOGI(
          "DEBUG: GPU callback dispatch skipped: source={} cpu={} "
          "(no callback registered)",
          source, cpu);
    }
    return;
  }
  if (kernel_state() && cvars::gpu_defer_interrupt_dispatch_during_module_load) {
    auto load_stats = kernel_state()->GetUserModuleLoadStats();
    if (load_stats.load_inflight &&
        load_stats.loaded_executable_user_modules == 0) {
      static std::atomic<uint64_t> deferred_count{0};
      uint64_t defer_id = deferred_count.fetch_add(1) + 1;
      uint64_t now_ms = Clock::QueryHostUptimeMillis();
      uint64_t last_dispatch_ms =
          last_interrupt_dispatch_ms_.load(std::memory_order_relaxed);
      uint64_t dispatch_silence_ms =
          (last_dispatch_ms && now_ms >= last_dispatch_ms)
              ? (now_ms - last_dispatch_ms)
              : 0;
      if (defer_id <= 8 || (defer_id % 120) == 0) {
        XELOGW(
            "DEBUG: GPU interrupt dispatch deferred during module load "
            "(count={} source={} cpu={} callback={:08X} load_inflight={} "
            "load_success={} load_fail={} loaded_exec_modules={} "
            "load_progress_count={} dispatch_silence_ms={})",
            defer_id, source, cpu, interrupt_callback_,
            load_stats.load_inflight ? "YES" : "NO", load_stats.load_success,
            load_stats.load_fail, load_stats.loaded_executable_user_modules,
            load_stats.load_progress_count, dispatch_silence_ms);
      }
      return;
    }
  }
  static std::atomic<uint64_t> dispatch_count{0};
  uint64_t dispatch_id = dispatch_count.fetch_add(1) + 1;
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  uint64_t callback_set_ms =
      last_interrupt_callback_set_ms_.load(std::memory_order_relaxed);
  uint64_t callback_age_ms = (callback_set_ms && now_ms >= callback_set_ms)
                                 ? (now_ms - callback_set_ms)
                                 : 0;
  bool log_interrupt =
      cvars::gpu_log_interrupts &&
      ShouldLogEvery(dispatch_id, cvars::gpu_log_interrupts_every_n);
  if (log_interrupt && cvars::gpu_log_interrupts_require_callback &&
      interrupt_callback_ == 0) {
    log_interrupt = false;
  }
  if (log_interrupt) {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    XELOGI(
        "DEBUG: GPU interrupt dispatch #{} source={} cpu={} callback={:08X} "
        "user_data={:08X} thread={}",
        dispatch_id, source, cpu, interrupt_callback_, interrupt_callback_data_,
        oss.str());
  }
  if (cvars::trace_gpu_bootstrap && interrupt_callback_ != 0) {
    XELOGI(
        "DEBUG: GPU callback dispatch: count={} source={} cpu={} callback={:08X} "
        "user_data={:08X} callback_age_ms={} now_ms={}",
        dispatch_id, source, cpu, interrupt_callback_, interrupt_callback_data_,
        callback_age_ms, now_ms);
  }
  kernel_state()->EmulateCPInterruptDPC(interrupt_callback_,
                                        interrupt_callback_data_, source, cpu);
  last_interrupt_dispatch_ms_.store(now_ms, std::memory_order_relaxed);
}

bool GraphicsSystem::IsReadyForSwap() const {
  if (!command_processor_) {
    return false;
  }
  auto rb_state = command_processor_->GetRingBufferDebugState();
  bool ring_ptr_ready = rb_state.primary_buffer_ptr != 0;
  bool ring_initialized = rb_state.ring_buffer_initialized;
  bool init_acknowledged = rb_state.initialization_acknowledged;
  bool callback_ready = interrupt_callback_ != 0;

  bool ready =
      ring_initialized && init_acknowledged;

  if (cvars::trace_gpu_bootstrap) {
    static std::atomic<uint64_t> ready_check_count{0};
    static std::atomic<uint64_t> last_not_ready_log_ms{0};
    uint64_t check_id = ready_check_count.fetch_add(1) + 1;
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    uint64_t interval_ms =
        static_cast<uint64_t>(cvars::gpu_startup_watch_interval_ms);

    bool should_log = ShouldLogEvery(check_id, 120);
    if (!ready) {
      uint64_t prev_ms = last_not_ready_log_ms.load(std::memory_order_relaxed);
      if (prev_ms == 0 || interval_ms == 0 || now_ms < prev_ms ||
          now_ms - prev_ms >= interval_ms) {
        should_log = true;
        last_not_ready_log_ms.store(now_ms, std::memory_order_relaxed);
      }
    }

    if (should_log) {
      XELOGI(
          "DEBUG: IsReadyForSwap: ready={} ring_initialized={} init_ack={} "
          "ring_ptr_ready={} ring_ptr={:08X} callback_ready={} callback={:08X} "
          "(ring_ptr/callback checks currently bypassed)",
          ready ? "YES" : "NO", ring_initialized ? "YES" : "NO",
          init_acknowledged ? "YES" : "NO", ring_ptr_ready ? "YES" : "NO",
          rb_state.primary_buffer_ptr, callback_ready ? "YES" : "NO",
          interrupt_callback_);
    }
  }

  return ready;
}

uint32_t GraphicsSystem::GetInterruptCallback() const {
  return interrupt_callback_;
}

bool GraphicsSystem::EnsureSystemCommandBufferReady(uint32_t min_size,
                                                    const char* reason) {
  if (min_size < 0x100) {
    min_size = 0x100;
  }

  uint32_t existing_ptr =
      system_command_buffer_ptr_.load(std::memory_order_relaxed);
  uint32_t existing_size =
      system_command_buffer_size_.load(std::memory_order_relaxed);
  if (existing_ptr != 0 && existing_size >= min_size) {
    return true;
  }

  if (!kernel_state_ || !kernel_state_->memory()) {
    XELOGE(
        "RING BUFFER: system command buffer gate failed (reason={}): "
        "kernel_state/memory unavailable",
        reason ? reason : "unknown");
    return false;
  }

  uint32_t allocated_ptr = kernel_state_->memory()->SystemHeapAlloc(
      min_size, 4096, kSystemHeapPhysical);
  if (!allocated_ptr) {
    XELOGE(
        "RING BUFFER: system command buffer allocation failed "
        "(reason={} size=0x{:X})",
        reason ? reason : "unknown", min_size);
    return false;
  }

  system_command_buffer_ptr_.store(allocated_ptr, std::memory_order_relaxed);
  system_command_buffer_size_.store(min_size, std::memory_order_relaxed);
  XELOGI(
      "RING BUFFER: system command buffer ready "
      "(reason={} ptr={:08X} size=0x{:X})",
      reason ? reason : "unknown", allocated_ptr, min_size);
  return true;
}

void GraphicsSystem::SetSystemCommandBuffer(uint32_t ptr, uint32_t size,
                                            const char* reason) {
  if (!ptr || !size) {
    return;
  }

  uint32_t prev_ptr = system_command_buffer_ptr_.load(std::memory_order_relaxed);
  uint32_t prev_size =
      system_command_buffer_size_.load(std::memory_order_relaxed);
  if (prev_ptr == ptr && prev_size == size) {
    return;
  }

  if (prev_ptr == ptr && prev_size > size) {
    size = prev_size;
  }

  system_command_buffer_ptr_.store(ptr, std::memory_order_relaxed);
  system_command_buffer_size_.store(size, std::memory_order_relaxed);
  XELOGI(
      "RING BUFFER: system command buffer updated "
      "(reason={} ptr={:08X} size=0x{:X} prev_ptr={:08X} prev_size=0x{:X})",
      reason ? reason : "unknown", ptr, size, prev_ptr, prev_size);
}

uint32_t GraphicsSystem::GetSystemCommandBuffer() const {
  return system_command_buffer_ptr_.load(std::memory_order_relaxed);
}

uint32_t GraphicsSystem::GetSystemCommandBufferSize() const {
  return system_command_buffer_size_.load(std::memory_order_relaxed);
}

void GraphicsSystem::AttemptFallbackInitialization() {
  bool callback_recovery_prereqs_met = false;
  if (cvars::gpu_no_force_run_profile) {
    bool allow_callback_only_recovery =
        interrupt_callback_ == 0 && command_processor_ != nullptr;
    if (allow_callback_only_recovery) {
      auto rb_dbg = command_processor_->GetRingBufferDebugState();
      bool ring_ready = rb_dbg.ring_buffer_initialized &&
                        rb_dbg.initialization_acknowledged &&
                        rb_dbg.primary_buffer_ptr != 0 &&
                        rb_dbg.primary_buffer_size != 0;
      bool bootstrap_activity_seen =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed) !=
              0 ||
          vd_set_interrupt_callback_state_.count.load(
              std::memory_order_relaxed) != 0 ||
          vd_swap_state_.count.load(std::memory_order_relaxed) != 0 ||
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed) != 0 ||
          mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed) != 0 ||
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed) != 0;
      allow_callback_only_recovery = ring_ready && bootstrap_activity_seen;
      if (allow_callback_only_recovery) {
        callback_recovery_prereqs_met = true;
        XELOGW(
            "DEBUG: no-force fallback callback gate ready: ring+bootstrap "
            "prerequisites are satisfied; waiting for guest callback "
            "registration (ring_ready=YES bootstrap_activity={})",
            bootstrap_activity_seen ? "YES" : "NO");
      }
    }
    if (!allow_callback_only_recovery) {
      static std::atomic<uint64_t> fallback_init_no_force_advisory_last_log_ms{0};
      uint64_t now_ms = Clock::QueryHostUptimeMillis();
      uint64_t interval_ms = static_cast<uint64_t>(
          cvars::gpu_no_force_run_profile_log_interval_ms);
      uint64_t last_log = fallback_init_no_force_advisory_last_log_ms.load(
          std::memory_order_relaxed);
      if (last_log == 0 || interval_ms == 0 || now_ms < last_log ||
          now_ms - last_log >= interval_ms) {
        fallback_init_no_force_advisory_last_log_ms.store(
            now_ms, std::memory_order_relaxed);
        XELOGW(
          "DEBUG: AttemptFallbackInitialization no-force advisory: conditions "
          "not ideal yet, continuing fallback path");
      }
    }
  }
  XELOGI("DEBUG: AttemptFallbackInitialization called");

  // Check if ring buffer needs initialization
  if (command_processor_ && command_processor_->primary_buffer_ptr() == 0) {
    XELOGW("DEBUG: Ring buffer not initialized, forcing default initialization");
    const uint32_t DEFAULT_RB_SIZE_LOG2 = 18;  // 2MB ring buffer
    const uint32_t DEFAULT_RB_SIZE = uint32_t(1) << (DEFAULT_RB_SIZE_LOG2 + 3);
    uint32_t rb_ptr = kernel_state_->memory()->SystemHeapAlloc(
        DEFAULT_RB_SIZE, 4096, kSystemHeapPhysical);
    if (rb_ptr != 0) {
      XELOGI("DEBUG: Allocated fallback ring buffer at {:08X}", rb_ptr);
      command_processor_->InitializeRingBuffer(rb_ptr, DEFAULT_RB_SIZE_LOG2);
    } else {
      XELOGE("DEBUG: Failed to allocate fallback ring buffer");
    }
  }

  // Hard gate policy: fallback init never installs a synthetic callback.
  if (interrupt_callback_ == 0) {
    XELOGW(
        "DEBUG: Interrupt callback still unset after fallback init; "
        "synthetic callback injection is disabled "
        "(callback_recovery_prereqs_met={})",
        callback_recovery_prereqs_met ? "YES" : "NO");
  }
}

void GraphicsSystem::DumpGuestThreadStates(const char* reason) {
  if (!kernel_state_) {
    XELOGW("DEBUG: Thread dump skipped (no kernel state) reason={}",
           reason ? reason : "unknown");
    return;
  }
  auto* object_table = kernel_state_->object_table();
  if (!object_table) {
    XELOGW("DEBUG: Thread dump skipped (no object table) reason={}",
           reason ? reason : "unknown");
    return;
  }
  auto* memory = kernel_state_->memory();
    bool ring_buffer_reason =
      reason &&
      std::string_view(reason).find("RING_BUFFER") != std::string_view::npos;
    bool no_swap_reason =
      reason && std::string_view(reason).find("NO_SWAP") !=
            std::string_view::npos;
    bool gpu_sync_reason =
      reason && std::string_view(reason).find("GPU_SYNC") !=
            std::string_view::npos;
    bool include_wait_snapshot =
      ring_buffer_reason || no_swap_reason || gpu_sync_reason;
  uint32_t wait_watch_object = kernel::xboxkrnl::GetWaitWatchObjectPtr();
  auto threads = object_table->GetObjectsByType<kernel::XThread>();
  size_t total_threads = threads.size();

  // Count thread execution modes for diagnostics.
  // NOTE: All Xenia threads are host-native OS threads; "guest" here means
  // they execute PPC guest code (have a guest execution context).
  size_t guest_exec_count = 0;
  size_t host_exec_count = 0;
  for (const auto& thread : threads) {
    if (thread->is_guest_thread()) {
      guest_exec_count++;
    } else {
      host_exec_count++;
    }
  }

  uint32_t max_threads = cvars::gpu_log_no_swap_threads_max;
  size_t dump_threads = (max_threads == 0)
                            ? total_threads
                            : std::min<size_t>(total_threads, max_threads);
  XELOGW(
      "DEBUG: Thread dump (reason={}): total={} host_threads={} guest_exec={} "
      "host_exec={} dump={}",
      reason ? reason : "unknown", total_threads, total_threads,
      guest_exec_count, host_exec_count, dump_threads);
    if (ring_buffer_reason) {
    XELOGW(
        "RING BUFFER: thread dump begin reason={} total={} guest_exec={} "
        "host_exec={} dump={}",
        reason ? reason : "unknown", total_threads, guest_exec_count,
        host_exec_count, dump_threads);
  }

  // Warning if no guest execution threads are registered yet.
  if (guest_exec_count == 0 && total_threads > 0) {
    XELOGW("DEBUG: Thread dump note: no guest-execution threads registered yet.");
  }

  cpu::Processor* processor = kernel_state_->emulator()
                                  ? kernel_state_->emulator()->processor()
                                  : nullptr;

  for (size_t i = 0; i < dump_threads; ++i) {
    auto thread_ref = threads[i];
    auto* thread = thread_ref.get();
    if (!thread) {
      continue;
    }

    // Classify by execution model:
    // - Guest threads execute PPC code through the JIT.
    // - Host threads execute native emulator code.
    const char* role = "HOST";
    const char* exec_mode = "HOST_NATIVE";
    if (thread->is_guest_thread()) {
      role = thread->main_thread() ? "GUEST_MAIN" : "GUEST";
      exec_mode = "GUEST_JIT";
    } else if (thread->main_thread()) {
      role = "HOST_MAIN";
    }

    // HOST threads run native C++ code and never update PPC registers.
    // Reading their PPCContext gives stale/initial values that are
    // misleading (they reflect stack_base from construction, not
    // actual execution state).  Report them separately.
    if (!thread->is_guest_thread()) {
      XELOGI(
          "DEBUG: thread id={} handle={:08X} role={} exec={} running={} "
          "name='{}' (HOST_NATIVE - no PPC context)",
          thread->thread_id(), thread->handle(), role, exec_mode,
          thread->is_running() ? "YES" : "NO", thread->thread_name());
      if (ring_buffer_reason) {
        XELOGI(
            "RING BUFFER: thread host id={} handle={:08X} role={} running={} "
            "name='{}'",
            thread->thread_id(), thread->handle(), role,
            thread->is_running() ? "YES" : "NO", thread->thread_name());
      }
      continue;
    }

    auto* thread_state = thread->thread_state();
    auto* ctx = thread_state ? thread_state->context() : nullptr;
    uint32_t start_address = thread->start_address();
    uint32_t pc = ctx ? ctx->current_pc : 0;
    uint32_t lr = ctx ? static_cast<uint32_t>(ctx->lr) : 0;
    uint32_t ctr = ctx ? static_cast<uint32_t>(ctx->ctr) : 0;
    uint32_t r1 = ctx ? static_cast<uint32_t>(ctx->r[1]) : 0;

    const char* module_name = "<unknown>";
    const char* function_name = "<unknown>";
    uint32_t function_address = 0;
    if (processor && pc) {
      auto module = processor->LookupModule(pc);
      if (module) {
        module_name = module->name().c_str();
      }
      auto function = processor->LookupFunction(pc);
      if (function) {
        function_name = function->name().c_str();
        function_address = function->address();
      }
    }

    const char* start_module_name = "<unknown>";
    const char* start_function_name = "<unknown>";
    uint32_t start_function_address = 0;
    if (processor && start_address) {
      auto start_module = processor->LookupModule(start_address);
      if (start_module) {
        start_module_name = start_module->name().c_str();
      }
      auto start_function = processor->LookupFunction(start_address);
      if (start_function) {
        start_function_name = start_function->name().c_str();
        start_function_address = start_function->address();
      }
    }

    const char* context_state =
        !ctx ? "NO_CONTEXT"
             : ((pc == 0 && lr == 0 && ctr == 0) ? "ZERO_REGISTERS"
                                                 : "VALID");

    uint32_t context_state_code =
        !ctx ? 0u : ((pc == 0 && lr == 0 && ctr == 0) ? 1u : 2u);

    if (thread->main_thread()) {
      static std::atomic<uint32_t> last_main_context_state{UINT32_MAX};
      static std::atomic<uint64_t> main_context_transition_count{0};
      static std::atomic<uint64_t> main_zero_state_log_count{0};
      static std::atomic<uint64_t> first_main_zero_ms{0};

      uint32_t prev_state =
          last_main_context_state.exchange(context_state_code,
                                           std::memory_order_relaxed);
      if (prev_state != context_state_code) {
        uint64_t transition_id =
            main_context_transition_count.fetch_add(1,
                                                    std::memory_order_relaxed) +
            1;
        XELOGI(
            "GUEST MAIN THREAD: Context state transition #{} {} -> {} "
            "(pc={:08X}, lr={:08X}, ctr={:08X}, start={:08X})",
            transition_id,
            prev_state == 0 ? "NO_CONTEXT"
                            : (prev_state == 1 ? "ZERO_REGISTERS"
                                               : (prev_state == 2 ? "VALID"
                                                                  : "UNKNOWN")),
            context_state, pc, lr, ctr, start_address);
        if (context_state_code == 1) {
          first_main_zero_ms.store(Clock::QueryHostUptimeMillis(),
                                   std::memory_order_relaxed);
        } else if (context_state_code == 2) {
          uint64_t first_zero =
              first_main_zero_ms.load(std::memory_order_relaxed);
          if (first_zero != 0) {
            uint64_t now_ms = Clock::QueryHostUptimeMillis();
            XELOGI(
                "GUEST MAIN THREAD: Context became valid after {}ms in "
                "ZERO_REGISTERS state.",
                now_ms >= first_zero ? (now_ms - first_zero) : 0);
            first_main_zero_ms.store(0, std::memory_order_relaxed);
          }
        }
      }

      if (context_state_code == 1 && start_address != 0) {
        uint64_t zero_log_id =
            main_zero_state_log_count.fetch_add(1, std::memory_order_relaxed) +
            1;
        if (zero_log_id <= 3 || (zero_log_id % 120) == 0) {
          uint64_t first_zero =
              first_main_zero_ms.load(std::memory_order_relaxed);
          uint64_t now_ms = Clock::QueryHostUptimeMillis();
          uint64_t zero_age_ms =
              first_zero != 0 && now_ms >= first_zero ? (now_ms - first_zero)
                                                      : 0;
          XELOGW(
              "GUEST MAIN THREAD: Context is still ZERO_REGISTERS "
              "(age={}ms, start={:08X}, start_module={}, "
              "start_fn={}({:08X})).",
              zero_age_ms, start_address, start_module_name,
              start_function_name, start_function_address);
        }
      }

    }

    XELOGI(
        "DEBUG: thread id={} handle={:08X} role={} exec={} main={} running={} "
        "start={:08X} cpu={} pc={:08X} lr={:08X} ctr={:08X} r1={:08X} "
        "context_state={} module={} fn={}({:08X}) start_module={} "
        "start_fn={}({:08X})",
        thread->thread_id(), thread->handle(), role, exec_mode,
        thread->main_thread() ? "YES" : "NO",
        thread->is_running() ? "YES" : "NO", start_address,
        static_cast<uint32_t>(thread->active_cpu()), pc, lr, ctr, r1,
        context_state, module_name, function_name, function_address,
        start_module_name, start_function_name, start_function_address);

    if (thread->main_thread() && thread->is_running() && ctx && pc == 0 &&
        start_address != 0) {
      XELOGW(
          "GUEST MAIN THREAD: Runtime context registers are zero in snapshot; "
          "using start address as fallback anchor (start={:08X}, "
          "start_module={}, start_fn={}({:08X})).",
          start_address, start_module_name, start_function_name,
          start_function_address);
    }
    if (ring_buffer_reason) {
      uint32_t guest_kthread_ptr = thread->guest_object();
      XELOGI(
          "RING BUFFER: thread guest id={} handle={:08X} guest_kthread={:08X} "
          "main={} running={} context_state={} pc={:08X} lr={:08X} "
          "fn={}({:08X}) start={:08X} start_fn={}({:08X})",
          thread->thread_id(), thread->handle(), guest_kthread_ptr,
          thread->main_thread() ? "YES" : "NO",
          thread->is_running() ? "YES" : "NO", context_state, pc, lr,
          function_name, function_address, start_address, start_function_name,
          start_function_address);

      if (!memory) {
        XELOGW(
            "RING BUFFER: thread wait snapshot unavailable id={} "
            "(kernel memory null)",
            thread->thread_id());
        continue;
      }

      auto kthread_result =
          memory->TranslateVirtualSafe<kernel::X_KTHREAD*>(guest_kthread_ptr);
      if (!kthread_result.success || !kthread_result.pointer) {
        XELOGW(
            "RING BUFFER: thread wait snapshot unavailable id={} "
            "guest_kthread={:08X} error={}",
            thread->thread_id(), guest_kthread_ptr, kthread_result.error);
        continue;
      }
      const kernel::X_KTHREAD* guest_kthread = kthread_result.pointer;
      uint8_t kthread_state = guest_kthread->thread_state;
      uint32_t wait_result = static_cast<uint32_t>(guest_kthread->wait_result);
      uint8_t wait_irql = guest_kthread->wait_irql;
      uint8_t alertable = guest_kthread->alertable;
      uint16_t wait_type_raw = guest_kthread->wait_timeout_block.wait_type;
      uint16_t wait_result_xstatus =
          guest_kthread->wait_timeout_block.wait_result_xstatus;
      uint32_t wait_object_ptr = guest_kthread->wait_timeout_block.object.m_ptr;
      const uint32_t default_wait_timeout_object_ptr =
          guest_kthread_ptr +
          static_cast<uint32_t>(offsetof(kernel::X_KTHREAD, wait_timeout_timer));
      bool wait_timeout_template =
          wait_object_ptr == default_wait_timeout_object_ptr &&
          wait_type_raw == 0x0201 && wait_result_xstatus == 0x0100;
      bool wait_object_mapped = false;
      uint8_t wait_object_type = 0xFF;
      int32_t wait_object_signal_state = 0;
      uint32_t wait_object_stashed_handle = 0;
      bool wait_object_stashed_lookup = false;
      const char* wait_object_stashed_type = "N/A";
      std::string wait_object_stashed_name = "<none>";
      if (wait_object_ptr && !wait_timeout_template) {
        auto wait_object_result = memory->TranslateVirtualSafe<kernel::X_DISPATCH_HEADER*>(
            wait_object_ptr);
        if (wait_object_result.success && wait_object_result.pointer) {
          wait_object_mapped = true;
          wait_object_type = wait_object_result.pointer->type;
          wait_object_signal_state =
              static_cast<int32_t>(wait_object_result.pointer->signal_state);
          if (wait_object_result.pointer->wait_list.flink_ptr ==
              kernel::kXObjSignature) {
            wait_object_stashed_handle =
                wait_object_result.pointer->wait_list.blink_ptr;
            auto wait_object_ref =
                object_table->LookupObject<kernel::XObject>(
                    wait_object_stashed_handle);
            if (wait_object_ref) {
              wait_object_stashed_lookup = true;
              wait_object_stashed_type =
                  GetKernelObjectTypeName(wait_object_ref->type());
              if (!wait_object_ref->name().empty()) {
                wait_object_stashed_name = wait_object_ref->name();
              }
            }
          }
        }
      }
      XELOGI(
          "RING BUFFER: thread wait id={} handle={:08X} state={}({}) "
          "wait_result={:08X} wait_irql={} alertable={} wait_obj={:08X} "
          "wait_obj_mapped={} wait_obj_type={}({}) wait_signal={} "
          "wait_type_raw={:04X} wait_type={} wait_xstatus={:04X} "
          "wait_template_default={} wait_template_obj={:08X} "
          "wait_obj_handle_hint={:08X} wait_obj_handle_lookup={} "
          "wait_obj_kernel_type={} wait_obj_name='{}' "
          "wait_watch_obj={:08X} wait_watch_match={}",
          thread->thread_id(), thread->handle(), GetKthreadStateName(kthread_state),
          kthread_state, wait_result, wait_irql, alertable, wait_object_ptr,
          wait_object_mapped ? "YES" : "NO", wait_object_type,
          wait_object_mapped ? GetDispatcherTypeName(wait_object_type) : "N/A",
          wait_object_signal_state, wait_type_raw, GetWaitTypeName(wait_type_raw),
          wait_result_xstatus, wait_timeout_template ? "YES" : "NO",
          default_wait_timeout_object_ptr, wait_object_stashed_handle,
          wait_object_stashed_lookup ? "YES" : "NO",
          wait_object_stashed_type, wait_object_stashed_name, wait_watch_object,
          wait_watch_object != 0 && wait_object_ptr == wait_watch_object
              ? "YES"
              : "NO");
      if (wait_timeout_template) {
        XELOGI(
            "RING BUFFER: thread wait snapshot note: KTHREAD wait_timeout_block "
            "matches default initialization template "
            "(wait_timeout_timer={:08X}); this does not indicate an active "
            "guest object wait. Rely on Ke/NtWait* instrumentation for active "
            "wait diagnostics.",
            default_wait_timeout_object_ptr);
      }
    } else if (include_wait_snapshot) {
      uint32_t guest_kthread_ptr = thread->guest_object();
      if (!memory) {
        XELOGW(
            "THREAD WAIT: snapshot unavailable id={} guest_kthread={:08X} "
            "reason={} (kernel memory null)",
            thread->thread_id(), guest_kthread_ptr,
            reason ? reason : "unknown");
        continue;
      }

      auto kthread_result =
          memory->TranslateVirtualSafe<kernel::X_KTHREAD*>(guest_kthread_ptr);
      if (!kthread_result.success || !kthread_result.pointer) {
        XELOGW(
            "THREAD WAIT: snapshot unavailable id={} guest_kthread={:08X} "
            "reason={} error={}",
            thread->thread_id(), guest_kthread_ptr,
            reason ? reason : "unknown", kthread_result.error);
        continue;
      }
      const kernel::X_KTHREAD* guest_kthread = kthread_result.pointer;
      uint8_t kthread_state = guest_kthread->thread_state;
      uint32_t wait_result = static_cast<uint32_t>(guest_kthread->wait_result);
      uint8_t wait_irql = guest_kthread->wait_irql;
      uint8_t alertable = guest_kthread->alertable;
      uint16_t wait_type_raw = guest_kthread->wait_timeout_block.wait_type;
      uint16_t wait_result_xstatus =
          guest_kthread->wait_timeout_block.wait_result_xstatus;
      uint32_t wait_object_ptr = guest_kthread->wait_timeout_block.object.m_ptr;
      const uint32_t default_wait_timeout_object_ptr =
          guest_kthread_ptr +
          static_cast<uint32_t>(offsetof(kernel::X_KTHREAD, wait_timeout_timer));
      bool wait_timeout_template =
          wait_object_ptr == default_wait_timeout_object_ptr &&
          wait_type_raw == 0x0201 && wait_result_xstatus == 0x0100;
      bool wait_object_mapped = false;
      uint8_t wait_object_type = 0xFF;
      int32_t wait_object_signal_state = 0;
      uint32_t wait_object_stashed_handle = 0;
      bool wait_object_stashed_lookup = false;
      const char* wait_object_stashed_type = "N/A";
      std::string wait_object_stashed_name = "<none>";
      if (wait_object_ptr && !wait_timeout_template) {
        auto wait_object_result =
            memory->TranslateVirtualSafe<kernel::X_DISPATCH_HEADER*>(
                wait_object_ptr);
        if (wait_object_result.success && wait_object_result.pointer) {
          wait_object_mapped = true;
          wait_object_type = wait_object_result.pointer->type;
          wait_object_signal_state =
              static_cast<int32_t>(wait_object_result.pointer->signal_state);
          if (wait_object_result.pointer->wait_list.flink_ptr ==
              kernel::kXObjSignature) {
            wait_object_stashed_handle =
                wait_object_result.pointer->wait_list.blink_ptr;
            auto wait_object_ref =
                object_table->LookupObject<kernel::XObject>(
                    wait_object_stashed_handle);
            if (wait_object_ref) {
              wait_object_stashed_lookup = true;
              wait_object_stashed_type =
                  GetKernelObjectTypeName(wait_object_ref->type());
              if (!wait_object_ref->name().empty()) {
                wait_object_stashed_name = wait_object_ref->name();
              }
            }
          }
        }
      }
      XELOGI(
          "THREAD WAIT: id={} handle={:08X} reason={} kthread_state={}({}) "
          "wait_result={:08X} wait_irql={} alertable={} wait_type_raw={} "
          "wait_type={} wait_result_xstatus={:04X} wait_object={:08X} "
          "wait_obj_mapped={} wait_obj_type={}({}) wait_signal={} "
          "wait_template_default={} wait_template_obj={:08X} "
          "wait_obj_handle_hint={:08X} wait_obj_handle_lookup={} "
          "wait_obj_kernel_type={} wait_obj_name='{}' "
          "wait_watch_obj={:08X} wait_watch_match={}",
          thread->thread_id(), thread->handle(), reason ? reason : "unknown",
          kthread_state, GetKthreadStateName(kthread_state), wait_result,
          wait_irql, alertable, wait_type_raw, GetWaitTypeName(wait_type_raw),
          wait_result_xstatus, wait_object_ptr,
          wait_object_mapped ? "YES" : "NO", wait_object_type,
          wait_object_mapped ? GetDispatcherTypeName(wait_object_type) : "N/A",
          wait_object_signal_state, wait_timeout_template ? "YES" : "NO",
          default_wait_timeout_object_ptr, wait_object_stashed_handle,
          wait_object_stashed_lookup ? "YES" : "NO",
          wait_object_stashed_type, wait_object_stashed_name, wait_watch_object,
          wait_watch_object != 0 && wait_object_ptr == wait_watch_object
              ? "YES"
              : "NO");

      if (wait_timeout_template) {
        XELOGI(
            "THREAD WAIT: id={} reason={} wait_timeout_block matches default "
            "template (wait_timeout_timer={:08X}); this is not an active "
            "guest wait object. Rely on Ke/NtWait* instrumentation for active "
            "waits.",
            thread->thread_id(), reason ? reason : "unknown",
            default_wait_timeout_object_ptr);
      }
    }
  }

  if (dump_threads < total_threads) {
    XELOGW("DEBUG: Thread dump truncated: {} threads omitted (max={})",
           total_threads - dump_threads, max_threads);
    if (ring_buffer_reason) {
      XELOGW("RING BUFFER: thread dump truncated omitted={} max={}",
             total_threads - dump_threads, max_threads);
    }
  }
  else {
    XELOGI("DEBUG: Thread dump complete: {} threads", dump_threads);
    if (ring_buffer_reason) {
      XELOGI("RING BUFFER: thread dump complete: {} threads", dump_threads);
    }
  }
}

void GraphicsSystem::DumpNoSwapDebugState(uint64_t since_first_vblank,
                                          uint32_t read_ptr,
                                          uint32_t write_ptr) {
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  std::string no_swap_stall_state;
  uint32_t no_swap_stall_repeat_samples = 0;
  uint64_t no_swap_stall_sample_ms = 0;
  uint64_t no_swap_stall_age_ms = 0;
  auto is_pre_bootstrap_no_swap_stall =
      [](const std::string& state) -> bool {
    return state == "guest_main_context_not_ready" ||
           state == "startup_prolog_helper_only" ||
           state == "gpu_bootstrap_entry_loop_before_callback" ||
           state == "title_local_privilege_gate_before_gpu_dispatch" ||
           state == "title_local_busy_loop_without_import_dispatch" ||
           state == "title_local_busy_loop_before_gpu_dispatch" ||
           state == "busy_loop_before_gpu_dispatch";
  };
  bool no_swap_stall_expect_never = false;
  if (kernel_state_) {
    auto* emulator = kernel_state_->emulator();
    if (emulator &&
        emulator->GetLastGpuDispatchStallSnapshot(
            &no_swap_stall_state, nullptr, nullptr, nullptr,
            &no_swap_stall_repeat_samples, &no_swap_stall_sample_ms) &&
        no_swap_stall_sample_ms != 0 && now_ms >= no_swap_stall_sample_ms) {
      no_swap_stall_age_ms = now_ms - no_swap_stall_sample_ms;
      no_swap_stall_expect_never =
          is_pre_bootstrap_no_swap_stall(no_swap_stall_state);
    }
  }
  if (no_swap_stall_expect_never) {
    XELOGW(
        "DEBUG: NO SWAP context: guest stall_state={} age_ms={} "
        "repeat_samples={}; MMIO/Vd '<never>' snapshots remain expected "
        "until the title exits this pre-bootstrap path",
        no_swap_stall_state, no_swap_stall_age_ms,
        no_swap_stall_repeat_samples);
  }

  if (cvars::gpu_log_no_swap_dump_mmio_state) {
    auto dump_mmio = [now_ms, no_swap_stall_expect_never,
                      &no_swap_stall_state](const char* label,
                                            const MmioWriteState& state) {
      uint64_t count = state.count.load(std::memory_order_relaxed);
      uint64_t last_ms = state.time_ms.load(std::memory_order_relaxed);
      if (count == 0) {
        if (no_swap_stall_expect_never) {
          XELOGI("   mmio {}: <never> (expected while guest stall_state={})",
                 label, no_swap_stall_state);
        } else {
          XELOGI("   mmio {}: <never>", label);
        }
        return;
      }
      uint64_t age_ms = (last_ms && now_ms >= last_ms) ? (now_ms - last_ms) : 0;
      XELOGI(
          "   mmio {}: count={} value={:08X} pc={:08X} thread_id={} "
          "age_ms={}",
          label, count, state.value.load(std::memory_order_relaxed),
          state.pc.load(std::memory_order_relaxed),
          state.thread_id.load(std::memory_order_relaxed), age_ms);
    };
    XELOGW("DEBUG: NO SWAP: MMIO write state snapshot");
    dump_mmio("CP_RB_BASE", mmio_cp_rb_base_);
    dump_mmio("CP_RB_CNTL", mmio_cp_rb_cntl_);
    dump_mmio("CP_RB_RPTR_ADDR", mmio_cp_rb_rptr_addr_);
    dump_mmio("CP_RB_RPTR", mmio_cp_rb_rptr_);
    dump_mmio("CP_RB_WPTR", mmio_cp_rb_wptr_);
    dump_mmio("CP_RB_WPTR_DELAY", mmio_cp_rb_wptr_delay_);
    dump_mmio("CP_RB_RPTR_WR", mmio_cp_rb_rptr_wr_);
    dump_mmio("FETCH0_WORD", mmio_fetch0_);

    uint64_t cp_mmio_total =
      mmio_cp_rb_base_.count.load(std::memory_order_relaxed) +
      mmio_cp_rb_cntl_.count.load(std::memory_order_relaxed) +
      mmio_cp_rb_rptr_addr_.count.load(std::memory_order_relaxed) +
      mmio_cp_rb_rptr_.count.load(std::memory_order_relaxed) +
      mmio_cp_rb_wptr_.count.load(std::memory_order_relaxed) +
      mmio_cp_rb_wptr_delay_.count.load(std::memory_order_relaxed) +
      mmio_cp_rb_rptr_wr_.count.load(std::memory_order_relaxed);
    uint64_t cp_mmio_guest =
      mmio_cp_guest_write_count_.load(std::memory_order_relaxed);
    uint64_t cp_mmio_host =
      mmio_cp_host_write_count_.load(std::memory_order_relaxed);
    XELOGI("   mmio CP_RB origin counters: guest={} host={}", cp_mmio_guest,
           cp_mmio_host);
    if (cp_mmio_total == 0) {
      uint64_t mmio_any =
        mmio_any_write_count_.load(std::memory_order_relaxed);
      uint64_t mmio_non_cp =
        mmio_non_cp_write_count_.load(std::memory_order_relaxed);
      uint32_t mmio_first_reg =
        mmio_any_first_reg_.load(std::memory_order_relaxed);
      uint32_t mmio_last_reg =
        mmio_any_last_reg_.load(std::memory_order_relaxed);
      uint32_t mmio_last_value =
        mmio_any_last_value_.load(std::memory_order_relaxed);
      uint32_t mmio_last_pc =
        mmio_any_last_pc_.load(std::memory_order_relaxed);
      uint32_t mmio_last_thread_id =
        mmio_any_last_thread_id_.load(std::memory_order_relaxed);
      XELOGW(
        "DEBUG: NO SWAP MMIO fallback: tracked CP_RB MMIO counters are <never>; "
        "global MMIO activity total={} non_cp={}",
        mmio_any, mmio_non_cp);
      XELOGI(
        "   mmio_any first_reg={:04X} last_reg={:04X} "
        "last_value={:08X} last_pc={:08X} last_thread_id={}",
        mmio_first_reg == 0xFFFFFFFFu ? 0u : mmio_first_reg, mmio_last_reg,
        mmio_last_value, mmio_last_pc, mmio_last_thread_id);
      if (register_file_) {
      XELOGI(
        "   cp_register_file snapshot: CP_RB_BASE={:08X} CP_RB_CNTL={:08X} "
        "CP_RB_RPTR_ADDR={:08X} CP_RB_RPTR={:08X} CP_RB_WPTR={:08X} "
        "CP_RB_WPTR_DELAY={:08X} CP_RB_RPTR_WR={:08X}",
        register_file_->values[0x01C0], register_file_->values[0x01C1],
        register_file_->values[0x01C3], register_file_->values[0x01C4],
        register_file_->values[0x01C5], register_file_->values[0x01C6],
        register_file_->values[0x01C7]);
      }
    }
  }

  if (cvars::gpu_log_no_swap_dump_vd_state) {
    auto dump_vd = [now_ms, no_swap_stall_expect_never,
                    &no_swap_stall_state](const char* label,
                                          const VdCallState& state) {
      uint64_t count = state.count.load(std::memory_order_relaxed);
      uint64_t last_ms = state.time_ms.load(std::memory_order_relaxed);
      if (count == 0) {
        if (no_swap_stall_expect_never) {
          XELOGI("   vd {}: <never> (expected while guest stall_state={})",
                 label, no_swap_stall_state);
        } else {
          XELOGI("   vd {}: <never>", label);
        }
        return;
      }
      uint64_t age_ms = (last_ms && now_ms >= last_ms) ? (now_ms - last_ms) : 0;
      XELOGI("   vd {}: count={} pc={:08X} thread_id={} age_ms={}", label,
             count, state.pc.load(std::memory_order_relaxed),
             state.thread_id.load(std::memory_order_relaxed), age_ms);
    };
    XELOGW("DEBUG: NO SWAP: Vd* call state snapshot");
    dump_vd("VdSwap", vd_swap_state_);
    dump_vd("VdInitializeEngines", vd_init_engines_state_);
    dump_vd("VdGetGraphicsAsicID", vd_get_graphics_asic_id_state_);
    dump_vd("VdInitializeRingBuffer", vd_init_ring_buffer_state_);
    dump_vd("VdEnableRingBufferRPtrWriteBack", vd_enable_rptr_writeback_state_);
    dump_vd("VdSetGraphicsInterruptCallback", vd_set_interrupt_callback_state_);
    dump_vd("VdGetSystemCommandBuffer", vd_get_system_cmd_buffer_state_);
    dump_vd("VdInitializeEDRAM", vd_init_edram_state_);
    dump_vd("VdRetrainEDRAM", vd_retrain_edram_state_);
    dump_vd("VdRetrainEDRAMWorker", vd_retrain_edram_worker_state_);
    dump_vd("VdIsHSIOTrainingSucceeded", vd_hsio_training_succeeded_state_);

    auto runtime_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
    auto find_runtime_stat = [&runtime_stats](uint16_t ordinal)
      -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
      for (const auto& stat : runtime_stats.ordinal_runtime_stats) {
      if (stat.ordinal == ordinal) {
        return &stat;
      }
      }
      return nullptr;
    };
    auto dump_vd_runtime_fallback =
      [&](const char* label, uint16_t ordinal, uint64_t vd_count) {
        const auto* stat = find_runtime_stat(ordinal);
        if (!stat) {
        return;
        }
        uint64_t runtime_total = stat->dynamic_lookup_calls +
                     stat->static_thunk_calls +
                     stat->export_entry_calls;
        if (vd_count == 0 && runtime_total > 0) {
        XELOGW(
          "   vd {}: <never> but runtime fallback saw activity "
          "(ordinal=0x{:03X} dynamic={} static_thunk={} "
          "export_entry={} total={})",
          label, static_cast<uint32_t>(ordinal),
          stat->dynamic_lookup_calls, stat->static_thunk_calls,
          stat->export_entry_calls, runtime_total);
        }
      };

    dump_vd_runtime_fallback(
      "VdSwap", 0x025B,
      vd_swap_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdInitializeEngines", 0x01C2,
      vd_init_engines_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdGetGraphicsAsicID", 0x01BC,
      vd_get_graphics_asic_id_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdInitializeRingBuffer", 0x01C3,
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdEnableRingBufferRPtrWriteBack", 0x01B6,
      vd_enable_rptr_writeback_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdSetGraphicsInterruptCallback", 0x01D5,
      vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdGetSystemCommandBuffer", 0x01BD,
      vd_get_system_cmd_buffer_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdInitializeEDRAM", 0x0268,
      vd_init_edram_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdRetrainEDRAM", 0x0269,
      vd_retrain_edram_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdRetrainEDRAMWorker", 0x026A,
      vd_retrain_edram_worker_state_.count.load(std::memory_order_relaxed));
    dump_vd_runtime_fallback(
      "VdIsHSIOTrainingSucceeded", 0x01C6,
      vd_hsio_training_succeeded_state_.count.load(std::memory_order_relaxed));

    if (runtime_stats.proc_lookup_calls || runtime_stats.proc_lookup_resolved ||
      runtime_stats.proc_lookup_missing) {
      XELOGI(
        "   vd runtime lookup totals: module_handle_calls={} "
        "module_handle_failures={} proc_lookup_calls={} resolved={} "
        "missing={} invalid_module={} fallback_hits={}",
        runtime_stats.module_handle_calls,
        runtime_stats.module_handle_failures,
        runtime_stats.proc_lookup_calls, runtime_stats.proc_lookup_resolved,
        runtime_stats.proc_lookup_missing,
        runtime_stats.proc_lookup_invalid_module,
        runtime_stats.proc_lookup_fallback_hits);
    }
  }

  if (cvars::gpu_log_no_swap_dump_ring_watch && command_processor_) {
    uint64_t hits = command_processor_->ring_buffer_watch_hits();
    uint64_t last_write_ms = command_processor_->ring_buffer_last_write_ms();
    uint32_t last_write_phys =
        command_processor_->ring_buffer_last_write_phys();
    uint64_t age_ms = (last_write_ms && now_ms >= last_write_ms)
                          ? (now_ms - last_write_ms)
                          : 0;
    XELOGW(
        "DEBUG: NO SWAP: ring write watch hits={} last_phys={:08X} "
        "last_age_ms={} rb_read={:08X} rb_write={:08X}",
        hits, last_write_phys, age_ms, read_ptr, write_ptr);
  }

  if (cvars::gpu_log_no_swap_dump_ring_watch && command_processor_) {
    auto dbg = command_processor_->GetRingBufferDebugState();
    XELOGW("DEBUG: NO SWAP: CommandProcessor debug snapshot");
    XELOGI(
      "   cp.worker_running={} setup_complete={} setup_failed={} "
      "ring_buffer_initialized={} init_ack={}",
           dbg.worker_running ? "YES" : "NO",
         dbg.worker_setup_complete ? "YES" : "NO",
         dbg.worker_setup_failed ? "YES" : "NO",
           dbg.ring_buffer_initialized ? "YES" : "NO",
           dbg.initialization_acknowledged ? "YES" : "NO");
    XELOGI("   cp.primary_buffer_ptr=0x{:08X} size=0x{:08X}",
           dbg.primary_buffer_ptr, dbg.primary_buffer_size);
    XELOGI(
        "   cp.read_ptr=0x{:08X} write_ptr=0x{:08X} rptr_wb=0x{:08X} "
        "rptr_freq=0x{:08X}",
        dbg.read_ptr_index, dbg.write_ptr_index, dbg.read_ptr_writeback_ptr,
        dbg.read_ptr_update_freq);
    XELOGI("   cp.swap_packets={} last_swap_ms={} frontbuffer=0x{:08X}",
           dbg.swap_packet_count, dbg.last_swap_packet_ms,
           dbg.last_swap_frontbuffer_ptr);
    XELOGI("   cp.swap_size={}x{}", dbg.last_swap_width, dbg.last_swap_height);
    uint64_t host_debug_swap_age =
        (dbg.debug_host_swap_last_ms && now_ms >= dbg.debug_host_swap_last_ms)
            ? (now_ms - dbg.debug_host_swap_last_ms)
            : 0;
    XELOGI(
        "   cp.host_debug_swaps total={} ring={} direct={} last_ms={} "
        "last_age_ms={} frontbuffer=0x{:08X} size={}x{}",
        dbg.debug_host_swap_injection_count,
        dbg.debug_host_swap_ring_injection_count,
        dbg.debug_host_swap_direct_issue_count, dbg.debug_host_swap_last_ms,
        host_debug_swap_age, dbg.debug_host_swap_last_frontbuffer_ptr,
        dbg.debug_host_swap_last_width, dbg.debug_host_swap_last_height);
    uint64_t ring_watch_last_arm_age =
        (dbg.ring_buffer_watch_last_arm_ms &&
         now_ms >= dbg.ring_buffer_watch_last_arm_ms)
            ? (now_ms - dbg.ring_buffer_watch_last_arm_ms)
            : 0;
    XELOGI(
        "   cp.ring_watch_hits={} arms={} last_arm_ms={} last_arm_age_ms={} "
        "last_write_ms={} last_write_phys=0x{:08X} rearm_pending={}",
        dbg.ring_buffer_watch_hits, dbg.ring_buffer_watch_arm_count,
        dbg.ring_buffer_watch_last_arm_ms, ring_watch_last_arm_age,
        dbg.ring_buffer_last_write_ms, dbg.ring_buffer_last_write_phys,
        dbg.ring_buffer_watch_rearm_requested ? "YES" : "NO");
    uint64_t ring_watch_transition_age =
      (dbg.ring_watch_last_transition_ms &&
       now_ms >= dbg.ring_watch_last_transition_ms)
        ? (now_ms - dbg.ring_watch_last_transition_ms)
        : 0;
    XELOGI(
      "   cp.ring_watch_transition phase_id={} range=[0x{:08X}..0x{:08X}) "
      "size=0x{:08X} transition_ms={} transition_age_ms={} "
      "guest_write_enabled={} restore_verify_pending={} force_rw_mode={}",
      dbg.ring_watch_last_phase_id, dbg.ring_watch_last_range_start,
      dbg.ring_watch_last_range_start + dbg.ring_watch_last_range_size,
      dbg.ring_watch_last_range_size, dbg.ring_watch_last_transition_ms,
      ring_watch_transition_age,
      dbg.ring_watch_last_guest_write_enabled ? "YES" : "NO",
      dbg.ring_watch_restore_verify_pending ? "YES" : "NO",
      dbg.ring_watch_force_rw_mode ? "YES" : "NO");
    XELOGI("   cp.events: write_ptr_event={} init_complete_event={}",
           dbg.has_write_ptr_event ? "YES" : "NO",
           dbg.has_init_complete_event ? "YES" : "NO");
    XELOGI(
      "   cp.init_diag: request_count={} ack_transitions={} "
      "last_request_ms={} last_signal_ms={} last_ack_ms={}",
      dbg.init_request_count, dbg.init_ack_transition_count,
      dbg.last_init_request_ms, dbg.last_init_signal_ms,
      dbg.last_init_ack_ms);

    uint64_t vd_swap_count =
      vd_swap_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_init_rb_count =
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_wptr_count =
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
    if (dbg.swap_packet_count > 0 && vd_swap_count == 0 &&
      vd_init_rb_count == 0 && mmio_rb_wptr_count == 0) {
      XELOGW(
        "DEBUG: NO SWAP diagnosis hint: CP consumed swap packets, but tracked "
        "guest Vd/MMIO bootstrap counters are still <never>. This suggests "
        "an instrumentation-path mismatch (imports/thunks/MMIO trap path) "
        "rather than pure ring inactivity.");
    }
    if (dbg.ring_buffer_watch_arm_count > 0 && dbg.ring_buffer_watch_hits == 0 &&
      dbg.ring_buffer_last_write_ms == 0) {
      XELOGW(
        "DEBUG: NO SWAP diagnosis hint: ring watch is arming but no write "
        "fault callbacks have fired yet. Compare watch-protect host_access "
        "with guest_protect in the watch-arm logs to validate actual "
        "mprotect state.");
    }
  }

  if (command_processor_) {
    auto dbg = command_processor_->GetRingBufferDebugState();
    uint64_t mmio_rb_base_writes =
        mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_cntl_writes =
        mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_wptr_writes =
        mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
    uint64_t wptr_update_total =
        wptr_update_count_.load(std::memory_order_relaxed);
    uint64_t wptr_source_mmio =
        wptr_source_mmio_count_.load(std::memory_order_relaxed);
    uint64_t wptr_source_cp_packet =
        wptr_source_cp_packet_count_.load(std::memory_order_relaxed);
    uint64_t wptr_source_debug_swap =
        wptr_source_debug_swap_count_.load(std::memory_order_relaxed);
    uint64_t wptr_source_other =
        wptr_source_other_count_.load(std::memory_order_relaxed);
    uint64_t wptr_guest_driven_count =
        wptr_source_mmio + wptr_source_cp_packet;
    uint32_t wptr_last_source_id =
        last_wptr_source_id_.load(std::memory_order_relaxed);
    uint64_t vd_init_engines_count =
        vd_init_engines_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_asic_id_count =
        vd_get_graphics_asic_id_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_init_count =
        vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_cb_count =
        vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_swap_count = vd_swap_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_init_edram_count =
        vd_init_edram_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_retrain_edram_count =
        vd_retrain_edram_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_retrain_worker_count =
        vd_retrain_edram_worker_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_hsio_count =
        vd_hsio_training_succeeded_state_.count.load(std::memory_order_relaxed);
    auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
    auto find_ordinal_stat = [&](uint16_t ordinal)
      -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
      for (const auto& stat : proc_stats.ordinal_runtime_stats) {
      if (stat.ordinal == ordinal) {
        return &stat;
      }
      }
      return nullptr;
    };
    const auto* stat_01C3 = find_ordinal_stat(0x01C3);
    const auto* stat_01D5 = find_ordinal_stat(0x01D5);
    const auto* stat_025B = find_ordinal_stat(0x025B);
    bool guest_main_dispatch_bridge_seen =
        proc_stats.guest_main_dispatch_bridge_calls_total != 0;
    uint64_t runtime_bootstrap_call_total =
      (stat_01C3 ? (stat_01C3->dynamic_lookup_calls +
              stat_01C3->static_thunk_calls +
              stat_01C3->export_entry_calls)
             : 0) +
      (stat_01D5 ? (stat_01D5->dynamic_lookup_calls +
              stat_01D5->static_thunk_calls +
              stat_01D5->export_entry_calls)
             : 0) +
      (stat_025B ? (stat_025B->dynamic_lookup_calls +
              stat_025B->static_thunk_calls +
              stat_025B->export_entry_calls)
             : 0);
    uint64_t mmio_cp_guest_writes =
      mmio_cp_guest_write_count_.load(std::memory_order_relaxed);
    bool cp_regs_nonzero =
      register_file_ &&
      (register_file_->values[0x01C0] != 0 || register_file_->values[0x01C1] != 0 ||
       register_file_->values[0x01C5] != 0);
    bool pre_ring_video_calls_seen =
        vd_init_engines_count > 0 || vd_asic_id_count > 0 ||
        vd_init_edram_count > 0 || vd_retrain_edram_count > 0 ||
        vd_retrain_worker_count > 0 || vd_hsio_count > 0;
    bool bootstrap_api_calls_seen =
      vd_init_count > 0 || vd_cb_count > 0 || vd_swap_count > 0 ||
      runtime_bootstrap_call_total > 0;
    bool any_mmio_ring_writes_seen =
        mmio_rb_base_writes > 0 || mmio_rb_cntl_writes > 0 ||
      mmio_rb_wptr_writes > 0 || mmio_cp_guest_writes > 0 || cp_regs_nonzero;
    bool host_debug_swap_observed = dbg.debug_host_swap_injection_count != 0;
    const char* bootstrap_source = "none";
    if (any_mmio_ring_writes_seen) {
      bootstrap_source = "mmio";
    } else if (bootstrap_api_calls_seen) {
      bootstrap_source = "vd_runtime";
    } else if (guest_main_dispatch_bridge_seen) {
      bootstrap_source = "guest_dispatch_bridge";
    } else if (dbg.ring_buffer_watch_hits > 0) {
      bootstrap_source = "watch_only";
    } else if (host_debug_swap_observed) {
      bootstrap_source = "host_inject_only";
    }
    const char* stage = "UNCLASSIFIED";
    const char* stage_reason = "unclassified";
    if (dbg.primary_buffer_ptr == 0 || !dbg.ring_buffer_initialized ||
        !dbg.initialization_acknowledged) {
      stage = pre_ring_video_calls_seen ? "PRE_RING_VIDEO_BOOTSTRAP_STALLED"
                                        : "BOOTSTRAP_INCOMPLETE";
      stage_reason = pre_ring_video_calls_seen
                         ? "pre-ring Vd video calls occurred, but ring/callback bootstrap never followed"
                         : "no Vd bootstrap calls and no CP_RB MMIO writes observed before ring setup";
    } else if (dbg.read_ptr_index == 0 && dbg.write_ptr_index == 0 &&
               dbg.swap_packet_count == 0 && dbg.ring_buffer_watch_hits == 0 &&
               mmio_rb_wptr_writes == 0) {
      stage = "RING_READY_NO_COMMAND_SUBMISSION";
      if (guest_main_dispatch_bridge_seen && !bootstrap_api_calls_seen &&
          !any_mmio_ring_writes_seen) {
        stage_reason =
            "host ring appears initialized and guest dispatch bridge executed, "
            "but no callback registration, GPU bootstrap calls, or ring "
            "command submission followed";
      } else {
        stage_reason =
            "ring appears initialized, but no command submission or write "
            "pointer updates were observed";
      }
    } else if (dbg.read_ptr_index == dbg.write_ptr_index &&
               dbg.swap_packet_count == 0) {
      stage = "RING_IDLE_NO_PACKETS";
      stage_reason =
          "read/write pointers are equal and no swap packets were consumed";
    } else if (dbg.swap_packet_count == 0) {
      stage = "RING_ACTIVITY_NO_SWAP";
      stage_reason =
          "ring activity exists but swap packets are still absent";
    } else if (dbg.debug_host_swap_ring_injection_count > 0 &&
               !bootstrap_api_calls_seen && !any_mmio_ring_writes_seen) {
      stage = "HOST_DEBUG_SWAP_ONLY";
      stage_reason =
          "swap packets are present, but only from host debug injection while "
          "guest Vd/MMIO bootstrap activity remains unobserved";
    } else {
      stage = "SWAPS_PRESENT";
      stage_reason = "swap packets are present";
    }
    uint64_t last_swap_age =
        (dbg.last_swap_packet_ms && now_ms >= dbg.last_swap_packet_ms)
            ? (now_ms - dbg.last_swap_packet_ms)
            : 0;
    uint64_t host_debug_swap_age =
        (dbg.debug_host_swap_last_ms && now_ms >= dbg.debug_host_swap_last_ms)
            ? (now_ms - dbg.debug_host_swap_last_ms)
            : 0;
    uint64_t ring_watch_last_arm_age =
        (dbg.ring_buffer_watch_last_arm_ms &&
         now_ms >= dbg.ring_buffer_watch_last_arm_ms)
            ? (now_ms - dbg.ring_buffer_watch_last_arm_ms)
            : 0;
    uint64_t dead_pipeline_recover_count =
      dead_pipeline_recovery_count_.load(std::memory_order_relaxed);
    uint64_t dead_pipeline_recover_last_ms =
      last_dead_pipeline_recovery_ms_.load(std::memory_order_relaxed);
    uint64_t dead_pipeline_recover_age =
      (dead_pipeline_recover_last_ms && now_ms >= dead_pipeline_recover_last_ms)
        ? (now_ms - dead_pipeline_recover_last_ms)
        : 0;
    uint64_t auto_callback_recover_count =
      auto_callback_recovery_count_.load(std::memory_order_relaxed);
    uint64_t auto_callback_recover_last_ms =
      last_auto_callback_recovery_ms_.load(std::memory_order_relaxed);
    uint64_t auto_callback_recover_age =
      (auto_callback_recover_last_ms &&
       now_ms >= auto_callback_recover_last_ms)
        ? (now_ms - auto_callback_recover_last_ms)
        : 0;
    XELOGW(
        "RING BUFFER: no-swap diagnosis stage={} ring_init={} init_ack={} "
        "rb_base={:08X} rb_size={:08X} read_ptr={:08X} write_ptr={:08X} "
        "swap_packets={} last_swap_age_ms={} ring_watch_hits={} "
        "ring_watch_arms={} ring_watch_last_arm_age_ms={} "
        "host_debug_swaps(total={},ring={},direct={},last_age_ms={}) "
        "wptr_updates(total={},guest={},last_source={},mmio={},cp_packet={},"
        "debug_swap={},other={}) "
        "vd_init_engines={} vd_asic_id={} vd_init={} vd_cb={} vd_swap={} "
        "vd_init_edram={} vd_retrain_edram={} vd_retrain_worker={} "
        "vd_hsio={} mmio_rb_base={} mmio_rb_cntl={} mmio_rb_wptr={} "
        "dead_pipeline_recover(count={},last_age_ms={}) "
        "auto_callback_recover(count={},last_age_ms={})",
        stage, dbg.ring_buffer_initialized ? "YES" : "NO",
        dbg.initialization_acknowledged ? "YES" : "NO", dbg.primary_buffer_ptr,
        dbg.primary_buffer_size, dbg.read_ptr_index, dbg.write_ptr_index,
        dbg.swap_packet_count, last_swap_age, dbg.ring_buffer_watch_hits,
        dbg.ring_buffer_watch_arm_count, ring_watch_last_arm_age,
        dbg.debug_host_swap_injection_count,
        dbg.debug_host_swap_ring_injection_count,
        dbg.debug_host_swap_direct_issue_count, host_debug_swap_age,
        wptr_update_total, wptr_guest_driven_count,
        WptrUpdateSourceName(wptr_last_source_id), wptr_source_mmio,
        wptr_source_cp_packet, wptr_source_debug_swap, wptr_source_other,
        vd_init_engines_count, vd_asic_id_count, vd_init_count, vd_cb_count,
        vd_swap_count, vd_init_edram_count, vd_retrain_edram_count,
        vd_retrain_worker_count, vd_hsio_count, mmio_rb_base_writes,
        mmio_rb_cntl_writes, mmio_rb_wptr_writes,
        dead_pipeline_recover_count, dead_pipeline_recover_age,
        auto_callback_recover_count, auto_callback_recover_age);
      XELOGW(
        "RING BUFFER: no-swap classifier detail stage={} reason='{}' "
        "bootstrap_api_calls_seen={} pre_ring_video_calls_seen={} "
        "any_mmio_ring_writes_seen={} guest_main_dispatch_bridge_seen={} "
        "guest_main_dispatch_bridge_total={} host_debug_swap_observed={} "
        "bootstrap_source={} "
        "wptr_guest_driven_observed={} wptr_last_source={} "
        "cp_worker_running={} cp_setup_complete={} "
        "cp_has_write_ptr_event={} cp_has_init_complete_event={} "
        "cp_init_request_count={} cp_init_ack_transitions={} "
        "cp_last_init_request_ms={} cp_last_init_signal_ms={} "
        "cp_last_init_ack_ms={} runtime_bootstrap_call_total={} "
        "mmio_cp_guest_writes={} cp_regs_nonzero={}",
        stage, stage_reason, bootstrap_api_calls_seen ? "YES" : "NO",
        pre_ring_video_calls_seen ? "YES" : "NO",
        any_mmio_ring_writes_seen ? "YES" : "NO",
        guest_main_dispatch_bridge_seen ? "YES" : "NO",
        proc_stats.guest_main_dispatch_bridge_calls_total,
        host_debug_swap_observed ? "YES" : "NO",
        bootstrap_source,
        wptr_guest_driven_count ? "YES" : "NO",
        WptrUpdateSourceName(wptr_last_source_id),
        dbg.worker_running ? "YES" : "NO",
        dbg.worker_setup_complete ? "YES" : "NO",
        dbg.has_write_ptr_event ? "YES" : "NO",
        dbg.has_init_complete_event ? "YES" : "NO",
        dbg.init_request_count, dbg.init_ack_transition_count,
        dbg.last_init_request_ms, dbg.last_init_signal_ms,
        dbg.last_init_ack_ms, runtime_bootstrap_call_total, mmio_cp_guest_writes,
        cp_regs_nonzero ? "YES" : "NO");

      if (interrupt_callback_ == 0 && vd_cb_count == 0) {
        XELOGW(
            "RING BUFFER: callback path unexecuted "
            "(interrupt_callback=0, VdSetGraphicsInterruptCallback count=0) "
            "- guest ring lifecycle may remain stalled");
      }
      if (dbg.read_ptr_index == dbg.write_ptr_index &&
          dbg.ring_buffer_watch_hits == 0) {
        XELOGW(
            "RING BUFFER: pointer stall observed "
            "(read_ptr={:08X} write_ptr={:08X} ring_watch_hits=0 "
            "ring_watch_arms={} guest_write_enabled={} force_rw_mode={} "
            "wptr_last_source={})",
            dbg.read_ptr_index, dbg.write_ptr_index,
            dbg.ring_buffer_watch_arm_count,
            dbg.ring_watch_last_guest_write_enabled ? "YES" : "NO",
            dbg.ring_watch_force_rw_mode ? "YES" : "NO",
            WptrUpdateSourceName(wptr_last_source_id));
      }

      if (kernel_state_ &&
        (std::string_view(stage) == "BOOTSTRAP_INCOMPLETE" ||
         std::string_view(stage) == "PRE_RING_VIDEO_BOOTSTRAP_STALLED")) {
        auto* probe_memory = kernel_state_->memory();
        uint32_t debugmon_slot = 0;
        bool debugmon_slot_committed = false;
        bool debugmon_slot_translated = false;
        ReadGuestWordSnapshot(probe_memory, 0x820005E0, &debugmon_slot,
                  &debugmon_slot_committed,
                  &debugmon_slot_translated);
        XELOGW(
          "RING BUFFER: no-swap probe KeDebugMonitorData slot "
          "addr=820005E0 committed={} translated={} value={:08X}",
          debugmon_slot_committed ? "YES" : "NO",
          debugmon_slot_translated ? "YES" : "NO", debugmon_slot);
        if (debugmon_slot_translated && debugmon_slot != 0) {
        uint32_t debugmon_struct = 0;
        bool debugmon_struct_committed = false;
        bool debugmon_struct_translated = false;
        ReadGuestWordSnapshot(probe_memory, debugmon_slot, &debugmon_struct,
                    &debugmon_struct_committed,
                    &debugmon_struct_translated);
        XELOGW(
          "RING BUFFER: no-swap probe KeDebugMonitorData ptr "
          "slot={:08X} committed={} translated={} value={:08X}",
          debugmon_slot, debugmon_struct_committed ? "YES" : "NO",
          debugmon_struct_translated ? "YES" : "NO", debugmon_struct);
        }

        uint32_t xex_slot = 0;
        bool xex_slot_committed = false;
        bool xex_slot_translated = false;
        ReadGuestWordSnapshot(probe_memory, 0x8200083C, &xex_slot,
                  &xex_slot_committed, &xex_slot_translated);
        XELOGW(
          "RING BUFFER: no-swap probe XexExecutableModuleHandle slot "
          "addr=8200083C committed={} translated={} value={:08X}",
          xex_slot_committed ? "YES" : "NO",
          xex_slot_translated ? "YES" : "NO", xex_slot);
        if (xex_slot_translated && xex_slot != 0) {
        uint32_t module_handle = 0;
        bool module_handle_committed = false;
        bool module_handle_translated = false;
        ReadGuestWordSnapshot(probe_memory, xex_slot, &module_handle,
                    &module_handle_committed,
                    &module_handle_translated);
        XELOGW(
          "RING BUFFER: no-swap probe XexExecutableModuleHandle ptr "
          "slot={:08X} committed={} translated={} value={:08X}",
          xex_slot, module_handle_committed ? "YES" : "NO",
          module_handle_translated ? "YES" : "NO", module_handle);
        }
      }
  }

  if (cvars::gpu_log_no_swap_dump_threads) {
    uint64_t last_dump =
        last_no_swap_thread_dump_ms_.load(std::memory_order_relaxed);
    uint64_t interval_ms =
        static_cast<uint64_t>(cvars::gpu_log_no_swap_threads_interval_ms);
    if (last_dump == 0 || interval_ms == 0 ||
        now_ms - last_dump >= interval_ms) {
      last_no_swap_thread_dump_ms_.store(now_ms, std::memory_order_relaxed);
      DumpGuestThreadStates("NO_SWAP");
    }
  }

  if (register_file_) {
    uint32_t rb_base_reg = register_file_->values[0x01C0];
    uint32_t rb_cntl_reg = register_file_->values[0x01C1];
    uint32_t rb_rptr_reg = register_file_->values[0x01C4];
    uint32_t rb_wptr_reg = register_file_->values[0x01C5];
    XELOGW(
        "DEBUG: NO SWAP: CP_RB_* regs base={:08X} cntl={:08X} rptr={:08X} "
        "wptr={:08X} since_first_vblank_ms={}",
        rb_base_reg, rb_cntl_reg, rb_rptr_reg, rb_wptr_reg, since_first_vblank);
  }
}

void GraphicsSystem::MarkVblank() {
  SCOPE_profile_cpu_f("gpu");

  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  Emulator* emulator = kernel_state_ ? kernel_state_->emulator() : nullptr;
  if (emulator && emulator->IsModuleLoadGateClosed()) {
    return;
  }
  if (cvars::gpu_vblank_suspend_until_module_load_success && kernel_state_) {
    auto load_stats = kernel_state_->GetUserModuleLoadStats();
    auto executable_module = kernel_state_->GetExecutableModule();
    bool executable_fully_ready = executable_module != nullptr &&
                                  executable_module->entry_point() != 0 &&
                                  !load_stats.load_inflight;
    if (!executable_fully_ready) {
      return;
    }
  }

  // Increment vblank counter (so the game sees us making progress).
  command_processor_->increment_counter();
  static std::atomic<uint64_t> vblank_count{0};
  uint64_t vblank_id = vblank_count.fetch_add(1) + 1;
  bool log_vblank = cvars::gpu_log_vblank &&
                    ShouldLogEvery(vblank_id, cvars::gpu_log_vblank_every_n);
  static std::atomic<uint32_t> last_logged_read{0};
  static std::atomic<uint32_t> last_logged_write{0};
  static std::atomic<uint64_t> last_logged_ms{0};
  uint32_t read_ptr =
      command_processor_ ? command_processor_->read_ptr_index() : 0;
  uint32_t write_ptr =
      command_processor_ ? command_processor_->write_ptr_index() : 0;
  static std::atomic<uint64_t> first_vblank_ms{0};
  if (first_vblank_ms.load(std::memory_order_relaxed) == 0) {
    first_vblank_ms.store(now_ms, std::memory_order_relaxed);
  }
  uint64_t since_first_vblank =
      now_ms - first_vblank_ms.load(std::memory_order_relaxed);
  ProbeCpBootstrapRegistersWhenMmioSilent(now_ms, since_first_vblank);
  kernel::KernelState::UserModuleLoadStats module_load_stats{};
  kernel::object_ref<kernel::UserModule> visible_executable_module;
  if (kernel_state_) {
    module_load_stats = kernel_state_->GetUserModuleLoadStats();
    visible_executable_module = kernel_state_->GetExecutableModule();
  }
  bool module_load_begin_seen = module_load_stats.last_begin_ms != 0;
  uint64_t since_last_module_load_begin_ms =
      module_load_begin_seen && now_ms >= module_load_stats.last_begin_ms
          ? now_ms - module_load_stats.last_begin_ms
          : 0;
  bool module_load_end_seen = module_load_stats.last_end_ms != 0;
  uint64_t since_last_module_load_end_ms =
      module_load_end_seen && now_ms >= module_load_stats.last_end_ms
          ? now_ms - module_load_stats.last_end_ms
          : 0;
  bool module_load_progress_seen = module_load_stats.last_progress_ms != 0;
  uint64_t since_last_module_load_progress_ms =
      module_load_progress_seen && now_ms >= module_load_stats.last_progress_ms
          ? now_ms - module_load_stats.last_progress_ms
          : 0;
  bool executable_module_visible =
      visible_executable_module ||
      module_load_stats.loaded_executable_user_modules != 0;
  bool guest_main_thread_ready = false;
  bool guest_main_thread_running = false;
  if (emulator) {
    guest_main_thread_ready = emulator->HasGuestMainThread();
    if (guest_main_thread_ready && !executable_module_visible) {
      static std::atomic<uint64_t> stale_guest_main_suppressed_last_log_ms{0};
      uint64_t interval_ms =
          static_cast<uint64_t>(cvars::gpu_prelaunch_suspend_log_interval_ms);
      uint64_t last_log = stale_guest_main_suppressed_last_log_ms.load(
          std::memory_order_relaxed);
      if (last_log == 0 || interval_ms == 0 || now_ms - last_log >= interval_ms) {
        stale_guest_main_suppressed_last_log_ms.store(
            now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: PRELAUNCH guest-main readiness suppressed: "
            "HasGuestMainThread=YES while no executable user module is "
            "visible yet (load_inflight={} load_success={} load_fail={} "
            "loaded_user_modules={} loaded_executable_user_modules={} "
            "load_begin_seen={} since_load_begin={}ms)",
            module_load_stats.load_inflight ? "YES" : "NO",
            module_load_stats.load_success, module_load_stats.load_fail,
            module_load_stats.loaded_user_modules,
            module_load_stats.loaded_executable_user_modules,
            module_load_begin_seen ? "YES" : "NO",
            since_last_module_load_begin_ms);
      }
      guest_main_thread_ready = false;
    }
    if (guest_main_thread_ready) {
      guest_main_thread_running = emulator->IsGuestMainThreadRunning();
      if (!guest_main_thread_running &&
          cvars::gpu_prelaunch_guest_main_resume_nudge) {
        uint64_t nudge_after_ms = static_cast<uint64_t>(
            cvars::gpu_prelaunch_guest_main_resume_nudge_after_ms);
        if (since_first_vblank >= nudge_after_ms) {
          uint64_t max_attempts = static_cast<uint64_t>(
              cvars::gpu_prelaunch_guest_main_resume_nudge_max_attempts);
          uint64_t current_attempts = guest_main_resume_nudge_count_.load(
              std::memory_order_relaxed);
          bool attempts_ok = max_attempts == 0 || current_attempts < max_attempts;
          if (attempts_ok) {
            uint64_t last_nudge_ms = last_guest_main_resume_nudge_ms_.load(
                std::memory_order_relaxed);
            uint64_t interval_ms = static_cast<uint64_t>(
                cvars::gpu_prelaunch_guest_main_resume_nudge_interval_ms);
            bool interval_due =
                (last_nudge_ms == 0 || interval_ms == 0 ||
                 now_ms >= last_nudge_ms + interval_ms);
            if (interval_due) {
              uint64_t nudge_id = guest_main_resume_nudge_count_.fetch_add(
                                     1, std::memory_order_relaxed) +
                                 1;
              last_guest_main_resume_nudge_ms_.store(
                  now_ms, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: PRELAUNCH guest-main resume nudge #{}: main thread is "
                  "present but not running (age={}ms load_inflight={} "
                  "loaded_exec_modules={} module_visible={})",
                  nudge_id, since_first_vblank,
                  module_load_stats.load_inflight ? "YES" : "NO",
                  module_load_stats.loaded_executable_user_modules,
                  executable_module_visible ? "YES" : "NO");
              bool resumed = emulator->EnsureGuestMainThreadRunning(
                  "gpu-prelaunch-guest-main-not-running");
              guest_main_thread_running = resumed;
            }
          }
        }
      }
    }
  }
  bool prelaunch_module_load_gate_active = false;
  if (cvars::gpu_prelaunch_suspend_during_module_load &&
      !guest_main_thread_ready && module_load_stats.load_inflight &&
      module_load_stats.loaded_executable_user_modules == 0) {
    uint64_t stall_after_ms = static_cast<uint64_t>(
        cvars::gpu_prelaunch_module_load_stall_after_ms);
    bool suspected_stall = false;
    if (stall_after_ms > 0 && module_load_begin_seen &&
        since_last_module_load_begin_ms >= stall_after_ms &&
        (!module_load_progress_seen ||
         since_last_module_load_progress_ms >= stall_after_ms)) {
      suspected_stall = true;
    }
    prelaunch_module_load_gate_active = !suspected_stall;

    static std::atomic<uint64_t> prelaunch_suspend_last_log_ms{0};
    uint64_t interval_ms = static_cast<uint64_t>(
        cvars::gpu_prelaunch_suspend_log_interval_ms);
    uint64_t last_log =
        prelaunch_suspend_last_log_ms.load(std::memory_order_relaxed);
    if (last_log == 0 || interval_ms == 0 ||
        now_ms - last_log >= interval_ms) {
      prelaunch_suspend_last_log_ms.store(now_ms, std::memory_order_relaxed);
      if (prelaunch_module_load_gate_active) {
        XELOGW(
            "DEBUG: PRELAUNCH module-load gate active: suspending non-essential "
            "GPU prelaunch diagnostics/watchdogs while LoadUserModule is "
            "inflight (age={}ms since_load_begin={}ms load_progress_seen={} "
            "since_load_progress={}ms load_progress_count={})",
            since_first_vblank, since_last_module_load_begin_ms,
            module_load_progress_seen ? "YES" : "NO",
            since_last_module_load_progress_ms,
            module_load_stats.load_progress_count);
      } else {
        XELOGE(
            "DEBUG: PRELAUNCH module-load gate released due to suspected stall "
            "(age={}ms since_load_begin={}ms load_progress_seen={} "
            "since_load_progress={}ms load_progress_count={})",
            since_first_vblank, since_last_module_load_begin_ms,
            module_load_progress_seen ? "YES" : "NO",
            since_last_module_load_progress_ms,
            module_load_stats.load_progress_count);
      }
    }
  }

  {
    static std::atomic<uint64_t> last_missing_cb_watchdog_log_ms{0};
    static std::atomic<uint64_t> last_missing_cb_watchdog_suppressed_log_ms{0};
    static std::atomic<uint64_t>
        last_missing_cb_watchdog_grace_log_ms{0};
    uint64_t swap_packets =
        command_processor_ ? command_processor_->swap_packet_count() : 0;
    uint32_t dump_after_vblank =
        cvars::gpu_debug_interrupt_missing_dump_after_vblank;
    bool watchdog_triggered =
        (dump_after_vblank != 0 && vblank_id >= dump_after_vblank) ||
        (swap_packets > 0);
    std::string callback_watch_stall_state;
    uint32_t callback_watch_stall_repeat_samples = 0;
    uint64_t callback_watch_stall_sample_ms = 0;
    uint64_t callback_watch_stall_age_ms = 0;
    bool callback_watch_startup_prolog_helper_grace = false;
    if (watchdog_triggered && interrupt_callback_ == 0 && kernel_state_) {
      if (auto* emulator = kernel_state_->emulator()) {
        if (emulator->GetLastGpuDispatchStallSnapshot(
                &callback_watch_stall_state, nullptr, nullptr, nullptr,
                &callback_watch_stall_repeat_samples,
                &callback_watch_stall_sample_ms) &&
            callback_watch_stall_sample_ms != 0 &&
            now_ms >= callback_watch_stall_sample_ms) {
          callback_watch_stall_age_ms =
              now_ms - callback_watch_stall_sample_ms;
        }
        callback_watch_startup_prolog_helper_grace =
            IsStartupPrologHelperGraceWindow(
                callback_watch_stall_state, callback_watch_stall_age_ms,
                callback_watch_stall_repeat_samples);
      }
    }
    if (watchdog_triggered && interrupt_callback_ == 0 &&
        prelaunch_module_load_gate_active) {
      uint64_t interval_ms = static_cast<uint64_t>(
          cvars::gpu_prelaunch_suspend_log_interval_ms);
      uint64_t last_log =
          last_missing_cb_watchdog_suppressed_log_ms.load(
              std::memory_order_relaxed);
      if (last_log == 0 || interval_ms == 0 ||
          now_ms - last_log >= interval_ms) {
        last_missing_cb_watchdog_suppressed_log_ms.store(
            now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: CALLBACK WATCHDOG deferred by prelaunch module-load gate "
            "(vblank_id={} age={}ms since_load_begin={}ms "
            "load_progress_seen={} since_load_progress={}ms)",
            vblank_id, since_first_vblank, since_last_module_load_begin_ms,
            module_load_progress_seen ? "YES" : "NO",
            since_last_module_load_progress_ms);
      }
    } else if (watchdog_triggered && interrupt_callback_ == 0 &&
               callback_watch_startup_prolog_helper_grace) {
      uint64_t last_log =
          last_missing_cb_watchdog_grace_log_ms.load(
              std::memory_order_relaxed);
      if (last_log == 0 || now_ms - last_log >= 1000) {
        last_missing_cb_watchdog_grace_log_ms.store(
            now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: CALLBACK WATCHDOG deferred during startup prolog helper grace "
            "(vblank_id={} age={}ms stall_age_ms={} repeat_samples={} "
            "stall_state={})",
            vblank_id, since_first_vblank, callback_watch_stall_age_ms,
            callback_watch_stall_repeat_samples,
            callback_watch_stall_state.empty() ? "<none>"
                                               : callback_watch_stall_state);
      }
    } else if (watchdog_triggered && interrupt_callback_ == 0) {
      uint64_t last_log =
          last_missing_cb_watchdog_log_ms.load(std::memory_order_relaxed);
      if (last_log == 0 || now_ms - last_log >= 1000) {
        last_missing_cb_watchdog_log_ms.store(now_ms,
                                              std::memory_order_relaxed);
        uint64_t vd_cb_count =
            vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
        uint32_t vd_cb_pc =
            vd_set_interrupt_callback_state_.pc.load(std::memory_order_relaxed);
        uint32_t vd_cb_lr =
            vd_set_interrupt_callback_state_.lr.load(std::memory_order_relaxed);
        uint32_t vd_cb_thread = vd_set_interrupt_callback_state_.thread_id.load(
            std::memory_order_relaxed);
        uint32_t import_value_addr = 0;
        uint32_t import_thunk_addr = 0;
        uint32_t import_value_word = 0;
        bool import_value_word_ok = false;
        auto executable_module =
            kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
        if (executable_module && executable_module->xex_module()) {
          const auto* import_libs =
              executable_module->xex_module()->import_libraries();
          if (import_libs) {
            for (const auto& lib : *import_libs) {
              if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
                continue;
              }
              for (const auto& import_fn : lib.imports) {
                if (import_fn.ordinal != 0x01D5) {
                  continue;
                }
                import_value_addr = import_fn.value_address;
                import_thunk_addr = import_fn.thunk_address;
                break;
              }
              if (import_value_addr || import_thunk_addr) {
                break;
              }
            }
          }
        }
        if (memory_ && import_value_addr) {
          auto safe = memory_->TranslateVirtualSafe<uint32_t*>(import_value_addr);
          if (safe.success && safe.pointer) {
            import_value_word = ::xe::load_and_swap<uint32_t>(safe.pointer);
            import_value_word_ok = true;
          }
        }
        std::string callback_watch_stall_state;
        uint32_t callback_watch_stall_repeat_samples = 0;
        uint64_t callback_watch_stall_sample_ms = 0;
        uint64_t callback_watch_stall_age_ms = 0;
        if (kernel_state_) {
          auto* emulator = kernel_state_->emulator();
          if (emulator &&
              emulator->GetLastGpuDispatchStallSnapshot(
                  &callback_watch_stall_state, nullptr, nullptr, nullptr,
                  &callback_watch_stall_repeat_samples,
                  &callback_watch_stall_sample_ms) &&
              callback_watch_stall_sample_ms != 0 &&
              now_ms >= callback_watch_stall_sample_ms) {
            callback_watch_stall_age_ms =
                now_ms - callback_watch_stall_sample_ms;
          }
        }
        XELOGE(
            "DEBUG: CALLBACK WATCHDOG: interrupt_callback_set=NO after "
            "vblank_id={} since_first_vblank={}ms swap_packets={} "
            "vd_set_cb_count={} vd_set_cb_pc={:08X} vd_set_cb_lr={:08X} "
            "vd_set_cb_thread={} import_0x01D5_value_addr={:08X} "
            "import_0x01D5_thunk_addr={:08X} import_0x01D5_value_word={}"
            "{:08X} stall_state={} stall_age_ms={} "
            "stall_repeat_samples={}",
            vblank_id, since_first_vblank, swap_packets, vd_cb_count,
            vd_cb_pc, vd_cb_lr, vd_cb_thread, import_value_addr,
            import_thunk_addr, import_value_word_ok ? "0x" : "<unread>",
            import_value_word_ok ? import_value_word : 0u,
            callback_watch_stall_state.empty()
                ? "<none>"
                : callback_watch_stall_state,
            callback_watch_stall_age_ms,
            callback_watch_stall_repeat_samples);
      }
    }
  }

  if (!guest_main_thread_ready) {
    auto prelaunch_executable_module =
        kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
    if (prelaunch_executable_module) {
      if (prelaunch_executable_module_missing_seen_.load(
              std::memory_order_relaxed) &&
          !prelaunch_executable_module_transition_logged_.exchange(
              true, std::memory_order_relaxed)) {
        XELOGI(
            "DEBUG: PRELAUNCH executable-module transition: module='{}' "
            "entry={:08X} age={}ms load_inflight={} load_success={} "
            "load_fail={} loaded_user_modules={} "
            "loaded_executable_user_modules={} load_progress_count={} "
            "load_progress_seen={} since_load_progress={}ms",
            prelaunch_executable_module->name(),
            prelaunch_executable_module->entry_point(), since_first_vblank,
            module_load_stats.load_inflight ? "YES" : "NO",
            module_load_stats.load_success, module_load_stats.load_fail,
            module_load_stats.loaded_user_modules,
            module_load_stats.loaded_executable_user_modules,
            module_load_stats.load_progress_count,
            module_load_progress_seen ? "YES" : "NO",
            since_last_module_load_progress_ms);
      }
    } else {
      prelaunch_executable_module_missing_seen_.store(true,
                                                      std::memory_order_relaxed);
    }
  }

  if (!prelaunch_module_load_gate_active && !guest_main_thread_ready &&
      kernel_state_ &&
      cvars::gpu_prelaunch_module_nudge &&
      !prelaunch_module_nudge_done_.load(std::memory_order_relaxed)) {
    uint64_t nudge_after_ms =
        static_cast<uint64_t>(cvars::gpu_prelaunch_module_nudge_after_ms);
    if (since_first_vblank >= nudge_after_ms) {
      auto executable_module = kernel_state_->GetExecutableModule();
      if (executable_module) {
        prelaunch_module_nudge_done_.store(true, std::memory_order_relaxed);
      } else {
        decltype(kernel_state_->FindFirstExecutableUserModule()) fallback_module =
            nullptr;
        if (!module_load_stats.load_inflight) {
          fallback_module = kernel_state_->FindFirstExecutableUserModule();
        }
        if (fallback_module &&
            !prelaunch_module_nudge_done_.exchange(true,
                                                   std::memory_order_relaxed)) {
          kernel_state_->SetExecutableModule(fallback_module);
          uint64_t nudge_id =
              prelaunch_module_nudge_count_.fetch_add(
                  1, std::memory_order_relaxed) +
              1;
          last_prelaunch_module_nudge_ms_.store(now_ms,
                                                std::memory_order_relaxed);
          XELOGW(
              "DEBUG: PRELAUNCH module nudge #{}: wired executable module='{}' "
              "entry={:08X} age={}ms",
              nudge_id, fallback_module->name(), fallback_module->entry_point(),
              since_first_vblank);
        } else {
          static std::atomic<uint64_t> no_module_nudge_log_count{0};
          uint64_t log_id = no_module_nudge_log_count.fetch_add(1) + 1;
          if (log_id <= 3 || (log_id % 120) == 0) {
            XELOGW(
                "DEBUG: PRELAUNCH module nudge deferred: no loaded executable "
                "user module available yet (age={}ms load_inflight={} "
                "load_success={} load_fail={} loaded_user_modules={} "
                "loaded_executable_user_modules={} load_begin_seen={} "
                "since_load_begin={}ms load_end_seen={} since_load_end={}ms "
                "load_progress_count={} load_progress_seen={} "
                "since_load_progress={}ms)",
                since_first_vblank,
                module_load_stats.load_inflight ? "YES" : "NO",
                module_load_stats.load_success, module_load_stats.load_fail,
                module_load_stats.loaded_user_modules,
                module_load_stats.loaded_executable_user_modules,
                module_load_begin_seen ? "YES" : "NO",
                since_last_module_load_begin_ms,
                module_load_end_seen ? "YES" : "NO",
                since_last_module_load_end_ms,
                module_load_stats.load_progress_count,
                module_load_progress_seen ? "YES" : "NO",
                since_last_module_load_progress_ms);
          }
          if (cvars::gpu_prelaunch_force_module_load_by_path) {
            ForceFallbackDelaySnapshot force_module_load_delay_snapshot;
            bool defer_force_module_load_nudge =
                ShouldDeferForceFallback(kernel_state_,
                                         &force_module_load_delay_snapshot);
            if (defer_force_module_load_nudge) {
              static std::atomic<uint64_t>
                  prelaunch_force_module_delay_last_log_ms{0};
              LogForceFallbackDeferred(
                  "PRELAUNCH module-force nudge", since_first_vblank, now_ms,
                  force_module_load_delay_snapshot,
                  &prelaunch_force_module_delay_last_log_ms);
            } else {
            uint64_t force_after_ms = static_cast<uint64_t>(
                cvars::gpu_prelaunch_force_module_load_after_ms);
            uint64_t force_retry_interval_ms = static_cast<uint64_t>(
                cvars::gpu_prelaunch_force_module_load_retry_interval_ms);
            uint64_t last_force_ms =
                last_prelaunch_force_module_load_nudge_ms_.load(
                    std::memory_order_relaxed);
            bool force_due = since_first_vblank >= force_after_ms;
            bool retry_due =
                (last_force_ms == 0 || force_retry_interval_ms == 0 ||
                 now_ms >= last_force_ms + force_retry_interval_ms);
            bool module_visibility_still_empty =
                module_load_stats.loaded_executable_user_modules == 0;
            std::string forced_path = cvars::gpu_prelaunch_force_module_load_path;
            if (forced_path.empty()) {
              forced_path = "game:\\default.xex";
            }
            bool force_load_call_seen = module_load_stats.load_calls != 0;
            bool force_load_call_gate_open =
                !cvars::gpu_prelaunch_force_module_load_require_load_call ||
                force_load_call_seen;
            bool force_base_mounted = true;
            std::string forced_base_path =
                xe::utf8::find_base_guest_path(forced_path);
            if (force_due && retry_due && module_visibility_still_empty &&
                !module_load_stats.load_inflight &&
                !forced_base_path.empty() &&
                forced_base_path.find(':') != std::string::npos &&
                kernel_state_ && kernel_state_->file_system()) {
              std::string forced_base_target;
              force_base_mounted = kernel_state_->file_system()->FindSymbolicLink(
                  forced_base_path, forced_base_target);
            }
            if (force_due && retry_due && module_visibility_still_empty &&
                !module_load_stats.load_inflight &&
                !force_load_call_gate_open) {
              static std::atomic<uint64_t> missing_load_call_log_count{0};
              uint64_t missing_load_call_log_id =
                  missing_load_call_log_count.fetch_add(1) + 1;
              if (missing_load_call_log_id <= 3 ||
                  (missing_load_call_log_id % 120) == 0) {
                XELOGW(
                    "DEBUG: PRELAUNCH module-force nudge deferred: waiting for "
                    "first LoadUserModule call "
                    "(age={}ms load_calls={} load_success={} load_fail={})",
                    since_first_vblank, module_load_stats.load_calls,
                    module_load_stats.load_success, module_load_stats.load_fail);
              }
            } else if (force_due && retry_due && module_visibility_still_empty &&
                       !module_load_stats.load_inflight &&
                       !force_base_mounted) {
              static std::atomic<uint64_t> force_base_not_mounted_log_count{0};
              uint64_t force_base_not_mounted_log_id =
                  force_base_not_mounted_log_count.fetch_add(1) + 1;
              if (force_base_not_mounted_log_id <= 3 ||
                  (force_base_not_mounted_log_id % 120) == 0) {
                XELOGW(
                    "DEBUG: PRELAUNCH module-force nudge deferred: base path '{}' "
                    "not mounted for '{}'",
                    forced_base_path, forced_path);
              }
            } else if (force_due && retry_due && module_visibility_still_empty &&
                !module_load_stats.load_inflight &&
                !prelaunch_force_module_load_nudge_inflight_.exchange(
                    true, std::memory_order_relaxed)) {
              uint64_t force_nudge_id =
                  prelaunch_force_module_load_nudge_count_.fetch_add(
                      1, std::memory_order_relaxed) +
                  1;
              last_prelaunch_force_module_load_nudge_ms_.store(
                  now_ms, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: PRELAUNCH module-force nudge #{}: queueing "
                  "KernelState::LoadUserModule('{}') "
                  "(age={}ms load_success={} load_fail={} "
                  "load_progress_count={})",
                  force_nudge_id, forced_path, since_first_vblank,
                  module_load_stats.load_success, module_load_stats.load_fail,
                  module_load_stats.load_progress_count);
              bool queued =
                  app_context_ &&
                  app_context_->CallInUIThreadDeferredTagged(
                      "GPUPrelaunchForceLoadUserModule",
                      [this, forced_path, force_nudge_id, since_first_vblank]() {
                        auto clear_inflight = [this]() {
                          prelaunch_force_module_load_nudge_inflight_.store(
                              false, std::memory_order_relaxed);
                        };
                        if (!kernel_state_) {
                          XELOGW(
                              "DEBUG: PRELAUNCH module-force nudge #{} aborted: "
                              "kernel_state unavailable",
                              force_nudge_id);
                          clear_inflight();
                          return;
                        }
                        auto existing_exec_module =
                            kernel_state_->GetExecutableModule();
                        if (existing_exec_module) {
                          XELOGI(
                              "DEBUG: PRELAUNCH module-force nudge #{} skipped: "
                              "executable already wired ('{}')",
                              force_nudge_id, existing_exec_module->name());
                          clear_inflight();
                          return;
                        }
                        auto live_load_stats =
                            kernel_state_->GetUserModuleLoadStats();
                        if (live_load_stats.load_inflight) {
                          XELOGW(
                              "DEBUG: PRELAUNCH module-force nudge #{} deferred "
                              "in UI callback: LoadUserModule already inflight "
                              "(load_calls={} load_success={} load_fail={})",
                              force_nudge_id, live_load_stats.load_calls,
                              live_load_stats.load_success,
                              live_load_stats.load_fail);
                          clear_inflight();
                          return;
                        }
                        auto* file_system = kernel_state_->file_system();
                        if (!file_system) {
                          XELOGW(
                              "DEBUG: PRELAUNCH module-force nudge #{} deferred "
                              "in UI callback: file_system unavailable",
                              force_nudge_id);
                          clear_inflight();
                          return;
                        }
                        std::string forced_base_path =
                            xe::utf8::find_base_guest_path(forced_path);
                        if (!forced_base_path.empty() &&
                            forced_base_path.find(':') != std::string::npos) {
                          std::string forced_base_target;
                          if (!file_system->FindSymbolicLink(forced_base_path,
                                                             forced_base_target)) {
                            static std::atomic<uint64_t>
                                missing_base_log_count{0};
                            uint64_t missing_base_log_id =
                                missing_base_log_count.fetch_add(1) + 1;
                            if (missing_base_log_id <= 3 ||
                                (missing_base_log_id % 120) == 0) {
                              XELOGW(
                                  "DEBUG: PRELAUNCH module-force nudge #{} deferred "
                                  "in UI callback: base path '{}' not mounted "
                                  "for '{}'",
                                  force_nudge_id, forced_base_path, forced_path);
                            }
                            clear_inflight();
                            return;
                          }
                        }
                        auto forced_entry = file_system->ResolvePath(forced_path);
                        if (!forced_entry) {
                          static std::atomic<uint64_t> unresolved_path_log_count{
                              0};
                          uint64_t unresolved_path_log_id =
                              unresolved_path_log_count.fetch_add(1) + 1;
                          if (unresolved_path_log_id <= 3 ||
                              (unresolved_path_log_id % 120) == 0) {
                            XELOGW(
                                "DEBUG: PRELAUNCH module-force nudge #{} deferred "
                                "in UI callback: path not resolvable yet ('{}')",
                                force_nudge_id, forced_path);
                          }
                          clear_inflight();
                          return;
                        }
                        std::string forced_resolved_path =
                            forced_entry->absolute_path();
                        if (forced_resolved_path.empty()) {
                          forced_resolved_path = forced_path;
                        }

                        uint64_t force_begin_ms = Clock::QueryHostUptimeMillis();
                        auto forced_module =
                            kernel_state_->LoadUserModule(forced_resolved_path);
                        uint64_t force_elapsed_ms =
                            Clock::QueryHostUptimeMillis() - force_begin_ms;
                        if (!forced_module) {
                          XELOGE(
                              "DEBUG: PRELAUNCH module-force nudge #{} failed: "
                              "LoadUserModule('{}') returned null "
                              "(requested_path='{}' resolved_path='{}' "
                              "elapsed={}ms)",
                              force_nudge_id, forced_resolved_path, forced_path,
                              forced_resolved_path, force_elapsed_ms);
                          clear_inflight();
                          return;
                        }
                        if (!forced_module->is_executable()) {
                          XELOGE(
                              "DEBUG: PRELAUNCH module-force nudge #{} loaded "
                              "non-executable module='{}' path='{}' "
                              "(elapsed={}ms)",
                              force_nudge_id, forced_module->name(),
                              forced_module->path(), force_elapsed_ms);
                          clear_inflight();
                          return;
                        }

                        if (!kernel_state_->GetExecutableModule()) {
                          kernel_state_->SetExecutableModule(forced_module);
                        }
                        XELOGW(
                            "DEBUG: PRELAUNCH module-force nudge #{} succeeded: "
                            "module='{}' entry={:08X} age_at_request={}ms "
                            "elapsed={}ms",
                            force_nudge_id, forced_module->name(),
                            forced_module->entry_point(), since_first_vblank,
                            force_elapsed_ms);
                        clear_inflight();
                      });
              if (!queued) {
                prelaunch_force_module_load_nudge_inflight_.store(
                    false, std::memory_order_relaxed);
                XELOGE(
                    "DEBUG: PRELAUNCH module-force nudge #{} failed to queue "
                    "UI-thread callback",
                    force_nudge_id);
              }
            } else if (force_due && module_load_stats.load_inflight) {
              static std::atomic<uint64_t> force_load_inflight_log_count{0};
              uint64_t inflight_log_id =
                  force_load_inflight_log_count.fetch_add(1) + 1;
              uint64_t stall_after_ms = static_cast<uint64_t>(
                  cvars::gpu_prelaunch_module_load_stall_after_ms);
              bool suspected_stall =
                  stall_after_ms > 0 &&
                  since_last_module_load_begin_ms >= stall_after_ms &&
                  (!module_load_progress_seen ||
                   since_last_module_load_progress_ms >= stall_after_ms);
              if (inflight_log_id <= 3 || (inflight_log_id % 120) == 0) {
                if (suspected_stall) {
                  XELOGE(
                      "DEBUG: PRELAUNCH module-force nudge stall-warning: "
                      "LoadUserModule inflight appears stalled "
                      "(age={}ms since_load_begin={}ms load_progress_seen={} "
                      "since_load_progress={}ms load_progress_count={})",
                      since_first_vblank, since_last_module_load_begin_ms,
                      module_load_progress_seen ? "YES" : "NO",
                      since_last_module_load_progress_ms,
                      module_load_stats.load_progress_count);
                } else {
                  XELOGW(
                      "DEBUG: PRELAUNCH module-force nudge deferred: "
                      "LoadUserModule still inflight "
                      "(age={}ms since_load_begin={}ms "
                      "load_progress_seen={} since_load_progress={}ms "
                      "load_progress_count={})",
                      since_first_vblank, since_last_module_load_begin_ms,
                      module_load_progress_seen ? "YES" : "NO",
                      since_last_module_load_progress_ms,
                      module_load_stats.load_progress_count);
                }
              }
            }
            }
          }
        }
      }
    }
  }

    if (cvars::gpu_startup_watch && !prelaunch_module_load_gate_active) {
    uint64_t first_watch_ms =
      startup_watch_first_vblank_ms_.load(std::memory_order_relaxed);
    if (first_watch_ms == 0) {
      startup_watch_first_vblank_ms_.store(now_ms, std::memory_order_relaxed);
      first_watch_ms = now_ms;
    }
    uint64_t startup_age_ms =
      (now_ms >= first_watch_ms) ? (now_ms - first_watch_ms) : 0;
    bool callback_set = interrupt_callback_ != 0;
    bool ring_initialized =
      command_processor_ ? command_processor_->IsRingBufferInitialized() : false;
    auto startup_dbg =
      command_processor_ ? command_processor_->GetRingBufferDebugState()
                 : CommandProcessor::RingBufferDebugState{};
    uint64_t vd_init_count =
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_cb_count =
      vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_swap_count = vd_swap_state_.count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_base_writes =
      mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_cntl_writes =
      mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_wptr_writes =
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
    auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
    XexCheckExecutablePrivilegeRuntimeState startup_xexcheck_runtime_state =
        ClassifyXexCheckExecutablePrivilegeRuntimeState(proc_stats);
    bool bootstrap_activity_seen =
      vd_init_count || vd_cb_count || vd_swap_count || mmio_rb_base_writes ||
      mmio_rb_cntl_writes || mmio_rb_wptr_writes ||
      proc_stats.guest_main_dispatch_bridge_calls_total != 0 ||
      proc_stats.proc_lookup_calls != 0 ||
      proc_stats.proc_lookup_resolved != 0 ||
      proc_stats.proc_lookup_missing != 0;
    std::string startup_dispatch_stall_state;
    std::string startup_dispatch_stall_targets;
    uint32_t startup_dispatch_stall_repeat_samples = 0;
    uint64_t startup_dispatch_stall_sample_ms = 0;
    uint64_t startup_dispatch_stall_age_ms = 0;
    bool startup_dispatch_stall_has_xex_privilege_scan = false;
    bool startup_dispatch_stall_xex_privilege_branch_only = false;
    if (kernel_state_) {
      auto* emulator = kernel_state_->emulator();
      if (emulator &&
          emulator->GetLastGpuDispatchStallSnapshot(
              &startup_dispatch_stall_state, nullptr, nullptr,
              &startup_dispatch_stall_targets,
              &startup_dispatch_stall_repeat_samples,
              &startup_dispatch_stall_sample_ms) &&
          startup_dispatch_stall_sample_ms != 0 &&
          now_ms >= startup_dispatch_stall_sample_ms) {
        startup_dispatch_stall_age_ms =
            now_ms - startup_dispatch_stall_sample_ms;
      }
    }
    startup_dispatch_stall_has_xex_privilege_scan =
        startup_dispatch_stall_targets.find("xex_privilege_check=YES") !=
        std::string::npos;
    startup_dispatch_stall_xex_privilege_branch_only =
        startup_dispatch_stall_has_xex_privilege_scan &&
        proc_stats.xex_check_executable_privilege_calls == 0;
    bool startup_ready = callback_set && ring_initialized &&
               startup_dbg.initialization_acknowledged;
    uint64_t last_watch_log_ms =
      startup_watch_last_log_ms_.load(std::memory_order_relaxed);
    uint64_t watch_interval =
      static_cast<uint64_t>(cvars::gpu_startup_watch_interval_ms);
    if (!startup_ready &&
      (last_watch_log_ms == 0 || watch_interval == 0 ||
       now_ms - last_watch_log_ms >= watch_interval)) {
      startup_watch_last_log_ms_.store(now_ms, std::memory_order_relaxed);
      auto startup_executable_module =
          kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
      XELOGW(
        "DEBUG: gpu_startup_watch: age={}ms guest_main_ready={} "
        "guest_main_running={} module='{}' ring_init={} init_ack={} "
        "callback_set={} read_ptr={:08X} write_ptr={:08X} "
        "bootstrap_activity_seen={} guest_main_dispatch_bridge_total={} "
        "vd_init={} vd_set_cb={} vd_swap={} "
        "mmio_rb_base={} mmio_rb_cntl={} mmio_rb_wptr={} "
        "load_inflight={} load_success={} load_fail={} "
        "loaded_user_modules={} loaded_executable_user_modules={} "
        "load_begin_seen={} since_load_begin={}ms load_end_seen={} "
        "since_load_end={}ms load_progress_count={} "
        "load_progress_seen={} since_load_progress={}ms "
        "proc_lookup_calls={} proc_lookup_resolved={} "
        "proc_lookup_missing={} xex_priv_scan_seen={} "
        "xex_check_privilege_calls={} "
        "xexcheck_static_thunk_calls={} xexcheck_export_entry_calls={} "
        "xexcheck_runtime_state={} xex_priv_branch_only={} "
        "wait_watch_obj={:08X} "
        "stall_state={} stall_age_ms={} stall_repeat_samples={}",
        startup_age_ms, guest_main_thread_ready ? "YES" : "NO",
        guest_main_thread_running ? "YES" : "NO",
        startup_executable_module ? startup_executable_module->name().c_str()
                                  : "<none>",
        ring_initialized ? "YES" : "NO",
        startup_dbg.initialization_acknowledged ? "YES" : "NO",
        callback_set ? "YES" : "NO", startup_dbg.read_ptr_index,
        startup_dbg.write_ptr_index,
        bootstrap_activity_seen ? "YES" : "NO",
        proc_stats.guest_main_dispatch_bridge_calls_total,
        vd_init_count, vd_cb_count,
        vd_swap_count, mmio_rb_base_writes, mmio_rb_cntl_writes,
        mmio_rb_wptr_writes, module_load_stats.load_inflight ? "YES" : "NO",
        module_load_stats.load_success, module_load_stats.load_fail,
        module_load_stats.loaded_user_modules,
        module_load_stats.loaded_executable_user_modules,
        module_load_begin_seen ? "YES" : "NO",
        since_last_module_load_begin_ms,
        module_load_end_seen ? "YES" : "NO",
        since_last_module_load_end_ms,
        module_load_stats.load_progress_count,
        module_load_progress_seen ? "YES" : "NO",
        since_last_module_load_progress_ms,
        proc_stats.proc_lookup_calls,
        proc_stats.proc_lookup_resolved, proc_stats.proc_lookup_missing,
        startup_dispatch_stall_has_xex_privilege_scan ? "YES" : "NO",
        proc_stats.xex_check_executable_privilege_calls,
        proc_stats.xex_check_executable_privilege_static_thunk_calls,
        proc_stats.xex_check_executable_privilege_export_entry_calls,
        XexCheckExecutablePrivilegeRuntimeStateName(
            startup_xexcheck_runtime_state),
        startup_dispatch_stall_xex_privilege_branch_only ? "YES" : "NO",
        kernel::xboxkrnl::GetWaitWatchObjectPtr(),
        startup_dispatch_stall_state.empty()
            ? "<none>"
            : startup_dispatch_stall_state,
        startup_dispatch_stall_age_ms,
        startup_dispatch_stall_repeat_samples);
    }
    if (startup_ready &&
      !startup_watch_ready_logged_.exchange(true, std::memory_order_relaxed)) {
      XELOGI(
        "DEBUG: gpu_startup_watch ready: age={}ms ring_init={} init_ack={} "
        "callback_set={} guest_main_ready={} read_ptr={:08X} "
        "write_ptr={:08X}",
        startup_age_ms, ring_initialized ? "YES" : "NO",
        startup_dbg.initialization_acknowledged ? "YES" : "NO",
        callback_set ? "YES" : "NO", guest_main_thread_ready ? "YES" : "NO",
        startup_dbg.read_ptr_index, startup_dbg.write_ptr_index);
    }
    uint64_t starvation_timeout =
      static_cast<uint64_t>(cvars::gpu_startup_starvation_timeout_ms);
    if (!startup_ready && starvation_timeout > 0 &&
      startup_age_ms >= starvation_timeout &&
      !startup_watch_timeout_logged_.exchange(true,
                          std::memory_order_relaxed)) {
      XELOGE(
        "DEBUG: gpu_startup_starvation: age={}ms guest_main_ready={} "
        "guest_main_running={} ring_init={} init_ack={} callback_set={} "
        "read_ptr={:08X} write_ptr={:08X} xex_priv_scan_seen={} "
        "xex_check_privilege_calls={} xexcheck_static_thunk_calls={} "
        "xexcheck_export_entry_calls={} xexcheck_runtime_state={} "
        "xex_priv_branch_only={} stall_state={} stall_age_ms={} "
        "stall_repeat_samples={}",
        startup_age_ms, guest_main_thread_ready ? "YES" : "NO",
        guest_main_thread_running ? "YES" : "NO",
        ring_initialized ? "YES" : "NO",
        startup_dbg.initialization_acknowledged ? "YES" : "NO",
        callback_set ? "YES" : "NO", startup_dbg.read_ptr_index,
        startup_dbg.write_ptr_index,
        startup_dispatch_stall_has_xex_privilege_scan ? "YES" : "NO",
        proc_stats.xex_check_executable_privilege_calls,
        proc_stats.xex_check_executable_privilege_static_thunk_calls,
        proc_stats.xex_check_executable_privilege_export_entry_calls,
        XexCheckExecutablePrivilegeRuntimeStateName(
            startup_xexcheck_runtime_state),
        startup_dispatch_stall_xex_privilege_branch_only ? "YES" : "NO",
        startup_dispatch_stall_state.empty()
            ? "<none>"
            : startup_dispatch_stall_state,
        startup_dispatch_stall_age_ms,
        startup_dispatch_stall_repeat_samples);
      if (startup_dispatch_stall_xex_privilege_branch_only) {
        XELOGW(
            "RING BUFFER: startup starvation privilege-gate detail "
            "branch_scan_sees_xexcheck=YES static_thunk_calls={} "
            "export_entry_calls={} runtime_state={} targets='{}'",
            proc_stats.xex_check_executable_privilege_static_thunk_calls,
            proc_stats.xex_check_executable_privilege_export_entry_calls,
            XexCheckExecutablePrivilegeRuntimeStateName(
                startup_xexcheck_runtime_state),
            startup_dispatch_stall_targets.empty()
                ? "<none>"
                : startup_dispatch_stall_targets);
      }
      DumpNoSwapDebugState(since_first_vblank, read_ptr, write_ptr);
      DumpGuestThreadStates("GPU_STARTUP_STARVATION");
      ForceFallbackDelaySnapshot startup_starvation_delay_snapshot;
      if (ShouldDeferForceFallback(kernel_state_, &startup_starvation_delay_snapshot)) {
        static std::atomic<uint64_t> startup_starvation_defer_last_log_ms{0};
        LogForceFallbackDeferred(
            "STARTUP starvation callback recovery", since_first_vblank, now_ms,
            startup_starvation_delay_snapshot,
            &startup_starvation_defer_last_log_ms);
      }
      TryAutoRecoverMissingInterruptCallback(now_ms, since_first_vblank,
                                             "startup-starvation");
    }
    }
  if (!guest_main_thread_ready && !prelaunch_module_load_gate_active) {
    static std::atomic<uint64_t> prelaunch_diag_log_count{0};
    static std::atomic<uint64_t> last_prelaunch_snapshot_ms{0};
    uint64_t prelaunch_id = prelaunch_diag_log_count.fetch_add(1) + 1;
    if (prelaunch_id <= 3 || (prelaunch_id % 240) == 0) {
      XELOGI(
          "GUEST MAIN THREAD: Not launched yet; deferring GPU guest-dependent "
          "diagnostics (vblank_id={}, since_first_vblank={}ms)",
          vblank_id, since_first_vblank);
      XELOGI(
          "DEBUG: GPU diagnostics deferred: guest main thread not launched yet "
          "(vblank_id={}, since_first_vblank={}ms)",
          vblank_id, since_first_vblank);
    }

        bool should_log_prelaunch_snapshot = (prelaunch_id <= 5);
        uint64_t last_prelaunch_snapshot =
          last_prelaunch_snapshot_ms.load(std::memory_order_relaxed);
        if (!should_log_prelaunch_snapshot && since_first_vblank >= 1000) {
          if (last_prelaunch_snapshot == 0 ||
            now_ms - last_prelaunch_snapshot >= 1000) {
          should_log_prelaunch_snapshot = true;
          }
        }
        if (should_log_prelaunch_snapshot) {
          last_prelaunch_snapshot_ms.store(now_ms, std::memory_order_relaxed);

          uint64_t swap_count =
            command_processor_ ? command_processor_->swap_packet_count() : 0;
          uint64_t refresh_success_count =
            presenter_ ? presenter_->GetGuestOutputRefreshSuccessCount() : 0;
          bool rb_initialized =
            command_processor_ ? command_processor_->IsRingBufferInitialized()
                     : false;

          uint64_t vd_init_ring_count =
            vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_set_cb_count =
            vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_swap_count =
            vd_swap_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_sys_cb_count = vd_get_system_cmd_buffer_state_.count.load(
            std::memory_order_relaxed);
          uint64_t vd_rptr_wb_count = vd_enable_rptr_writeback_state_.count.load(
            std::memory_order_relaxed);

          auto executable_module =
            kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
          const char* executable_name =
            executable_module ? executable_module->name().c_str() : "<none>";
          uint32_t executable_entry =
            executable_module ? executable_module->entry_point() : 0;

          XELOGW(
            "DEBUG: PRELAUNCH GPU snapshot: vblank_id={} age={}ms module='{}' "
            "entry={:08X} rb_init={} swap_packets={} refresh_success={} "
            "interrupt_callback_set={}",
            vblank_id, since_first_vblank, executable_name, executable_entry,
            rb_initialized ? "YES" : "NO", swap_count, refresh_success_count,
            interrupt_callback_ != 0 ? "YES" : "NO");
          XELOGW(
            "DEBUG: PRELAUNCH GPU Vd counters: InitRB={} SetCb={} Swap={} "
            "GetSysCB={} EnableRPtrWB={}",
            vd_init_ring_count, vd_set_cb_count, vd_swap_count, vd_sys_cb_count,
            vd_rptr_wb_count);

          if (command_processor_) {
          auto rb_state = command_processor_->GetRingBufferDebugState();
          XELOGW(
            "DEBUG: PRELAUNCH GPU ring state: rb_base={:08X} rb_size={:08X} "
            "rptr={:08X} wptr={:08X} init_ack={} init_requests={} "
            "init_ack_transitions={}",
            rb_state.primary_buffer_ptr, rb_state.primary_buffer_size,
            rb_state.read_ptr_index, rb_state.write_ptr_index,
            rb_state.initialization_acknowledged ? "YES" : "NO",
            rb_state.init_request_count, rb_state.init_ack_transition_count);
          }
        }

      if (cvars::gpu_prelaunch_bootstrap_nudge) {
        uint64_t nudge_after_ms =
          static_cast<uint64_t>(cvars::gpu_prelaunch_bootstrap_nudge_after_ms);
        if (nudge_after_ms > 0 && since_first_vblank >= nudge_after_ms &&
          !prelaunch_bootstrap_nudge_done_.load(std::memory_order_relaxed)) {
        uint64_t vd_init_ring_count =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
        uint64_t vd_set_cb_count =
          vd_set_interrupt_callback_state_.count.load(
            std::memory_order_relaxed);
        uint64_t vd_swap_count =
          vd_swap_state_.count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_base_writes =
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_cntl_writes =
          mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_wptr_writes =
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
        bool no_bootstrap_activity =
          vd_init_ring_count == 0 && vd_set_cb_count == 0 &&
          vd_swap_count == 0 && mmio_rb_base_writes == 0 &&
          mmio_rb_cntl_writes == 0 && mmio_rb_wptr_writes == 0;

        if (no_bootstrap_activity) {
          ForceFallbackDelaySnapshot prelaunch_bootstrap_delay_snapshot;
          bool defer_prelaunch_bootstrap_nudge =
              ShouldDeferForceFallback(kernel_state_,
                                       &prelaunch_bootstrap_delay_snapshot);
          if (defer_prelaunch_bootstrap_nudge) {
            static std::atomic<uint64_t>
                prelaunch_bootstrap_defer_last_log_ms{0};
            LogForceFallbackDeferred("PRELAUNCH bootstrap nudge",
                                     since_first_vblank, now_ms,
                                     prelaunch_bootstrap_delay_snapshot,
                                     &prelaunch_bootstrap_defer_last_log_ms);
            if (cvars::gpu_no_force_run_profile && command_processor_) {
              auto ensure_prelaunch_probe_ring_configured = [&]() {
                auto rb_state = command_processor_->GetRingBufferDebugState();
                bool ring_configured = rb_state.ring_buffer_initialized &&
                                       rb_state.primary_buffer_ptr != 0 &&
                                       rb_state.primary_buffer_size != 0;
                if (ring_configured ||
                    !cvars::gpu_bootstrap_probe_prime_ring_if_missing) {
                  return ring_configured;
                }
                uint32_t size_log2 = cvars::gpu_force_init_ring_buffer_size_log2;
                if (size_log2 == 0 || size_log2 > 24) {
                  size_log2 = 18;
                }
                uint32_t size_bytes = uint32_t(1) << (size_log2 + 3);
                if (!forced_ring_buffer_ptr_ && kernel_state_) {
                  forced_ring_buffer_ptr_ =
                      kernel_state_->memory()->SystemHeapAlloc(
                          size_bytes, 4096, kSystemHeapPhysical);
                  forced_ring_buffer_size_log2_ = size_log2;
                }
                if (!forced_ring_buffer_ptr_) {
                  XELOGW(
                      "DEBUG: PRELAUNCH no-force bootstrap probe ring-prime "
                      "skipped: allocation failed (size_log2={} bytes={})",
                      size_log2, size_bytes);
                  return false;
                }
                XELOGW(
                    "DEBUG: PRELAUNCH no-force bootstrap probe ring-prime: "
                    "initializing ring (rb_base={:08X} size_log2={} bytes={})",
                    forced_ring_buffer_ptr_, forced_ring_buffer_size_log2_,
                    size_bytes);
                command_processor_->InitializeRingBuffer(forced_ring_buffer_ptr_,
                                                         forced_ring_buffer_size_log2_);
                rb_state = command_processor_->GetRingBufferDebugState();
                ring_configured = rb_state.ring_buffer_initialized &&
                                  rb_state.primary_buffer_ptr != 0 &&
                                  rb_state.primary_buffer_size != 0;
                XELOGI(
                    "DEBUG: PRELAUNCH no-force bootstrap probe ring-prime result: "
                    "ring_init={} init_ack={} rb_base={:08X} rb_size={:08X}",
                    rb_state.ring_buffer_initialized ? "YES" : "NO",
                    rb_state.initialization_acknowledged ? "YES" : "NO",
                    rb_state.primary_buffer_ptr, rb_state.primary_buffer_size);
                return ring_configured;
              };
              auto prelaunch_rb_state =
                command_processor_->GetRingBufferDebugState();
              bool prelaunch_rb_ready =
                prelaunch_rb_state.ring_buffer_initialized &&
                prelaunch_rb_state.primary_buffer_ptr != 0 &&
                prelaunch_rb_state.primary_buffer_size != 0;
              if (!prelaunch_rb_ready) {
                prelaunch_rb_ready = ensure_prelaunch_probe_ring_configured();
              }
              if (prelaunch_rb_ready &&
                !prelaunch_bootstrap_nudge_done_.exchange(
                  true, std::memory_order_relaxed)) {
              uint64_t nudge_id = prelaunch_bootstrap_nudge_count_.fetch_add(
                           1, std::memory_order_relaxed) +
                         1;
              last_prelaunch_bootstrap_nudge_ms_.store(
                now_ms, std::memory_order_relaxed);
              XELOGW(
                "DEBUG: PRELAUNCH bootstrap no-force probe #{}: injecting "
                "guest-style MMIO ring activity (without force/fallback "
                "init) after {}ms of zero bootstrap activity",
                nudge_id, since_first_vblank);
              InjectGuestBootstrapDebugActivity(
                "prelaunch-no-force-bootstrap-probe", since_first_vblank);
              TryAutoRecoverMissingInterruptCallback(
                  now_ms, since_first_vblank,
                  "prelaunch-no-force-bootstrap-probe");
              }
            }
          } else if (!prelaunch_bootstrap_nudge_done_.exchange(
                         true, std::memory_order_relaxed)) {
            uint64_t nudge_id =
              prelaunch_bootstrap_nudge_count_.fetch_add(
                1, std::memory_order_relaxed) +
              1;
            last_prelaunch_bootstrap_nudge_ms_.store(now_ms,
                                 std::memory_order_relaxed);
            XELOGW(
              "DEBUG: PRELAUNCH bootstrap nudge #{}: guest main thread still not "
              "launched after {}ms with zero Vd/MMIO bootstrap activity; "
              "forcing fallback init + guest bootstrap injection",
              nudge_id, since_first_vblank);
            if (command_processor_) {
            command_processor_->ForceFallbackInitialization();
            }
            InjectGuestBootstrapDebugActivity("prelaunch-bootstrap-nudge",
                            since_first_vblank);
          }
        }
        }
      }

        if (command_processor_) {
        static std::atomic<uint64_t> postmodule_probe_last_ms{0};
        static std::atomic<bool> postmodule_probe_done{false};

        uint64_t probe_after_module_end_ms = static_cast<uint64_t>(
          cvars::gpu_postmodule_bootstrap_probe_after_ms);
        if (probe_after_module_end_ms > 0 && module_load_end_seen &&
          !module_load_stats.load_inflight &&
          module_load_stats.loaded_executable_user_modules > 0 &&
          since_last_module_load_end_ms >= probe_after_module_end_ms) {
          uint64_t vd_init_ring_count =
            vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_set_cb_count =
            vd_set_interrupt_callback_state_.count.load(
              std::memory_order_relaxed);
          uint64_t vd_swap_count =
            vd_swap_state_.count.load(std::memory_order_relaxed);
          uint64_t mmio_rb_base_writes =
            mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
          uint64_t mmio_rb_cntl_writes =
            mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
          uint64_t mmio_rb_wptr_writes =
            mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
          bool no_bootstrap_activity =
            vd_init_ring_count == 0 && vd_set_cb_count == 0 &&
            vd_swap_count == 0 && mmio_rb_base_writes == 0 &&
            mmio_rb_cntl_writes == 0 && mmio_rb_wptr_writes == 0;
          if (no_bootstrap_activity) {
          bool already_done =
            postmodule_probe_done.load(std::memory_order_relaxed);
          if (!cvars::gpu_postmodule_bootstrap_probe_once || !already_done) {
            uint64_t last_probe_ms =
              postmodule_probe_last_ms.load(std::memory_order_relaxed);
            uint64_t probe_interval_ms = static_cast<uint64_t>(
              cvars::gpu_postmodule_bootstrap_probe_interval_ms);
            bool interval_due =
              (last_probe_ms == 0 || probe_interval_ms == 0 ||
               now_ms >= last_probe_ms + probe_interval_ms);
            auto postmodule_rb_state =
              command_processor_->GetRingBufferDebugState();
            bool ring_ready =
              postmodule_rb_state.ring_buffer_initialized &&
              postmodule_rb_state.initialization_acknowledged &&
              postmodule_rb_state.primary_buffer_ptr != 0 &&
              postmodule_rb_state.primary_buffer_size != 0;
            if (interval_due && ring_ready) {
            postmodule_probe_last_ms.store(now_ms,
                             std::memory_order_relaxed);
            postmodule_probe_done.store(true, std::memory_order_relaxed);
            XELOGW(
              "DEBUG: POSTMODULE bootstrap probe: module-load ended but "
              "guest-main is still not ready; Vd/MMIO bootstrap "
              "activity remains zero, injecting guest-style MMIO "
              "bootstrap activity (since_module_end={}ms, "
              "no_force_profile={})",
              since_last_module_load_end_ms,
              cvars::gpu_no_force_run_profile ? "ON" : "OFF");
            if (!cvars::gpu_no_force_run_profile) {
              command_processor_->ForceFallbackInitialization();
            }
            InjectGuestBootstrapDebugActivity(
              "postmodule-no-guest-main-bootstrap",
              since_last_module_load_end_ms);
            } else if (!ring_ready) {
            static std::atomic<uint64_t> postmodule_probe_defer_log_ms{0};
            uint64_t last_defer_log =
              postmodule_probe_defer_log_ms.load(std::memory_order_relaxed);
            if (last_defer_log == 0 || now_ms - last_defer_log >= 1000) {
              postmodule_probe_defer_log_ms.store(
                now_ms, std::memory_order_relaxed);
              XELOGW(
                "DEBUG: POSTMODULE bootstrap probe deferred: ring not ready "
                "(ring_init={} init_ack={} rb_base={:08X} "
                "rb_size={:08X})",
                postmodule_rb_state.ring_buffer_initialized ? "YES"
                                      : "NO",
                postmodule_rb_state.initialization_acknowledged ? "YES"
                                         : "NO",
                postmodule_rb_state.primary_buffer_ptr,
                postmodule_rb_state.primary_buffer_size);
            }
            }
          }
          }
        }
        }

    if (cvars::gpu_prelaunch_guest_main_launch_nudge) {
      uint64_t launch_nudge_after_ms = static_cast<uint64_t>(
          cvars::gpu_prelaunch_guest_main_launch_nudge_after_ms);
      if (since_first_vblank >= launch_nudge_after_ms &&
          !prelaunch_guest_main_launch_nudge_done_.load(
              std::memory_order_relaxed)) {
        uint64_t retry_interval_ms = static_cast<uint64_t>(
            cvars::gpu_prelaunch_guest_main_launch_nudge_retry_interval_ms);
        uint64_t last_attempt_ms =
            last_prelaunch_guest_main_launch_nudge_ms_.load(
                std::memory_order_relaxed);
        bool retry_due =
            (last_attempt_ms == 0 || retry_interval_ms == 0 ||
             now_ms >= last_attempt_ms + retry_interval_ms);
        if (retry_due) {
          bool module_load_gate_blocks_guest_main_launch =
              module_load_stats.load_inflight ||
              module_load_stats.loaded_executable_user_modules == 0;
          if (module_load_gate_blocks_guest_main_launch) {
            static std::atomic<uint64_t> guest_main_load_gate_log_count{0};
            uint64_t gate_log_id = guest_main_load_gate_log_count.fetch_add(1) + 1;
            if (gate_log_id <= 3 || (gate_log_id % 120) == 0) {
              XELOGW(
                  "DEBUG: PRELAUNCH guest-main nudge deferred: waiting for "
                  "module-load gate to clear "
                  "(age={}ms load_inflight={} loaded_executable_user_modules={} "
                  "load_end_seen={} since_load_end={}ms load_progress_count={})",
                  since_first_vblank,
                  module_load_stats.load_inflight ? "YES" : "NO",
                  module_load_stats.loaded_executable_user_modules,
                  module_load_end_seen ? "YES" : "NO",
                  since_last_module_load_end_ms,
                  module_load_stats.load_progress_count);
            }
          } else {
            ForceFallbackDelaySnapshot guest_main_launch_delay_snapshot;
            bool defer_guest_main_launch_nudge =
                ShouldDeferForceFallback(kernel_state_,
                                         &guest_main_launch_delay_snapshot);
            if (defer_guest_main_launch_nudge) {
              static std::atomic<uint64_t> guest_main_launch_defer_last_log_ms{0};
              LogForceFallbackDeferred("PRELAUNCH guest-main launch nudge",
                                       since_first_vblank, now_ms,
                                       guest_main_launch_delay_snapshot,
                                       &guest_main_launch_defer_last_log_ms);
            } else {
              auto executable_module =
                  kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
              if (!executable_module) {
                static std::atomic<uint64_t> no_module_log_count{0};
                uint64_t no_module_id = no_module_log_count.fetch_add(1) + 1;
                if (no_module_id <= 3 || (no_module_id % 120) == 0) {
                  XELOGW(
                      "DEBUG: PRELAUNCH guest-main nudge deferred: no executable module "
                      "available yet (age={}ms load_inflight={} "
                      "loaded_executable_user_modules={})",
                      since_first_vblank,
                      module_load_stats.load_inflight ? "YES" : "NO",
                      module_load_stats.loaded_executable_user_modules);
                }
              } else if (executable_module->entry_point() == 0) {
                static std::atomic<uint64_t> entry_unset_log_count{0};
                uint64_t entry_log_id = entry_unset_log_count.fetch_add(1) + 1;
                if (entry_log_id <= 3 || (entry_log_id % 120) == 0) {
                  XELOGW(
                      "DEBUG: PRELAUNCH guest-main nudge deferred: executable "
                      "module='{}' has entry=0 (load_end_seen={} "
                      "since_load_end={}ms)",
                      executable_module->name(),
                      module_load_end_seen ? "YES" : "NO",
                      since_last_module_load_end_ms);
                }
              } else {
                uint64_t nudge_id =
                    prelaunch_guest_main_launch_nudge_count_.fetch_add(
                        1, std::memory_order_relaxed) +
                    1;
                last_prelaunch_guest_main_launch_nudge_ms_.store(
                    now_ms, std::memory_order_relaxed);
                XELOGW(
                    "DEBUG: PRELAUNCH guest-main launch nudge #{}: attempting "
                    "KernelState::LaunchModule for executable='{}' entry={:08X} "
                    "after {}ms prelaunch age",
                    nudge_id, executable_module->name(),
                    executable_module->entry_point(), since_first_vblank);
                auto launched_thread = kernel_state_->LaunchModule(executable_module);
                if (launched_thread) {
                  prelaunch_guest_main_launch_nudge_done_.store(
                      true, std::memory_order_relaxed);
                  XELOGW(
                      "DEBUG: PRELAUNCH guest-main launch nudge #{} succeeded: "
                      "thread_handle={:08X} thread_id={} running={}",
                      nudge_id, launched_thread->handle(),
                      launched_thread->thread_id(),
                      launched_thread->is_running() ? "YES" : "NO");
                } else {
                  XELOGE(
                      "DEBUG: PRELAUNCH guest-main launch nudge #{} failed: "
                      "KernelState::LaunchModule returned null; will retry in "
                      "{}ms",
                      nudge_id, retry_interval_ms);
                }
              }
            }
          }
        }
      }
    }

    if (interrupt_callback_ == 0) {
      ForceFallbackDelaySnapshot prelaunch_callback_delay_snapshot;
      if (ShouldDeferForceFallback(kernel_state_,
                                   &prelaunch_callback_delay_snapshot)) {
        static std::atomic<uint64_t> prelaunch_callback_defer_last_log_ms{0};
        LogForceFallbackDeferred("PRELAUNCH callback recovery",
                                 since_first_vblank, now_ms,
                                 prelaunch_callback_delay_snapshot,
                                 &prelaunch_callback_defer_last_log_ms);
      }
      TryAutoRecoverMissingInterruptCallback(now_ms, since_first_vblank,
                                             "prelaunch-missing-callback");
    }
  } else {
    static std::atomic<bool> guest_main_launch_logged{false};
    if (!guest_main_launch_logged.exchange(true, std::memory_order_relaxed)) {
          uint64_t vd_init_ring_count =
            vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_set_cb_count =
            vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_swap_count =
            vd_swap_state_.count.load(std::memory_order_relaxed);
          uint64_t refresh_success_count =
            presenter_ ? presenter_->GetGuestOutputRefreshSuccessCount() : 0;
      XELOGI(
          "GUEST MAIN THREAD: Successfully launched a main thread under "
            "guest. (vblank_id={}, since_first_vblank={}ms, running={}, "
            "vd_init_rb={}, vd_set_cb={}, vd_swap={}, refresh_success={})",
          vblank_id, since_first_vblank,
            guest_main_thread_running ? "YES" : "NO", vd_init_ring_count,
            vd_set_cb_count, vd_swap_count, refresh_success_count);
    }
    static std::atomic<uint64_t> guest_main_heartbeat_log_count{0};
    uint64_t heartbeat_id =
        guest_main_heartbeat_log_count.fetch_add(1) + 1;
    if (heartbeat_id <= 3 || (heartbeat_id % 240) == 0) {
      XELOGI(
          "GUEST MAIN THREAD: Heartbeat (vblank_id={}, "
          "since_first_vblank={}ms, running={})",
          vblank_id, since_first_vblank,
          guest_main_thread_running ? "YES" : "NO");
      XELOGI(
          "RING BUFFER: guest main thread heartbeat (vblank_id={}, "
          "since_first_vblank={}ms, running={})",
          vblank_id, since_first_vblank,
          guest_main_thread_running ? "YES" : "NO");
    }

    if (guest_main_thread_running && interrupt_callback_ == 0) {
      TryAutoRecoverMissingInterruptCallback(now_ms, since_first_vblank,
                                             "guest-main-running");
    }
    if (guest_main_thread_running && command_processor_) {
      static std::atomic<uint64_t> postlaunch_probe_last_ms{0};
      static std::atomic<bool> postlaunch_probe_done{false};

      uint64_t probe_after_ms =
          static_cast<uint64_t>(cvars::gpu_postlaunch_bootstrap_probe_after_ms);
      if (probe_after_ms > 0 && since_first_vblank >= probe_after_ms) {
        uint64_t vd_init_ring_count =
            vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
        uint64_t vd_set_cb_count = vd_set_interrupt_callback_state_.count.load(
            std::memory_order_relaxed);
        uint64_t vd_swap_count =
            vd_swap_state_.count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_base_writes =
            mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_cntl_writes =
            mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_wptr_writes =
            mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
        bool no_bootstrap_activity =
            vd_init_ring_count == 0 && vd_set_cb_count == 0 &&
            vd_swap_count == 0 && mmio_rb_base_writes == 0 &&
            mmio_rb_cntl_writes == 0 && mmio_rb_wptr_writes == 0;
        if (no_bootstrap_activity) {
          bool already_done = postlaunch_probe_done.load(std::memory_order_relaxed);
          if (!cvars::gpu_postlaunch_bootstrap_probe_once || !already_done) {
            uint64_t last_probe_ms =
                postlaunch_probe_last_ms.load(std::memory_order_relaxed);
            uint64_t probe_interval_ms = static_cast<uint64_t>(
                cvars::gpu_postlaunch_bootstrap_probe_interval_ms);
            bool interval_due =
                (last_probe_ms == 0 || probe_interval_ms == 0 ||
                 now_ms >= last_probe_ms + probe_interval_ms);
            auto postlaunch_rb_state = command_processor_->GetRingBufferDebugState();
            bool ring_ready = postlaunch_rb_state.ring_buffer_initialized &&
                              postlaunch_rb_state.initialization_acknowledged &&
                              postlaunch_rb_state.primary_buffer_ptr != 0 &&
                              postlaunch_rb_state.primary_buffer_size != 0;
            if (interval_due && ring_ready) {
              postlaunch_probe_last_ms.store(now_ms, std::memory_order_relaxed);
              postlaunch_probe_done.store(true, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: POSTLAUNCH bootstrap probe: guest main running but "
                  "Vd/MMIO bootstrap activity is still zero; injecting "
                  "guest-style MMIO ring bootstrap activity "
                  "(no_force_profile={})",
                  cvars::gpu_no_force_run_profile ? "ON" : "OFF");
              if (!cvars::gpu_no_force_run_profile) {
                command_processor_->ForceFallbackInitialization();
              }
              InjectGuestBootstrapDebugActivity(
                  "postlaunch-running-no-bootstrap", since_first_vblank);
            } else if (!ring_ready) {
              static std::atomic<uint64_t> postlaunch_probe_defer_log_ms{0};
              uint64_t last_defer_log = postlaunch_probe_defer_log_ms.load(
                  std::memory_order_relaxed);
              if (last_defer_log == 0 || now_ms - last_defer_log >= 1000) {
                postlaunch_probe_defer_log_ms.store(now_ms,
                                                    std::memory_order_relaxed);
                XELOGW(
                    "DEBUG: POSTLAUNCH bootstrap probe deferred: ring not ready "
                    "(ring_init={} init_ack={} rb_base={:08X} rb_size={:08X})",
                    postlaunch_rb_state.ring_buffer_initialized ? "YES" : "NO",
                    postlaunch_rb_state.initialization_acknowledged ? "YES" : "NO",
                    postlaunch_rb_state.primary_buffer_ptr,
                    postlaunch_rb_state.primary_buffer_size);
              }
            }
          }
        }
      }
    }
  }

  if (cvars::gpu_debug_gate_watch) {
    static std::atomic<uint64_t> gate_count{0};
    static std::atomic<uint32_t> last_gate_value{0xFFFFFFFFu};
    static std::atomic<bool> last_gate_valid{false};
    uint64_t gate_id = gate_count.fetch_add(1) + 1;
    uint32_t every = cvars::gpu_debug_gate_watch_every_n;
    bool log_this = (every == 0) || (gate_id % every) == 0;
    BootstrapGateSnapshot gate_snapshot = ReadBootstrapGateSnapshot(kernel_state_);
    bool changed =
        gate_snapshot.committed && gate_snapshot.translated &&
        (!last_gate_valid.load(std::memory_order_relaxed) ||
         gate_snapshot.value != last_gate_value.load(std::memory_order_relaxed));
    if (cvars::gpu_debug_gate_watch_log_on_change_only) {
      log_this = changed;
    }
    if (log_this) {
      XELOGI(
          "DEBUG: GATE WATCH #{} vblank_id={} addr={:08X} value=0x{:08X} "
          "committed={} translated={} changed={} since_first_vblank_ms={}",
          gate_id, vblank_id, gate_snapshot.address, gate_snapshot.value,
          gate_snapshot.committed ? "YES" : "NO",
          gate_snapshot.translated ? "YES" : "NO",
          changed ? "YES" : "NO", since_first_vblank);
    }
    if (gate_snapshot.committed && gate_snapshot.translated) {
      last_gate_value.store(gate_snapshot.value, std::memory_order_relaxed);
      last_gate_valid.store(true, std::memory_order_relaxed);
    }
  }
  if (log_vblank && cvars::gpu_log_vblank_only_on_activity) {
    uint64_t idle_suppress_ms =
        static_cast<uint64_t>(cvars::gpu_log_vblank_idle_suppress_ms);
    uint32_t last_read = last_logged_read.load(std::memory_order_relaxed);
    uint32_t last_write = last_logged_write.load(std::memory_order_relaxed);
    uint64_t last_ms = last_logged_ms.load(std::memory_order_relaxed);
    bool activity_changed = (read_ptr != last_read || write_ptr != last_write);
    bool interval_elapsed = (idle_suppress_ms == 0) || (last_ms == 0) ||
                            (now_ms - last_ms >= idle_suppress_ms);
    if (!activity_changed && !interval_elapsed) {
      log_vblank = false;
    }
  }
  if (log_vblank) {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    XELOGI(
        "DEBUG: VBLANK #{} counter={} read_ptr={:08X} write_ptr={:08X} thread={}",
        vblank_id, command_processor_ ? command_processor_->counter() : 0,
        read_ptr, write_ptr, oss.str());
    last_logged_read.store(read_ptr, std::memory_order_relaxed);
    last_logged_write.store(write_ptr, std::memory_order_relaxed);
    last_logged_ms.store(Clock::QueryHostUptimeMillis(),
                         std::memory_order_relaxed);
  }
  bool bootstrap_ready_for_gpu_progress = false;
  CommandProcessor::RingBufferDebugState bootstrap_state{};
  if (command_processor_) {
    bootstrap_state = command_processor_->GetRingBufferDebugState();
    bootstrap_ready_for_gpu_progress =
        bootstrap_state.primary_buffer_ptr != 0 &&
        bootstrap_state.ring_buffer_initialized &&
        bootstrap_state.initialization_acknowledged;
  }
  auto maybe_force_gpu_init_if_missing = [&]() {
    if (!guest_main_thread_ready || !cvars::gpu_force_init_if_missing ||
        cvars::gpu_no_force_run_profile ||
        forced_gpu_init_done_.load(std::memory_order_relaxed)) {
      return;
    }

    uint64_t vd_init_count =
        vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_cb_count =
        vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_swap_count =
        vd_swap_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_sys_count =
        vd_get_system_cmd_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_base_writes =
        mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_cntl_writes =
        mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);

    uint32_t force_after = cvars::gpu_force_init_after_vblank;
    if (force_after != 0 && vblank_id < force_after) {
      return;
    }
    ForceFallbackDelaySnapshot force_init_delay_snapshot;
    if (ShouldDeferForceFallback(kernel_state_, &force_init_delay_snapshot)) {
      static std::atomic<uint64_t> force_init_defer_last_log_ms{0};
      LogForceFallbackDeferred("RING BUFFER force-init watchdog",
                               since_first_vblank, now_ms,
                               force_init_delay_snapshot,
                               &force_init_defer_last_log_ms);
      return;
    }

    bool need_init = bootstrap_state.primary_buffer_ptr == 0 ||
                     !bootstrap_state.ring_buffer_initialized;
    bool need_cb = interrupt_callback_ == 0 && vd_cb_count == 0;
    if (!need_init && !need_cb) {
      forced_gpu_init_done_.store(true, std::memory_order_relaxed);
      XELOGI(
          "RING BUFFER: force-init watchdog skipped (already ready) "
          "(vblank_id={}, rb_base={:08X}, ring_init={}, init_ack={}, "
          "callback_set={})",
          vblank_id, bootstrap_state.primary_buffer_ptr,
          bootstrap_state.ring_buffer_initialized ? "YES" : "NO",
          bootstrap_state.initialization_acknowledged ? "YES" : "NO",
          interrupt_callback_ != 0 ? "YES" : "NO");
      return;
    }

    uint64_t last_wptr_ms = last_wptr_update_ms_.load(std::memory_order_relaxed);
    uint64_t last_wptr_age = (last_wptr_ms && now_ms >= last_wptr_ms)
                                 ? (now_ms - last_wptr_ms)
                                 : 0;
    XELOGW(
        "RING BUFFER: force-init watchdog firing mode={} "
        "(vblank_id={}, since_first_vblank={}ms, need_init={}, need_cb={}, "
        "vd_init={}, vd_cb={}, vd_swap={}, vd_sys={}, mmio_rb_base={}, "
        "mmio_rb_cntl={}, last_wptr_age_ms={}, rb_read={:08X}, rb_write={:08X})",
        "config", vblank_id, since_first_vblank, need_init ? "YES" : "NO",
        need_cb ? "YES" : "NO", vd_init_count, vd_cb_count, vd_swap_count,
        vd_sys_count, mmio_rb_base_writes, mmio_rb_cntl_writes, last_wptr_age,
        read_ptr, write_ptr);

    if (need_init) {
      if (!forced_ring_buffer_ptr_) {
        uint32_t size_log2 = cvars::gpu_force_init_ring_buffer_size_log2;
        uint32_t size_bytes = uint32_t(1) << (size_log2 + 3);
        forced_ring_buffer_ptr_ = kernel_state_->memory()->SystemHeapAlloc(
            size_bytes, 4096, kSystemHeapPhysical);
        forced_ring_buffer_size_log2_ = size_log2;
        if (forced_ring_buffer_ptr_) {
          XELOGW(
              "RING BUFFER: force-init allocated ring buffer ptr={:08X} "
              "size_log2={} bytes={}",
              forced_ring_buffer_ptr_, size_log2, size_bytes);
        } else {
          XELOGE("RING BUFFER: force-init failed to allocate ring buffer");
        }
      }
      if (forced_ring_buffer_ptr_) {
        InitializeRingBuffer(forced_ring_buffer_ptr_,
                             forced_ring_buffer_size_log2_);
      }
    }

    if (need_cb) {
      if (cvars::gpu_allow_forced_interrupt_callback_fallback) {
        if (forced_interrupt_callback_ == 0 &&
            !forced_interrupt_callback_failed_) {
          if (!processor_) {
            XELOGW("RING BUFFER: force-init cannot define callback (no CPU)");
            forced_interrupt_callback_failed_ = true;
          } else {
            auto* function = processor_->DefineBuiltin(
                "ForcedGpuInterruptCallback", &ForcedGpuInterruptCallback, this,
                nullptr);
            if (!function) {
              XELOGW(
                  "RING BUFFER: force-init failed to define callback builtin");
              forced_interrupt_callback_failed_ = true;
            } else {
              forced_interrupt_callback_ = function->address();
              XELOGW(
                  "RING BUFFER: force-init callback builtin created at {:08X}",
                  forced_interrupt_callback_);
            }
          }
        }
        if (forced_interrupt_callback_) {
          SetInterruptCallback(forced_interrupt_callback_, 0);
        }
      } else {
        XELOGW(
            "RING BUFFER: force-init skipped callback injection "
            "(gpu_allow_forced_interrupt_callback_fallback=false)");
      }
    }

    forced_gpu_init_done_.store(true, std::memory_order_relaxed);
  };
  maybe_force_gpu_init_if_missing();
  if (command_processor_) {
    bootstrap_state = command_processor_->GetRingBufferDebugState();
    bootstrap_ready_for_gpu_progress =
        bootstrap_state.primary_buffer_ptr != 0 &&
        bootstrap_state.ring_buffer_initialized &&
        bootstrap_state.initialization_acknowledged;
  }

    uint64_t vd_init_engines_count =
      vd_init_engines_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_init_count =
      vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
    uint64_t vd_cb_count =
      vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_base_writes =
      mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_cntl_writes =
      mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
    uint64_t mmio_rb_wptr_writes =
      mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
    auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();

    bool bootstrap_activity_observed =
      vd_init_engines_count > 0 || vd_init_count > 0 || vd_cb_count > 0 ||
      mmio_rb_base_writes > 0 || mmio_rb_cntl_writes > 0 ||
      mmio_rb_wptr_writes > 0 || proc_stats.proc_lookup_calls > 0 ||
      interrupt_callback_ != 0;

  if (guest_main_thread_ready &&
      cvars::gpu_suspend_nonessential_until_ring_init_ack &&
      !bootstrap_ready_for_gpu_progress && bootstrap_activity_observed) {
    static std::atomic<uint64_t> bootstrap_gate_log_count{0};
    uint64_t gate_log_id = bootstrap_gate_log_count.fetch_add(1) + 1;
    uint32_t every_n =
        cvars::gpu_suspend_nonessential_bootstrap_log_every_n_vblank;
    if (gate_log_id <= 5 || every_n == 0 || (gate_log_id % every_n) == 0) {
      XELOGW(
          "RING BUFFER: GPU bootstrap gate active: waiting for ring init ack "
          "(vblank_id={}, since_first_vblank={}ms, ring_init={}, init_ack={}, "
          "rb_base={:08X}, rb_size={:08X}, read_ptr={:08X}, write_ptr={:08X}, "
          "callback_set={})",
          vblank_id, since_first_vblank,
          bootstrap_state.ring_buffer_initialized ? "YES" : "NO",
          bootstrap_state.initialization_acknowledged ? "YES" : "NO",
          bootstrap_state.primary_buffer_ptr, bootstrap_state.primary_buffer_size,
          read_ptr, write_ptr, interrupt_callback_ != 0 ? "YES" : "NO");
    }

    {
      auto find_ordinal_stat = [&](uint16_t ordinal)
          -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
        for (const auto& stat : proc_stats.ordinal_runtime_stats) {
          if (stat.ordinal == ordinal) {
            return &stat;
          }
        }
        return nullptr;
      };

      const auto* stat_01C2 = find_ordinal_stat(0x01C2);
      const auto* stat_01C3 = find_ordinal_stat(0x01C3);
      const auto* stat_01D5 = find_ordinal_stat(0x01D5);
      const auto* stat_025B = find_ordinal_stat(0x025B);

      uint64_t progress_mask = 0;
      if (bootstrap_state.worker_setup_complete) {
        progress_mask |= (uint64_t(1) << 0);
      }
      if (bootstrap_state.worker_setup_failed) {
        progress_mask |= (uint64_t(1) << 1);
      }
      if (vd_init_engines_count > 0) {
        progress_mask |= (uint64_t(1) << 2);
      }
      if (vd_init_count > 0) {
        progress_mask |= (uint64_t(1) << 3);
      }
      if (vd_cb_count > 0) {
        progress_mask |= (uint64_t(1) << 4);
      }
      if (mmio_rb_base_writes > 0) {
        progress_mask |= (uint64_t(1) << 5);
      }
      if (mmio_rb_cntl_writes > 0) {
        progress_mask |= (uint64_t(1) << 6);
      }
      if (mmio_rb_wptr_writes > 0) {
        progress_mask |= (uint64_t(1) << 7);
      }
      if (stat_01C2 && stat_01C2->static_thunk_calls > 0) {
        progress_mask |= (uint64_t(1) << 8);
      }
      if (stat_01C2 && stat_01C2->export_entry_calls > 0) {
        progress_mask |= (uint64_t(1) << 9);
      }
      if (stat_01C3 && stat_01C3->static_thunk_calls > 0) {
        progress_mask |= (uint64_t(1) << 10);
      }
      if (stat_01C3 && stat_01C3->export_entry_calls > 0) {
        progress_mask |= (uint64_t(1) << 11);
      }
      if (stat_01D5 && stat_01D5->static_thunk_calls > 0) {
        progress_mask |= (uint64_t(1) << 12);
      }
      if (stat_01D5 && stat_01D5->export_entry_calls > 0) {
        progress_mask |= (uint64_t(1) << 13);
      }
      if (proc_stats.proc_lookup_calls > 0) {
        progress_mask |= (uint64_t(1) << 14);
      }
      if (interrupt_callback_ != 0) {
        progress_mask |= (uint64_t(1) << 15);
      }
      if (bootstrap_state.ring_buffer_initialized) {
        progress_mask |= (uint64_t(1) << 16);
      }
      if (bootstrap_state.initialization_acknowledged) {
        progress_mask |= (uint64_t(1) << 17);
      }

      static std::atomic<uint64_t> last_progress_mask{UINT64_MAX};
      static std::atomic<uint64_t> progress_transition_count{0};
      uint64_t previous_mask =
          last_progress_mask.exchange(progress_mask, std::memory_order_relaxed);
      bool progress_changed = previous_mask != progress_mask;
      if (progress_changed || gate_log_id <= 5 || every_n == 0 ||
          (gate_log_id % every_n) == 0) {
        uint64_t transition_id = progress_changed
                                     ? (progress_transition_count.fetch_add(
                                            1, std::memory_order_relaxed) +
                                        1)
                                     : progress_transition_count.load(
                                           std::memory_order_relaxed);
        XELOGW(
            "RING BUFFER: bootstrap progression #{} vblank_id={} "
            "worker_setup_complete={} worker_setup_failed={} "
            "ring_init={} init_ack={} callback_set={} "
            "vd_init_engines={} vd_init_rb={} vd_set_cb={} "
            "thunk_01C2={} export_01C2={} thunk_01C3={} export_01C3={} "
            "thunk_01D5={} export_01D5={} dynamic_lookup_total={} "
            "mmio_rb_base={} mmio_rb_cntl={} mmio_rb_wptr={} mask={:016X}",
            transition_id, vblank_id,
            bootstrap_state.worker_setup_complete ? "YES" : "NO",
            bootstrap_state.worker_setup_failed ? "YES" : "NO",
            bootstrap_state.ring_buffer_initialized ? "YES" : "NO",
            bootstrap_state.initialization_acknowledged ? "YES" : "NO",
            interrupt_callback_ != 0 ? "YES" : "NO", vd_init_engines_count,
            vd_init_count, vd_cb_count,
            stat_01C2 ? stat_01C2->static_thunk_calls : 0,
            stat_01C2 ? stat_01C2->export_entry_calls : 0,
            stat_01C3 ? stat_01C3->static_thunk_calls : 0,
            stat_01C3 ? stat_01C3->export_entry_calls : 0,
            stat_01D5 ? stat_01D5->static_thunk_calls : 0,
            stat_01D5 ? stat_01D5->export_entry_calls : 0,
            proc_stats.proc_lookup_calls, mmio_rb_base_writes,
            mmio_rb_cntl_writes, mmio_rb_wptr_writes, progress_mask);

        auto get_behavior_name = [](const cpu::Function* function)
            -> const char* {
          if (!function) {
            return "<none>";
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
            default:
              return "Unknown";
          }
        };

        auto log_bootstrap_import_readiness = [&](uint16_t ordinal) {
          kernel::object_ref<kernel::UserModule> executable_module;
          if (kernel_state_) {
            executable_module = kernel_state_->GetExecutableModule();
          }
          if (!executable_module || !executable_module->xex_module()) {
            XELOGW(
                "RING BUFFER: thunk readiness ordinal=0x{:03X} name={} "
                "import_map=UNAVAILABLE",
                ordinal, kernel::xboxkrnl::GetGpuBootstrapOrdinalName(ordinal));
            return;
          }

          bool imported = false;
          uint32_t value_addr = 0;
          uint32_t thunk_addr = 0;
          const auto* import_libs =
              executable_module->xex_module()->import_libraries();
          if (import_libs) {
            for (const auto& lib : *import_libs) {
              if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
                continue;
              }
              for (const auto& import_fn : lib.imports) {
                if (import_fn.ordinal != ordinal) {
                  continue;
                }
                imported = true;
                if (import_fn.value_address) {
                  value_addr = import_fn.value_address;
                }
                if (import_fn.thunk_address) {
                  thunk_addr = import_fn.thunk_address;
                }
              }
            }
          }

          uint32_t value_word = 0;
          bool value_translated = false;
          if (value_addr) {
            auto safe = memory_->TranslateVirtualSafe<uint32_t*>(value_addr);
            if (safe.success && safe.pointer) {
              value_word = ::xe::load_and_swap<uint32_t>(safe.pointer);
              value_translated = true;
            }
          }

          uint32_t thunk_word0 = 0;
          uint32_t thunk_word1 = 0;
          bool thunk_translated = false;
          bool thunk_has_sc2_stub = false;
          if (thunk_addr) {
            auto safe0 = memory_->TranslateVirtualSafe<uint32_t*>(thunk_addr);
            auto safe1 = memory_->TranslateVirtualSafe<uint32_t*>(
                thunk_addr + sizeof(uint32_t));
            if (safe0.success && safe0.pointer && safe1.success &&
                safe1.pointer) {
              thunk_word0 = ::xe::load_and_swap<uint32_t>(safe0.pointer);
              thunk_word1 = ::xe::load_and_swap<uint32_t>(safe1.pointer);
              thunk_translated = true;
              thunk_has_sc2_stub =
                  thunk_word0 == 0x44000042 && thunk_word1 == 0x4E800020;
            }
          }

          const cpu::Function* thunk_fn =
              (processor_ && thunk_addr) ? processor_->LookupFunction(thunk_addr)
                                         : nullptr;
          const cpu::Function* value_target_fn =
              (processor_ && value_word) ? processor_->LookupFunction(value_word)
                                         : nullptr;

          XELOGW(
              "RING BUFFER: thunk readiness ordinal=0x{:03X} name={} "
              "imported={} value_addr={:08X} value_word={:08X} "
              "value_translated={} value_fn_behavior={} value_fn_status={} "
              "thunk_addr={:08X} thunk_w0={:08X} thunk_w1={:08X} "
              "thunk_translated={} thunk_sc2_stub={} "
              "thunk_fn_behavior={} thunk_fn_status={}",
              ordinal, kernel::xboxkrnl::GetGpuBootstrapOrdinalName(ordinal),
              imported ? "YES" : "NO", value_addr, value_word,
              value_translated ? "YES" : "NO",
              get_behavior_name(value_target_fn),
              value_target_fn ? static_cast<int>(value_target_fn->status()) : -1,
              thunk_addr, thunk_word0, thunk_word1,
              thunk_translated ? "YES" : "NO",
              thunk_has_sc2_stub ? "YES" : "NO",
              get_behavior_name(thunk_fn),
              thunk_fn ? static_cast<int>(thunk_fn->status()) : -1);
        };

        log_bootstrap_import_readiness(0x01C2);
        log_bootstrap_import_readiness(0x01C3);
        log_bootstrap_import_readiness(0x01D5);

        struct BootstrapImportAddressProbe {
          uint16_t ordinal;
          uint32_t value_addr;
          uint32_t thunk_addr;
        };
        std::array<BootstrapImportAddressProbe, 3> import_probes{{
            {0x01C2, 0, 0},
            {0x01C3, 0, 0},
            {0x01D5, 0, 0},
        }};

        kernel::object_ref<kernel::UserModule> executable_module;
        if (kernel_state_) {
          executable_module = kernel_state_->GetExecutableModule();
        }
        if (executable_module && executable_module->xex_module()) {
          const auto* import_libs =
              executable_module->xex_module()->import_libraries();
          if (import_libs) {
            for (const auto& lib : *import_libs) {
              if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
                continue;
              }
              for (const auto& import_fn : lib.imports) {
                for (auto& probe : import_probes) {
                  if (import_fn.ordinal != probe.ordinal) {
                    continue;
                  }
                  if (import_fn.value_address) {
                    probe.value_addr = import_fn.value_address;
                  }
                  if (import_fn.thunk_address) {
                    probe.thunk_addr = import_fn.thunk_address;
                  }
                }
              }
            }
          }
        }

        auto decode_branch_target = [](uint32_t pc, uint32_t instr,
                                       uint32_t* target_out,
                                       bool* link_out) -> bool {
          uint32_t opcode = instr >> 26;
          if (opcode != 18) {
            return false;
          }
          bool aa = (instr & 0x2) != 0;
          bool lk = (instr & 0x1) != 0;
          uint32_t li = instr & 0x03FFFFFC;
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
          return true;
        };

        kernel::XThread* guest_main_thread = nullptr;
        if (kernel_state_ && kernel_state_->object_table()) {
          auto threads =
              kernel_state_->object_table()->GetObjectsByType<kernel::XThread>();
          for (const auto& thread_ref : threads) {
            auto* thread = thread_ref.get();
            if (!thread) {
              continue;
            }
            if (thread->is_guest_thread() && thread->main_thread()) {
              guest_main_thread = thread;
              break;
            }
          }
        }

        if (!guest_main_thread || !memory_) {
          XELOGW(
              "RING BUFFER: callsite probe unavailable guest_main={} memory={}",
              guest_main_thread ? "YES" : "NO", memory_ ? "YES" : "NO");
        } else {
          auto* thread_state = guest_main_thread->thread_state();
          auto* ctx = thread_state ? thread_state->context() : nullptr;
          if (!ctx) {
            XELOGW(
                "RING BUFFER: callsite probe unavailable (guest main has no "
                "PPC context)");
          } else {
            uint32_t pc = ctx->current_pc;
            uint32_t lr = static_cast<uint32_t>(ctx->lr);
            uint32_t ctr = static_cast<uint32_t>(ctx->ctr);
            const char* module_name = "<unknown>";
            const char* function_name = "<unknown>";
            uint32_t function_address = 0;
            if (processor_ && pc) {
              auto* module = processor_->LookupModule(pc);
              if (module) {
                module_name = module->name().c_str();
              }
              auto* function = processor_->LookupFunction(pc);
              if (function) {
                function_name = function->name().c_str();
                function_address = function->address();
              }
            }

            uint32_t pc_instr = 0;
            bool pc_instr_valid = false;
            uint32_t pc_branch_target = 0;
            bool pc_branch_link = false;
            bool pc_is_branch = false;
            auto pc_safe = memory_->TranslateVirtualSafe<uint32_t*>(pc);
            if (pc_safe.success && pc_safe.pointer) {
              pc_instr = ::xe::load_and_swap<uint32_t>(pc_safe.pointer);
              pc_instr_valid = true;
              pc_is_branch = decode_branch_target(pc, pc_instr,
                                                 &pc_branch_target,
                                                 &pc_branch_link);
            }

            XELOGW(
                "RING BUFFER: callsite probe main thread pc={:08X} lr={:08X} "
                "ctr={:08X} module={} fn={}({:08X}) pc_instr_valid={} "
                "pc_instr={:08X} pc_is_branch={} branch_target={:08X} "
                "branch_link={}",
                pc, lr, ctr, module_name, function_name, function_address,
                pc_instr_valid ? "YES" : "NO", pc_instr,
                pc_is_branch ? "YES" : "NO", pc_branch_target,
                pc_branch_link ? "YES" : "NO");

            static std::atomic<bool> logged_track_guest_pc_note{false};
            if (!cvars::track_guest_pc &&
              !logged_track_guest_pc_note.exchange(true,
                                 std::memory_order_relaxed)) {
              XELOGW(
                "RING BUFFER: callsite probe note: track_guest_pc=OFF; "
                "runtime current_pc/lr/ctr may remain zero for guest "
                "threads. Fallback entrypoint BL scan will be used.");
            }

            uint32_t guest_main_start = guest_main_thread->start_address();

            for (const auto& probe : import_probes) {
              uint32_t near_direct_branch_hits = 0;
              uint32_t nearest_branch_addr = 0;
              uint32_t nearest_branch_target = 0;
              uint32_t nearest_branch_distance = UINT32_MAX;

              if (pc && probe.thunk_addr) {
                constexpr int kWindowInstructions = 96;
                for (int i = -kWindowInstructions; i <= kWindowInstructions;
                     ++i) {
                  uint32_t addr = pc + static_cast<uint32_t>(i * 4);
                  auto safe = memory_->TranslateVirtualSafe<uint32_t*>(addr);
                  if (!safe.success || !safe.pointer) {
                    continue;
                  }
                  uint32_t code = ::xe::load_and_swap<uint32_t>(safe.pointer);
                  uint32_t target = 0;
                  bool link = false;
                  if (!decode_branch_target(addr, code, &target, &link)) {
                    continue;
                  }
                  if (!link) {
                    continue;
                  }
                  if (target == probe.thunk_addr ||
                      (probe.value_addr && target == probe.value_addr)) {
                    ++near_direct_branch_hits;
                    uint32_t distance =
                        addr > pc ? (addr - pc) : (pc - addr);
                    if (distance < nearest_branch_distance) {
                      nearest_branch_distance = distance;
                      nearest_branch_addr = addr;
                      nearest_branch_target = target;
                    }
                  }
                }
              }

              bool pc_hits_probe =
                  pc == probe.thunk_addr ||
                  (probe.value_addr && pc == probe.value_addr);
              bool lr_hits_probe =
                  lr == probe.thunk_addr ||
                  (probe.value_addr && lr == probe.value_addr);
              bool ctr_hits_probe =
                  ctr == probe.thunk_addr ||
                  (probe.value_addr && ctr == probe.value_addr);
              bool current_branch_targets_probe =
                  pc_is_branch &&
                  (pc_branch_target == probe.thunk_addr ||
                   (probe.value_addr && pc_branch_target == probe.value_addr));

              uint32_t entry_bl_hits = 0;
              uint32_t entry_first_bl_addr = 0;
              uint32_t entry_first_bl_target = 0;
              uint32_t near_bctrl_hits = 0;
              uint32_t entry_bctrl_hits = 0;
              uint32_t near_value_ref_hits = 0;
              uint32_t entry_value_ref_hits = 0;

              auto count_value_ref = [&](uint32_t instr,
                                         uint32_t value_addr) -> bool {
                if (!value_addr) {
                  return false;
                }
                uint32_t opcode = instr >> 26;
                uint16_t imm16 = static_cast<uint16_t>(instr & 0xFFFF);
                uint16_t hi16 = static_cast<uint16_t>(value_addr >> 16);
                uint16_t lo16 = static_cast<uint16_t>(value_addr & 0xFFFF);

                // lis rD, imm (alias of addis with rA=0)
                if (opcode == 15 && ((instr >> 16) & 0x1F) == 0 &&
                    imm16 == hi16) {
                  return true;
                }
                // ori / addi / lwz with low 16-bit immediate matching the
                // import value slot address.
                if ((opcode == 24 || opcode == 14 || opcode == 32) &&
                    imm16 == lo16) {
                  return true;
                }
                return false;
              };

              if (pc && probe.thunk_addr) {
                constexpr int kNearWindowInstructions = 96;
                for (int i = -kNearWindowInstructions; i <= kNearWindowInstructions;
                     ++i) {
                  uint32_t addr = pc + static_cast<uint32_t>(i * 4);
                  auto safe = memory_->TranslateVirtualSafe<uint32_t*>(addr);
                  if (!safe.success || !safe.pointer) {
                    continue;
                  }
                  uint32_t code = ::xe::load_and_swap<uint32_t>(safe.pointer);
                  if (code == 0x4E800421) {
                    ++near_bctrl_hits;
                  }
                  if (count_value_ref(code, probe.value_addr)) {
                    ++near_value_ref_hits;
                  }
                }
              }

              if (guest_main_start && probe.thunk_addr) {
                constexpr uint32_t kEntrypointScanInstructions = 4096;
                for (uint32_t i = 0; i < kEntrypointScanInstructions; ++i) {
                  uint32_t addr = guest_main_start + i * sizeof(uint32_t);
                  auto safe = memory_->TranslateVirtualSafe<uint32_t*>(addr);
                  if (!safe.success || !safe.pointer) {
                    continue;
                  }
                  uint32_t code = ::xe::load_and_swap<uint32_t>(safe.pointer);
                  if (code == 0x4E800421) {
                    ++entry_bctrl_hits;
                  }
                  if (count_value_ref(code, probe.value_addr)) {
                    ++entry_value_ref_hits;
                  }
                  uint32_t target = 0;
                  bool link = false;
                  if (!decode_branch_target(addr, code, &target, &link) ||
                      !link) {
                    continue;
                  }
                  if (target == probe.thunk_addr ||
                      (probe.value_addr && target == probe.value_addr)) {
                    ++entry_bl_hits;
                    if (!entry_first_bl_addr) {
                      entry_first_bl_addr = addr;
                      entry_first_bl_target = target;
                    }
                  }
                }
              }

              XELOGW(
                  "RING BUFFER: callsite probe ordinal=0x{:03X} name={} "
                  "value_addr={:08X} thunk_addr={:08X} pc_hit={} lr_hit={} "
                  "ctr_hit={} current_branch_targets_probe={} "
                  "near_direct_bl_hits={} nearest_bl_addr={:08X} "
                  "nearest_bl_target={:08X} nearest_bl_distance_bytes={} "
                  "entry_start={:08X} entry_bl_hits={} entry_first_bl_addr={:08X} "
                  "entry_first_bl_target={:08X} near_bctrl_hits={} "
                  "entry_bctrl_hits={} near_value_ref_hits={} "
                  "entry_value_ref_hits={}",
                  probe.ordinal,
                  kernel::xboxkrnl::GetGpuBootstrapOrdinalName(probe.ordinal),
                  probe.value_addr, probe.thunk_addr,
                  pc_hits_probe ? "YES" : "NO",
                  lr_hits_probe ? "YES" : "NO",
                  ctr_hits_probe ? "YES" : "NO",
                  current_branch_targets_probe ? "YES" : "NO",
                  near_direct_branch_hits, nearest_branch_addr,
                  nearest_branch_target,
                  nearest_branch_distance == UINT32_MAX
                      ? 0
                      : nearest_branch_distance,
                  guest_main_start, entry_bl_hits, entry_first_bl_addr,
                  entry_first_bl_target, near_bctrl_hits, entry_bctrl_hits,
                  near_value_ref_hits, entry_value_ref_hits);
            }
          }
        }
      }
    }

    if (!cvars::gpu_force_init_if_missing) {
      uint32_t fail_after = cvars::gpu_force_init_after_vblank;
      if (fail_after == 0 || vblank_id >= fail_after) {
        static std::atomic<uint64_t> bootstrap_fail_log_count{0};
        uint64_t fail_id = bootstrap_fail_log_count.fetch_add(1) + 1;
        if (fail_id <= 3 || every_n == 0 || (fail_id % every_n) == 0) {
          uint64_t vd_init_engines_count =
              vd_init_engines_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_asic_id_count = vd_get_graphics_asic_id_state_.count.load(
              std::memory_order_relaxed);
          uint64_t vd_init_count =
              vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_cb_count = vd_set_interrupt_callback_state_.count.load(
              std::memory_order_relaxed);
          uint64_t vd_swap_count =
              vd_swap_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_sys_count = vd_get_system_cmd_buffer_state_.count.load(
              std::memory_order_relaxed);
          uint64_t vd_init_edram_count =
              vd_init_edram_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_retrain_edram_count =
              vd_retrain_edram_state_.count.load(std::memory_order_relaxed);
          uint64_t vd_retrain_worker_count =
              vd_retrain_edram_worker_state_.count.load(
                  std::memory_order_relaxed);
          uint64_t vd_hsio_count = vd_hsio_training_succeeded_state_.count.load(
              std::memory_order_relaxed);
          uint64_t mmio_rb_base_writes =
              mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
          uint64_t mmio_rb_cntl_writes =
              mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
          uint64_t mmio_rb_wptr_writes =
              mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
          bool pre_ring_video_calls_seen =
              vd_init_engines_count > 0 || vd_asic_id_count > 0 ||
              vd_init_edram_count > 0 || vd_retrain_edram_count > 0 ||
              vd_retrain_worker_count > 0 || vd_hsio_count > 0;
          const char* bootstrap_stage = "PARTIAL_BOOTSTRAP_NO_ACK";
          uint32_t bootstrap_stage_id = 0;
          if (vd_init_count == 0 && vd_cb_count == 0 && vd_swap_count == 0 &&
              vd_sys_count == 0 && !pre_ring_video_calls_seen &&
              mmio_rb_base_writes == 0 && mmio_rb_cntl_writes == 0 &&
              mmio_rb_wptr_writes == 0) {
            bootstrap_stage = "BOOTSTRAP_NEVER_ENTERED";
            bootstrap_stage_id = 1;
          } else if (vd_init_count == 0 && vd_cb_count == 0 && vd_swap_count == 0 &&
                     vd_sys_count == 0 && pre_ring_video_calls_seen) {
            bootstrap_stage = "PRE_RING_VIDEO_BOOTSTRAP_STALLED";
            bootstrap_stage_id = 2;
          } else if (vd_init_count > 0 &&
                     !bootstrap_state.ring_buffer_initialized) {
            bootstrap_stage = "VD_INIT_CALLED_RING_NOT_READY";
            bootstrap_stage_id = 3;
          } else if (bootstrap_state.ring_buffer_initialized &&
                     !bootstrap_state.initialization_acknowledged) {
            bootstrap_stage = "RING_INIT_WITHOUT_ACK";
            bootstrap_stage_id = 4;
          } else if (mmio_rb_base_writes == 0 && mmio_rb_cntl_writes == 0) {
            bootstrap_stage = "NO_CP_MMIO_RING_SETUP";
            bootstrap_stage_id = 5;
          }
          static std::atomic<uint32_t> last_bootstrap_stage_id{0xFFFFFFFFu};
          static std::atomic<uint64_t> bootstrap_stage_transition_count{0};
          uint32_t previous_stage_id =
              last_bootstrap_stage_id.exchange(bootstrap_stage_id,
                                               std::memory_order_relaxed);
          if (previous_stage_id != bootstrap_stage_id) {
            uint64_t transition_id =
                bootstrap_stage_transition_count.fetch_add(
                    1, std::memory_order_relaxed) +
                1;
            XELOGW(
                "RING BUFFER: bootstrap stage transition #{} previous={} "
                "current={} vblank_id={} since_first_vblank={}ms",
                transition_id, GetBootstrapStageNameFromId(previous_stage_id),
                bootstrap_stage, vblank_id,
                since_first_vblank);
          }
          BootstrapGateSnapshot gate_snapshot =
              ReadBootstrapGateSnapshot(kernel_state_);
          const char* gate_probe_ppc_op = nullptr;
          bool gate_probe_reliable = true;
          if (gate_snapshot.committed && gate_snapshot.translated) {
            gate_probe_ppc_op = ClassifyPpcInstructionWord(gate_snapshot.value);
            if (gate_probe_ppc_op) {
              gate_probe_reliable = false;
            }
          }
          XELOGE(
              "RING BUFFER: bootstrap failed: ring init handshake never "
              "completed (vblank_id={}, since_first_vblank={}ms, "
              "gpu_force_init_if_missing=OFF)",
              vblank_id, since_first_vblank);
          XELOGE(
              "RING BUFFER: bootstrap failed details: vd_init_engines={} "
              "vd_asic_id={} vd_init={} vd_cb={} vd_swap={} vd_sys={} "
              "vd_init_edram={} vd_retrain_edram={} vd_retrain_worker={} "
              "vd_hsio={} mmio_rb_base={} mmio_rb_cntl={} mmio_rb_wptr={} "
              "rb_base={:08X} rb_size={:08X} read_ptr={:08X} write_ptr={:08X}",
              vd_init_engines_count, vd_asic_id_count, vd_init_count, vd_cb_count,
              vd_swap_count, vd_sys_count, vd_init_edram_count,
              vd_retrain_edram_count, vd_retrain_worker_count, vd_hsio_count,
              mmio_rb_base_writes, mmio_rb_cntl_writes, mmio_rb_wptr_writes,
              bootstrap_state.primary_buffer_ptr,
              bootstrap_state.primary_buffer_size, read_ptr, write_ptr);
          uint64_t mmio_any_writes =
              mmio_any_write_count_.load(std::memory_order_relaxed);
          uint64_t mmio_non_cp_writes =
              mmio_non_cp_write_count_.load(std::memory_order_relaxed);
          uint32_t mmio_first_reg =
              mmio_any_first_reg_.load(std::memory_order_relaxed);
          uint32_t mmio_last_reg =
              mmio_any_last_reg_.load(std::memory_order_relaxed);
          uint32_t mmio_last_value =
              mmio_any_last_value_.load(std::memory_order_relaxed);
          uint32_t mmio_last_pc =
              mmio_any_last_pc_.load(std::memory_order_relaxed);
          uint32_t mmio_last_thread_id =
              mmio_any_last_thread_id_.load(std::memory_order_relaxed);
          uint64_t mmio_endian_fix_count =
              mmio_cp_endian_autofix_count_.load(std::memory_order_relaxed);
          uint32_t mmio_endian_fix_reg =
              mmio_cp_endian_last_reg_.load(std::memory_order_relaxed);
          uint32_t mmio_endian_fix_raw =
              mmio_cp_endian_last_raw_value_.load(std::memory_order_relaxed);
          uint32_t mmio_endian_fix_normalized =
              mmio_cp_endian_last_normalized_value_.load(
                  std::memory_order_relaxed);
          XELOGE(
              "RING BUFFER: bootstrap MMIO summary any_writes={} "
              "non_cp_writes={} first_reg_state={} first_reg={:04X} "
              "last_reg={:04X} "
              "last_value={:08X} last_pc={:08X} last_thread_id={} "
              "endian_autofixes={} endian_last_reg={:04X} "
              "endian_last_raw={:08X} endian_last_normalized={:08X}",
              mmio_any_writes, mmio_non_cp_writes,
              mmio_first_reg == 0xFFFFFFFFu ? "NONE" : "SEEN", mmio_first_reg,
              mmio_last_reg,
              mmio_last_value, mmio_last_pc, mmio_last_thread_id,
              mmio_endian_fix_count, mmio_endian_fix_reg, mmio_endian_fix_raw,
              mmio_endian_fix_normalized);
          if (mmio_rb_base_writes == 0 && mmio_rb_cntl_writes == 0 &&
              mmio_rb_wptr_writes == 0) {
            XELOGE(
                "RING BUFFER: bootstrap diagnosis signal: guest has not "
                "written CP_RB_BASE/CP_RB_CNTL/CP_RB_WPTR in this run");
            if (kernel_state_ && kernel_state_->memory()) {
              auto* probe_memory = kernel_state_->memory();
              auto log_mmio_mapping_probe = [&](const char* reg_name,
                                                uint32_t addr_value) {
                bool mapped =
                    probe_memory->LookupVirtualMappedRange(addr_value) != nullptr;
                void* host_ptr = probe_memory->TranslateVirtual(addr_value);
                bool translated = host_ptr != nullptr;
                xe::memory::PageAccess access = xe::memory::PageAccess::kNoAccess;
                bool access_known = false;
                if (translated) {
                  size_t page_len = xe::memory::page_size();
                  xe::memory::QueryProtect(host_ptr, page_len, access);
                  access_known = true;
                }
                auto* heap = probe_memory->LookupHeap(addr_value);
                int heap_type = -1;
                bool committed = false;
                uint32_t protect = 0;
                if (heap) {
                  heap_type = static_cast<int>(heap->heap_type());
                  xe::HeapAllocationInfo info{};
                  if (heap->QueryRegionInfo(addr_value, &info)) {
                    committed = (info.state & xe::kMemoryAllocationCommit) != 0;
                    protect = info.protect;
                  }
                }
                if (!heap) {
                  XELOGE(
                      "RING BUFFER: bootstrap MMIO mapping probe reg={} "
                      "addr={:08X} mapped_range={} translated={} heap_backed=NO",
                      reg_name ? reg_name : "<unknown>", addr_value,
                      mapped ? "YES" : "NO", translated ? "YES" : "NO");
                } else {
                  XELOGE(
                      "RING BUFFER: bootstrap MMIO mapping probe reg={} "
                      "addr={:08X} mapped_range={} translated={} heap_type={} "
                      "committed={} protect=0x{:X}",
                      reg_name ? reg_name : "<unknown>", addr_value,
                      mapped ? "YES" : "NO", translated ? "YES" : "NO",
                      heap_type, committed ? "YES" : "NO", protect);
                }
                if (translated && access_known) {
                  XELOGE(
                      "RING BUFFER: bootstrap MMIO mapping probe reg={} "
                      "addr={:08X} host={} page_access={}",
                      reg_name ? reg_name : "<unknown>", addr_value, host_ptr,
                      access == xe::memory::PageAccess::kNoAccess
                          ? "NO_ACCESS"
                          : (access == xe::memory::PageAccess::kReadOnly
                                 ? "READ_ONLY"
                                 : (access == xe::memory::PageAccess::kReadWrite
                                        ? "READ_WRITE"
                                        : "OTHER")));
                }
              };
              log_mmio_mapping_probe("CP_RB_BASE", 0x7FC80700u);
              log_mmio_mapping_probe("CP_RB_CNTL", 0x7FC80704u);
              log_mmio_mapping_probe("CP_RB_WPTR", 0x7FC80714u);
            }
          }
            XELOGW(
              "RING BUFFER: bootstrap diagnosis stage={} callback_set={} "
              "ring_init={} init_ack={} gate_addr={:08X} gate_value=0x{:08X} "
              "gate_committed={} gate_translated={} gate_probe_reliable={}",
              bootstrap_stage, interrupt_callback_ != 0 ? "YES" : "NO",
              bootstrap_state.ring_buffer_initialized ? "YES" : "NO",
              bootstrap_state.initialization_acknowledged ? "YES" : "NO",
              gate_snapshot.address, gate_snapshot.value,
              gate_snapshot.committed ? "YES" : "NO",
              gate_snapshot.translated ? "YES" : "NO",
              gate_probe_reliable ? "YES" : "NO");
          if (gate_probe_ppc_op) {
            XELOGW(
                "RING BUFFER: bootstrap gate probe note: addr={:08X} "
                "contains PPC instruction word '{}' (0x{:08X}); this probe "
                "address may be executable code rather than a data gate; "
                "treat this gate probe as diagnostic-only",
                gate_snapshot.address, gate_probe_ppc_op, gate_snapshot.value);
          }

          if (std::string_view(bootstrap_stage) == "BOOTSTRAP_NEVER_ENTERED") {
            static std::atomic<uint64_t> debug_monitor_chain_probe_count{0};
            uint64_t probe_id =
                debug_monitor_chain_probe_count.fetch_add(1, std::memory_order_relaxed) +
                1;
            if (probe_id == 1 || every_n == 0 || (probe_id % every_n) == 0) {
              auto* probe_memory = kernel_state_ ? kernel_state_->memory() : nullptr;
              uint32_t slot_value = 0;
              bool slot_committed = false;
              bool slot_translated = false;
              ReadGuestWordSnapshot(probe_memory, 0x820005E0, &slot_value,
                                    &slot_committed, &slot_translated);
                XELOGW(
                  "RING BUFFER: bootstrap pointer-chain probe addr={:08X} "
                  "committed={} translated={} value={:08X}",
                  0x820005E0u, slot_committed ? "YES" : "NO",
                  slot_translated ? "YES" : "NO", slot_value);
              if (cvars::gpu_autofix_debug_monitor_import_slot &&
                  slot_translated && slot_value == 0) {
                auto* resolver = processor_ ? processor_->export_resolver()
                                            : nullptr;
                auto* export_entry =
                    resolver
                        ? resolver->GetExportByOrdinal(
                              "xboxkrnl.exe", 0x0059u)
                        : nullptr;
                uint32_t mapped_ptr = export_entry ? export_entry->variable_ptr
                                                   : 0u;
                if (mapped_ptr != 0 && probe_memory) {
                  auto slot_safe =
                      probe_memory->TranslateVirtualSafe<uint32_t*>(0x820005E0);
                  if (slot_safe.success && slot_safe.pointer) {
                    xe::store_and_swap<uint32_t>(slot_safe.pointer, mapped_ptr);
                    slot_value = mapped_ptr;
                    XELOGW(
                        "RING BUFFER: bootstrap auto-fix repaired "
                        "KeDebugMonitorData import slot addr=820005E0 "
                        "value={:08X}",
                        mapped_ptr);
                  }
                } else {
                  XELOGW(
                      "RING BUFFER: bootstrap auto-fix skipped "
                      "KeDebugMonitorData slot repair (mapped_ptr={:08X})",
                      mapped_ptr);
                }
              }
              if (slot_translated && slot_value != 0) {
                uint32_t p0_value = 0;
                bool p0_committed = false;
                bool p0_translated = false;
                ReadGuestWordSnapshot(probe_memory, slot_value, &p0_value,
                                      &p0_committed, &p0_translated);
                XELOGW(
                    "RING BUFFER: bootstrap pointer-chain probe addr={:08X} "
                    "committed={} translated={} value={:08X}",
                    slot_value, p0_committed ? "YES" : "NO",
                    p0_translated ? "YES" : "NO", p0_value);
                if (p0_translated && p0_value != 0) {
                  uint32_t callback_slot_addr = p0_value + 0x18;
                  uint32_t callback_value = 0;
                  bool callback_committed = false;
                  bool callback_translated = false;
                  ReadGuestWordSnapshot(probe_memory, callback_slot_addr,
                                        &callback_value, &callback_committed,
                                        &callback_translated);
                    XELOGW(
                      "RING BUFFER: bootstrap pointer-chain probe "
                      "callback_addr={:08X} committed={} translated={} "
                      "value={:08X}",
                      callback_slot_addr, callback_committed ? "YES" : "NO",
                      callback_translated ? "YES" : "NO", callback_value);
                }
              }

              uint32_t xex_slot_value = 0;
              bool xex_slot_committed = false;
              bool xex_slot_translated = false;
              ReadGuestWordSnapshot(probe_memory, 0x8200083C, &xex_slot_value,
                                    &xex_slot_committed,
                                    &xex_slot_translated);
                XELOGW(
                  "RING BUFFER: executable-module probe addr={:08X} "
                  "committed={} translated={} value={:08X}",
                  0x8200083Cu, xex_slot_committed ? "YES" : "NO",
                  xex_slot_translated ? "YES" : "NO", xex_slot_value);

              if (xex_slot_translated && xex_slot_value == 0) {
                auto* resolver = processor_ ? processor_->export_resolver()
                                            : nullptr;
                auto* export_entry =
                    resolver
                        ? resolver->GetExportByOrdinal(
                              "xboxkrnl.exe", 0x00C1u)
                        : nullptr;
                uint32_t mapped_ptr = export_entry ? export_entry->variable_ptr
                                                   : 0u;
                if (mapped_ptr != 0 && probe_memory) {
                  auto xex_slot_safe =
                      probe_memory->TranslateVirtualSafe<uint32_t*>(0x8200083C);
                  if (xex_slot_safe.success && xex_slot_safe.pointer) {
                    xe::store_and_swap<uint32_t>(xex_slot_safe.pointer,
                                                 mapped_ptr);
                    xex_slot_value = mapped_ptr;
                    XELOGW(
                        "RING BUFFER: bootstrap auto-fix repaired "
                        "XexExecutableModuleHandle import slot "
                        "addr=8200083C value={:08X}",
                        mapped_ptr);
                  }
                }
              }

              if (xex_slot_translated && xex_slot_value != 0) {
                uint32_t module_handle_ptr = 0;
                bool module_handle_committed = false;
                bool module_handle_translated = false;
                ReadGuestWordSnapshot(probe_memory, xex_slot_value,
                                      &module_handle_ptr,
                                      &module_handle_committed,
                                      &module_handle_translated);
                XELOGW(
                    "RING BUFFER: executable-module probe handle_slot={:08X} "
                    "committed={} translated={} value={:08X}",
                    xex_slot_value, module_handle_committed ? "YES" : "NO",
                    module_handle_translated ? "YES" : "NO",
                    module_handle_ptr);
              }
            }
          }
          uint32_t tracked_missing_static_imports = 0;
          uint32_t tracked_total_static_ordinals = 0;
          bool vd_set_interrupt_callback_static_imported = false;
          uint32_t vd_set_interrupt_callback_value_address = 0;
          uint32_t vd_set_interrupt_callback_thunk_address = 0;
          std::array<bool, 0x300> tracked_imported_ordinals{};
          bool tracked_import_map_available = false;
          kernel::object_ref<kernel::UserModule> executable_module;
          if (kernel_state_) {
            executable_module = kernel_state_->GetExecutableModule();
          }
          if (!executable_module) {
            XELOGE("RING BUFFER: bootstrap import summary unavailable "
                   "(no executable module)");
          } else if (!executable_module->xex_module()) {
            XELOGE("RING BUFFER: bootstrap import summary unavailable "
                   "(executable is not XEX)");
          } else {
            const auto* import_libs =
                executable_module->xex_module()->import_libraries();
            static constexpr std::array<uint16_t, 13> kGpuBootstrapOrdinals = {
                0x01B6, 0x01BC, 0x01BD, 0x01BE, 0x01BF, 0x01C2, 0x01C3,
                0x01C6, 0x01D5, 0x025B, 0x0268, 0x0269, 0x026A};
            uint32_t gpu_import_entries = 0;
            uint32_t gpu_import_thunks = 0;
            uint32_t gpu_import_values = 0;
            if (import_libs) {
              for (const auto& lib : *import_libs) {
                if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
                  continue;
                }
                for (const auto& import_fn : lib.imports) {
                  if (!IsGpuBootstrapOrdinal(import_fn.ordinal)) {
                    continue;
                  }
                  ++gpu_import_entries;
                  if (import_fn.value_address) {
                    ++gpu_import_values;
                  }
                  if (import_fn.thunk_address) {
                    ++gpu_import_thunks;
                  }
                  if (import_fn.ordinal < tracked_imported_ordinals.size()) {
                    tracked_imported_ordinals[import_fn.ordinal] = true;
                  }
                  if (import_fn.ordinal == 0x01D5) {
                    vd_set_interrupt_callback_static_imported = true;
                    if (import_fn.value_address) {
                      vd_set_interrupt_callback_value_address =
                          import_fn.value_address;
                    }
                    if (import_fn.thunk_address) {
                      vd_set_interrupt_callback_thunk_address =
                          import_fn.thunk_address;
                    }
                  }
                }
              }
            }
            tracked_import_map_available = true;
            uint32_t required_imported = 0;
            uint32_t required_missing = 0;
            uint32_t required_exports_implemented = 0;
            auto* resolver = processor_ ? processor_->export_resolver() : nullptr;
            for (uint16_t ordinal : kGpuBootstrapOrdinals) {
              bool imported = ordinal < tracked_imported_ordinals.size()
                                  ? tracked_imported_ordinals[ordinal]
                                  : false;
              if (imported) {
                ++required_imported;
              } else {
                ++required_missing;
              }
              bool export_implemented = false;
              if (resolver) {
                auto* export_entry =
                    resolver->GetExportByOrdinal("xboxkrnl.exe", ordinal);
                export_implemented =
                    export_entry && export_entry->is_implemented();
              }
              if (export_implemented) {
                ++required_exports_implemented;
              }
              if (!imported || !export_implemented) {
                XELOGE(
                    "RING BUFFER: bootstrap import check ordinal=0x{:03X} "
                    "name={} static_imported={} export_implemented={}",
                    ordinal, GetGpuBootstrapOrdinalName(ordinal),
                    imported ? "YES" : "NO",
                    export_implemented ? "YES" : "NO");
              }
            }
            XELOGE(
                "RING BUFFER: bootstrap import summary "
                "static_gpu_import_entries={} static_gpu_import_values={} "
                "static_gpu_import_thunks={} tracked_ordinals_imported={} "
                "tracked_ordinals_not_imported={} "
                "tracked_exports_implemented={} tracked_ordinals_total={}",
                gpu_import_entries, gpu_import_values, gpu_import_thunks,
                required_imported, required_missing, required_exports_implemented,
                kGpuBootstrapOrdinals.size());
            if (vd_set_interrupt_callback_static_imported) {
              XELOGE(
                  "RING BUFFER: callback path static import ordinal=0x1D5 "
                  "name=VdSetGraphicsInterruptCallback value_addr={:08X} "
                  "thunk_addr={:08X}",
                  vd_set_interrupt_callback_value_address,
                  vd_set_interrupt_callback_thunk_address);

              auto log_import_addr_word = [&](const char* label,
                                             uint32_t address) {
                if (!label || !address) {
                  return;
                }
                auto safe = memory_->TranslateVirtualSafe<uint32_t*>(address);
                if (!safe.success || !safe.pointer) {
                  XELOGE(
                      "RING BUFFER: callback path {} addr={:08X} "
                      "translated=NO error={}",
                      label, address, safe.error);
                  return;
                }
                uint32_t word = ::xe::load_and_swap<uint32_t>(safe.pointer);
                XELOGE(
                    "RING BUFFER: callback path {} addr={:08X} "
                    "translated=YES value={:08X}",
                    label, address, word);
              };

              auto log_function_binding = [&](const char* label,
                                              uint32_t guest_address) {
                if (!label || !guest_address || !processor_) {
                  return;
                }
                auto* module = processor_->LookupModule(guest_address);
                auto* function = processor_->LookupFunction(guest_address);
                const char* module_name = module ? module->name().c_str()
                                                 : "<unknown>";
                const char* function_name =
                    function ? function->name().c_str() : "<unknown>";
                const char* behavior = "Unknown";
                if (function) {
                  switch (function->behavior()) {
                    case cpu::Function::Behavior::kDefault:
                      behavior = "Default";
                      break;
                    case cpu::Function::Behavior::kProlog:
                      behavior = "Prolog";
                      break;
                    case cpu::Function::Behavior::kEpilog:
                      behavior = "Epilog";
                      break;
                    case cpu::Function::Behavior::kEpilogReturn:
                      behavior = "EpilogReturn";
                      break;
                    case cpu::Function::Behavior::kBuiltin:
                      behavior = "Builtin";
                      break;
                    case cpu::Function::Behavior::kExtern:
                      behavior = "Extern";
                      break;
                  }
                }
                XELOGE(
                    "RING BUFFER: callback path {} target={:08X} module={} "
                    "function={} function_addr={:08X} behavior={} "
                    "status={}",
                    label, guest_address, module_name, function_name,
                    function ? function->address() : 0, behavior,
                    function ? static_cast<int>(function->status()) : -1);
              };

              if (vd_set_interrupt_callback_value_address) {
                log_import_addr_word("value_slot",
                                    vd_set_interrupt_callback_value_address);
                auto value_safe = memory_->TranslateVirtualSafe<uint32_t*>(
                    vd_set_interrupt_callback_value_address);
                if (value_safe.success && value_safe.pointer) {
                  uint32_t value_target =
                      ::xe::load_and_swap<uint32_t>(value_safe.pointer);
                  log_function_binding("value_slot_target", value_target);
                }
              }

              if (vd_set_interrupt_callback_thunk_address) {
                log_import_addr_word("thunk_word_0",
                                    vd_set_interrupt_callback_thunk_address);
                log_function_binding("thunk_target",
                                     vd_set_interrupt_callback_thunk_address);
                for (uint32_t i = 0; i < 4; ++i) {
                  uint32_t addr = vd_set_interrupt_callback_thunk_address +
                                  i * sizeof(uint32_t);
                  auto safe = memory_->TranslateVirtualSafe<uint32_t*>(addr);
                  if (!safe.success || !safe.pointer) {
                    XELOGE(
                        "RING BUFFER: callback path thunk disasm addr={:08X} "
                        "translated=NO error={}",
                        addr, safe.error);
                    continue;
                  }
                  uint32_t code = ::xe::load_and_swap<uint32_t>(safe.pointer);
                  xe::StringBuffer sb;
                  xe::cpu::ppc::DisasmPPC(addr, code, &sb);
                  XELOGE(
                      "RING BUFFER: callback path thunk disasm {:08X}: "
                      "{:08X} {}",
                      addr, code, sb.to_string());
                }
              }
            } else {
              XELOGE(
                  "RING BUFFER: callback path static import missing "
                  "ordinal=0x1D5 name=VdSetGraphicsInterruptCallback");
            }
            tracked_missing_static_imports = required_missing;
            tracked_total_static_ordinals =
                static_cast<uint32_t>(kGpuBootstrapOrdinals.size());
          }
          auto get_vd_call_count_for_ordinal = [&](uint16_t ordinal) -> uint64_t {
            switch (ordinal) {
              case 0x01B6: {
                return vd_enable_rptr_writeback_state_.count.load(
                    std::memory_order_relaxed);
              }
              case 0x01BC: {
                uint64_t vd_asic_id_count = vd_get_graphics_asic_id_state_.count.load(
                    std::memory_order_relaxed);
                return vd_asic_id_count;
              }
              case 0x01BD: {
                uint64_t vd_sys_count = vd_get_system_cmd_buffer_state_.count.load(
                    std::memory_order_relaxed);
                return vd_sys_count;
              }
              case 0x01C2: {
                uint64_t vd_init_engines_count = vd_init_engines_state_.count.load(
                    std::memory_order_relaxed);
                return vd_init_engines_count;
              }
              case 0x01C3: {
                uint64_t vd_init_count = vd_init_ring_buffer_state_.count.load(
                    std::memory_order_relaxed);
                return vd_init_count;
              }
              case 0x01C6: {
                uint64_t vd_hsio_count = vd_hsio_training_succeeded_state_.count.load(
                    std::memory_order_relaxed);
                return vd_hsio_count;
              }
              case 0x01D5: {
                uint64_t vd_cb_count = vd_set_interrupt_callback_state_.count.load(
                    std::memory_order_relaxed);
                return vd_cb_count;
              }
              case 0x025B: {
                uint64_t vd_swap_count = vd_swap_state_.count.load(
                    std::memory_order_relaxed);
                return vd_swap_count;
              }
              case 0x0268: {
                uint64_t vd_init_edram_count = vd_init_edram_state_.count.load(
                    std::memory_order_relaxed);
                return vd_init_edram_count;
              }
              case 0x0269: {
                uint64_t vd_retrain_edram_count = vd_retrain_edram_state_.count.load(
                    std::memory_order_relaxed);
                return vd_retrain_edram_count;
              }
              case 0x026A: {
                uint64_t vd_retrain_worker_count = vd_retrain_edram_worker_state_.count.load(
                    std::memory_order_relaxed);
                return vd_retrain_worker_count;
              }
              default: {
                return 0;
              }
            }
          };
          auto proc_stats =
              kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
          uint64_t dynamic_lookup_total = 0;
          uint64_t static_thunk_total = 0;
          uint64_t export_entry_total = 0;
          uint32_t imported_with_runtime_activity = 0;
          uint32_t imported_without_runtime_activity = 0;
          for (const auto& ordinal_stat : proc_stats.ordinal_runtime_stats) {
            dynamic_lookup_total += ordinal_stat.dynamic_lookup_calls;
            static_thunk_total += ordinal_stat.static_thunk_calls;
            export_entry_total += ordinal_stat.export_entry_calls;
            bool static_imported =
                tracked_import_map_available &&
                ordinal_stat.ordinal < tracked_imported_ordinals.size() &&
                tracked_imported_ordinals[ordinal_stat.ordinal];
            if (static_imported) {
              uint64_t vd_call_count =
                  get_vd_call_count_for_ordinal(ordinal_stat.ordinal);
              bool runtime_activity =
                  (ordinal_stat.dynamic_lookup_calls +
                   ordinal_stat.static_thunk_calls +
                   ordinal_stat.export_entry_calls + vd_call_count) > 0;
              if (runtime_activity) {
                ++imported_with_runtime_activity;
              } else {
                ++imported_without_runtime_activity;
              }
            }
          }
          XELOGE(
              "RING BUFFER: bootstrap import runtime stats "
              "module_handle_calls={} module_handle_failures={} "
              "proc_lookup_calls={} proc_lookup_resolved={} "
              "proc_lookup_missing={} proc_lookup_invalid_module={} "
              "proc_lookup_fallback_hits={} ordinal_01BC_calls={} "
              "ordinal_01BF_calls={} ordinal_0268_calls={} "
              "dynamic_lookup_total={} static_thunk_total={} "
              "export_entry_total={}",
              proc_stats.module_handle_calls, proc_stats.module_handle_failures,
              proc_stats.proc_lookup_calls, proc_stats.proc_lookup_resolved,
              proc_stats.proc_lookup_missing,
              proc_stats.proc_lookup_invalid_module,
              proc_stats.proc_lookup_fallback_hits,
              proc_stats.ordinal_01BC_calls, proc_stats.ordinal_01BF_calls,
              proc_stats.ordinal_0268_calls, dynamic_lookup_total,
              static_thunk_total, export_entry_total);
          for (const auto& ordinal_stat : proc_stats.ordinal_runtime_stats) {
            bool static_imported =
                tracked_import_map_available &&
                ordinal_stat.ordinal < tracked_imported_ordinals.size() &&
                tracked_imported_ordinals[ordinal_stat.ordinal];
            bool is_variable_export =
                ordinal_stat.ordinal == 0x01BE || ordinal_stat.ordinal == 0x01BF;
            uint64_t vd_call_count =
                get_vd_call_count_for_ordinal(ordinal_stat.ordinal);
            bool runtime_activity =
                (ordinal_stat.dynamic_lookup_calls +
                 ordinal_stat.static_thunk_calls +
                 ordinal_stat.export_entry_calls + vd_call_count) > 0;
            XELOGE(
                "RING BUFFER: bootstrap ordinal runtime ordinal=0x{:03X} "
                "name={} static_imported={} dynamic_lookup_calls={} "
                "static_thunk_calls={} export_entry_calls={} vd_call_count={} "
                "variable_export={} runtime_activity={}",
                ordinal_stat.ordinal,
                GetGpuBootstrapOrdinalName(ordinal_stat.ordinal),
                tracked_import_map_available
                    ? (static_imported ? "YES" : "NO")
                    : "UNKNOWN",
                ordinal_stat.dynamic_lookup_calls,
                ordinal_stat.static_thunk_calls, ordinal_stat.export_entry_calls,
                vd_call_count, is_variable_export ? "YES" : "NO",
                runtime_activity ? "YES" : "NO");
          }
          if (tracked_import_map_available) {
            XELOGE(
                "RING BUFFER: bootstrap import activity summary "
                "static_imported_with_runtime_activity={} "
                "static_imported_without_runtime_activity={}",
                imported_with_runtime_activity, imported_without_runtime_activity);
          }
          if (proc_stats.proc_lookup_calls == 0) {
            if (proc_stats.loader_proc_lookup_calls_total != 0) {
              XELOGE(
                  "RING BUFFER: bootstrap import runtime diagnosis: guest "
                  "called XexGetProcedureAddress {} time(s), but none of "
                  "those lookups targeted tracked GPU bootstrap exports",
                  proc_stats.loader_proc_lookup_calls_total);
            } else {
              XELOGE(
                  "RING BUFFER: bootstrap import runtime diagnosis: no "
                  "dynamic XexGetProcedureAddress lookups for GPU bootstrap "
                  "ordinals have occurred in this run");
            }
            if (static_thunk_total == 0 && export_entry_total == 0) {
              XELOGE(
                  "RING BUFFER: bootstrap import runtime diagnosis: no static "
                  "import thunk dispatches or Vd export entry calls observed "
                  "for tracked GPU ordinals");
            }
            else if (static_thunk_total != 0 && export_entry_total != 0) {
              XELOGE(
                  "RING BUFFER: bootstrap import runtime diagnosis: static "
                  "import thunks and Vd export entries were used to call GPU "
                  "bootstrap ordinals");
            }
            if (tracked_missing_static_imports > 0) {
              XELOGE(
                  "RING BUFFER: bootstrap import runtime diagnosis: "
                  "tracked_ordinals_not_imported={} of {}; missing static "
                  "imports alone do not block bootstrap unless code reaches "
                  "dynamic lookup/call sites",
                  tracked_missing_static_imports,
                  tracked_total_static_ordinals);
            }
          }
          else if (proc_stats.proc_lookup_resolved != 0) {
            XELOGE(
                "RING BUFFER: bootstrap import runtime diagnosis: dynamic "
                "XexGetProcedureAddress lookups for GPU bootstrap ordinals "
                "have occurred in this run");
          }

          static std::atomic<uint64_t> last_bootstrap_thread_dump_ms{0};
          uint64_t last_dump =
              last_bootstrap_thread_dump_ms.load(std::memory_order_relaxed);
          uint64_t dump_interval_ms =
              static_cast<uint64_t>(cvars::gpu_log_no_swap_threads_interval_ms);
          if (last_dump == 0 || dump_interval_ms == 0 ||
              now_ms - last_dump >= dump_interval_ms) {
            last_bootstrap_thread_dump_ms.store(now_ms,
                                                std::memory_order_relaxed);
            XELOGW("RING BUFFER: bootstrap failure thread snapshot requested");
            DumpGuestThreadStates("RING_BUFFER_BOOTSTRAP_FAIL");
          }
        }
      }
    }

    // Keep guest-driven callback dispatch behavior intact if the game has
    // installed the callback before ring init completes.
    if (interrupt_callback_ != 0) {
      DispatchInterruptCallback(0, 2);
    } else if (cvars::trace_gpu_bootstrap) {
      XELOGI(
          "DEBUG: GPU vblank dispatch skipped by bootstrap gate: callback not set "
          "(waiting for ring init ack)");
    }
    return;
  }
  if (guest_main_thread_ready &&
      cvars::gpu_suspend_nonessential_until_ring_init_ack &&
      !bootstrap_ready_for_gpu_progress && !bootstrap_activity_observed) {
    static std::atomic<uint64_t> prebootstrap_gate_log_count{0};
    uint64_t prebootstrap_log_id = prebootstrap_gate_log_count.fetch_add(1) + 1;
    uint32_t every_n =
        cvars::gpu_suspend_nonessential_bootstrap_log_every_n_vblank;
    if (prebootstrap_log_id <= 5 || every_n == 0 ||
        (prebootstrap_log_id % every_n) == 0) {
      uint64_t vd_init_count =
        vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_cb_count = vd_set_interrupt_callback_state_.count.load(
        std::memory_order_relaxed);
      uint64_t vd_swap_count =
        vd_swap_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_sys_count = vd_get_system_cmd_buffer_state_.count.load(
        std::memory_order_relaxed);
      uint64_t mmio_rb_base_writes =
        mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
      uint64_t mmio_rb_cntl_writes =
        mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
      uint64_t mmio_rb_wptr_writes =
        mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
      XELOGW(
          "RING BUFFER: bootstrap gate deferred (pre-bootstrap): no guest "
          "GPU bootstrap activity observed yet "
          "(vblank_id={}, since_first_vblank={}ms, ring_init={}, init_ack={}, "
        "callback_set={}, vd_init={}, vd_cb={}, vd_swap={}, vd_sys={}, "
        "mmio_rb_base={}, mmio_rb_cntl={}, mmio_rb_wptr={})",
          vblank_id, since_first_vblank,
          bootstrap_state.ring_buffer_initialized ? "YES" : "NO",
          bootstrap_state.initialization_acknowledged ? "YES" : "NO",
        interrupt_callback_ != 0 ? "YES" : "NO", vd_init_count,
        vd_cb_count, vd_swap_count, vd_sys_count, mmio_rb_base_writes,
        mmio_rb_cntl_writes, mmio_rb_wptr_writes);

      auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
      XexCheckExecutablePrivilegeRuntimeState xexcheck_runtime_state =
          ClassifyXexCheckExecutablePrivilegeRuntimeState(proc_stats);
      auto find_ordinal_stat = [&](uint16_t ordinal)
          -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
        for (const auto& stat : proc_stats.ordinal_runtime_stats) {
          if (stat.ordinal == ordinal) {
            return &stat;
          }
        }
        return nullptr;
      };
      const auto* stat_01C2 = find_ordinal_stat(0x01C2);
      const auto* stat_01C3 = find_ordinal_stat(0x01C3);
      const auto* stat_01D5 = find_ordinal_stat(0x01D5);
        const auto* stat_025B = find_ordinal_stat(0x025B);
      bool thunk_counter_observation_enabled =
          cvars::trace_gpu_bootstrap || cvars::log_gpu_import_thunks;
      uint64_t mmio_any_writes = mmio_any_write_count_.load(std::memory_order_relaxed);
      uint64_t mmio_non_cp_writes =
          mmio_non_cp_write_count_.load(std::memory_order_relaxed);
      uint32_t mmio_first_reg = mmio_any_first_reg_.load(std::memory_order_relaxed);
      uint32_t mmio_last_reg = mmio_any_last_reg_.load(std::memory_order_relaxed);
      uint32_t mmio_last_value =
          mmio_any_last_value_.load(std::memory_order_relaxed);
      uint32_t mmio_last_pc = mmio_any_last_pc_.load(std::memory_order_relaxed);
      uint32_t mmio_last_thread_id =
          mmio_any_last_thread_id_.load(std::memory_order_relaxed);
      uint32_t prebootstrap_wait_watch_object =
          kernel::xboxkrnl::GetWaitWatchObjectPtr();
      XELOGW(
          "RING BUFFER: pre-bootstrap runtime stats module_handle_calls={} "
          "module_handle_failures={} proc_lookup_calls={} proc_lookup_resolved={} "
          "proc_lookup_missing={} proc_lookup_invalid_module={} "
          "proc_lookup_fallback_hits={} wait_watch_obj={:08X} "
          "thunk_counter_observation={} (trace_gpu_bootstrap={}, "
          "log_gpu_import_thunks={})",
          proc_stats.module_handle_calls, proc_stats.module_handle_failures,
          proc_stats.proc_lookup_calls, proc_stats.proc_lookup_resolved,
          proc_stats.proc_lookup_missing, proc_stats.proc_lookup_invalid_module,
          proc_stats.proc_lookup_fallback_hits, prebootstrap_wait_watch_object,
          thunk_counter_observation_enabled ? "ON" : "OFF",
          cvars::trace_gpu_bootstrap ? "ON" : "OFF",
          cvars::log_gpu_import_thunks ? "ON" : "OFF");
      XELOGW(
          "RING BUFFER: pre-bootstrap ordinal activity "
          "01C2(dyn={},thunk={},export={}) "
          "01C3(dyn={},thunk={},export={}) "
          "01D5(dyn={},thunk={},export={})",
          stat_01C2 ? stat_01C2->dynamic_lookup_calls : 0,
          stat_01C2 ? stat_01C2->static_thunk_calls : 0,
          stat_01C2 ? stat_01C2->export_entry_calls : 0,
          stat_01C3 ? stat_01C3->dynamic_lookup_calls : 0,
          stat_01C3 ? stat_01C3->static_thunk_calls : 0,
          stat_01C3 ? stat_01C3->export_entry_calls : 0,
          stat_01D5 ? stat_01D5->dynamic_lookup_calls : 0,
          stat_01D5 ? stat_01D5->static_thunk_calls : 0,
          stat_01D5 ? stat_01D5->export_entry_calls : 0);
      XELOGW(
          "RING BUFFER: pre-bootstrap MMIO summary any_writes={} "
          "non_cp_writes={} first_reg_state={} first_reg={:04X} "
          "last_reg={:04X} last_value={:08X} last_pc={:08X} "
          "last_thread_id={}",
          mmio_any_writes, mmio_non_cp_writes,
          mmio_first_reg == 0xFFFFFFFFu ? "NONE" : "SEEN", mmio_first_reg,
          mmio_last_reg, mmio_last_value, mmio_last_pc, mmio_last_thread_id);
      if ((stat_01C2 ? (stat_01C2->dynamic_lookup_calls +
                        stat_01C2->static_thunk_calls +
                        stat_01C2->export_entry_calls)
                     : 0) == 0 &&
          (stat_01C3 ? (stat_01C3->dynamic_lookup_calls +
                        stat_01C3->static_thunk_calls +
                        stat_01C3->export_entry_calls)
                     : 0) == 0 &&
          (stat_01D5 ? (stat_01D5->dynamic_lookup_calls +
                        stat_01D5->static_thunk_calls +
                        stat_01D5->export_entry_calls)
                     : 0) == 0) {
        if (thunk_counter_observation_enabled) {
          XELOGW(
              "RING BUFFER: pre-bootstrap diagnosis: no dynamic lookups, "
              "static thunk calls, or export entries observed yet for "
              "ordinals 0x01C2/0x01C3/0x01D5");
        } else {
          XELOGW(
              "RING BUFFER: pre-bootstrap diagnosis: no dynamic lookups or "
              "export entries observed yet for ordinals "
              "0x01C2/0x01C3/0x01D5; static-thunk counters are currently "
              "disabled by config");
        }
      }
    }
  }
  if (interrupt_callback_ == 0 && guest_main_thread_ready) {
    if (cvars::gpu_vblank_callback_startup_gate && command_processor_) {
      auto gate_rb_state = command_processor_->GetRingBufferDebugState();
      uint64_t gate_vd_init_count =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t gate_vd_cb_count =
          vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
      uint64_t gate_vd_swap_count =
          vd_swap_state_.count.load(std::memory_order_relaxed);
      uint64_t gate_mmio_rb_base_writes =
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
      uint64_t gate_mmio_rb_cntl_writes =
          mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
      uint64_t gate_mmio_rb_wptr_writes =
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
      auto gate_proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
      DispatchPathEvidence gate_dispatch_evidence =
          ClassifyDispatchPathEvidence(gate_proc_stats);
      uint64_t gate_runtime_bootstrap_total =
          GetGpuBootstrapOrdinalRuntimeTotal(gate_proc_stats, 0x01C2) +
          GetGpuBootstrapOrdinalRuntimeTotal(gate_proc_stats, 0x01C3) +
          GetGpuBootstrapOrdinalRuntimeTotal(gate_proc_stats, 0x01D5);
      uint64_t gate_runtime_dispatch_total =
          gate_dispatch_evidence.combined_runtime_dispatch_total;
      bool gate_bootstrap_or_runtime_seen =
          gate_vd_init_count != 0 || gate_vd_cb_count != 0 ||
          gate_vd_swap_count != 0 || gate_mmio_rb_base_writes != 0 ||
          gate_mmio_rb_cntl_writes != 0 || gate_mmio_rb_wptr_writes != 0 ||
          gate_runtime_bootstrap_total != 0 ||
          gate_runtime_dispatch_total != 0 ||
          gate_proc_stats.proc_lookup_calls != 0 ||
          gate_proc_stats.proc_lookup_resolved != 0 ||
          gate_proc_stats.proc_lookup_missing != 0;
      auto gate_executable_module =
          kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
      bool gate_executable_present = gate_executable_module != nullptr;
      uint32_t gate_executable_entry =
          gate_executable_present ? gate_executable_module->entry_point() : 0;
      bool gate_module_ready =
          gate_executable_present && gate_executable_entry != 0 &&
          !module_load_stats.load_inflight;
      bool gate_ring_configured =
          gate_rb_state.ring_buffer_initialized &&
          gate_rb_state.primary_buffer_ptr != 0 &&
          gate_rb_state.primary_buffer_size != 0;
      bool gate_ring_ready =
          gate_ring_configured && gate_rb_state.initialization_acknowledged;
      bool gate_startup_bridge_ready =
          gate_ring_configured && guest_main_thread_running && gate_module_ready;
      bool gate_dispatch_path_gate_ready =
          gate_dispatch_evidence.evidence_case != DispatchPathEvidenceCase::kNone;
      bool gate_callback_startup_gate_ready =
          gate_dispatch_path_gate_ready && gate_ring_ready &&
          gate_startup_bridge_ready && gate_bootstrap_or_runtime_seen;

      static std::atomic<uint64_t> callback_startup_gate_last_log_ms{0};
      uint64_t gate_log_interval_ms = static_cast<uint64_t>(
          cvars::gpu_vblank_callback_startup_gate_log_interval_ms);
      uint64_t last_gate_log =
          callback_startup_gate_last_log_ms.load(std::memory_order_relaxed);
      bool gate_log_due = last_gate_log == 0 || gate_log_interval_ms == 0 ||
                          now_ms < last_gate_log ||
                          now_ms - last_gate_log >= gate_log_interval_ms;
      if (gate_log_due) {
        callback_startup_gate_last_log_ms.store(now_ms,
                                                std::memory_order_relaxed);
        std::string gate_dispatch_stall_state;
        uint32_t gate_dispatch_stall_repeat_samples = 0;
        uint64_t gate_dispatch_stall_sample_ms = 0;
        uint64_t gate_dispatch_stall_age_ms = 0;
        if (kernel_state_) {
          auto* emulator = kernel_state_->emulator();
          if (emulator) {
            bool have_gate_dispatch_stall =
                emulator->GetLastGpuDispatchStallSnapshot(
                    &gate_dispatch_stall_state, nullptr, nullptr, nullptr,
                    &gate_dispatch_stall_repeat_samples,
                    &gate_dispatch_stall_sample_ms);
            if (have_gate_dispatch_stall && gate_dispatch_stall_sample_ms != 0 &&
                now_ms >= gate_dispatch_stall_sample_ms) {
              gate_dispatch_stall_age_ms =
                  now_ms - gate_dispatch_stall_sample_ms;
            }
            bool gate_should_sample_dispatch_stall =
                interrupt_callback_ == 0 &&
                gate_dispatch_evidence.evidence_case ==
                    DispatchPathEvidenceCase::kGuestMainDispatchBridge &&
                gate_runtime_bootstrap_total == 0 &&
                gate_runtime_dispatch_total == 0 && gate_vd_init_count == 0 &&
                gate_vd_cb_count == 0 && gate_vd_swap_count == 0 &&
                gate_mmio_rb_base_writes == 0 && gate_mmio_rb_cntl_writes == 0 &&
                gate_mmio_rb_wptr_writes == 0 &&
                gate_proc_stats.loader_module_handle_calls_total == 0 &&
                gate_proc_stats.loader_proc_lookup_calls_total == 0 &&
                gate_proc_stats.proc_lookup_calls == 0 &&
                gate_proc_stats.proc_lookup_resolved == 0 &&
                gate_proc_stats.proc_lookup_missing == 0;
            if (gate_should_sample_dispatch_stall) {
              static std::atomic<uint64_t>
                  callback_startup_gate_dispatch_stall_sample_last_ms{0};
              uint64_t sample_interval_ms =
                  gate_log_interval_ms == 0
                      ? 0
                      : (have_gate_dispatch_stall &&
                                 !gate_dispatch_stall_state.empty()
                             ? gate_log_interval_ms
                             : std::min<uint64_t>(gate_log_interval_ms, 50));
              uint64_t last_sample_ms =
                  callback_startup_gate_dispatch_stall_sample_last_ms.load(
                      std::memory_order_relaxed);
              bool sample_due =
                  last_sample_ms == 0 || sample_interval_ms == 0 ||
                  now_ms < last_sample_ms ||
                  now_ms - last_sample_ms >= sample_interval_ms;
              if (sample_due) {
                callback_startup_gate_dispatch_stall_sample_last_ms.store(
                    now_ms, std::memory_order_relaxed);
                emulator->LogMainThreadGpuDispatchStallSample(
                    "gpu-callback-startup-gate-stall", 0, 0, 0);
                have_gate_dispatch_stall =
                    emulator->GetLastGpuDispatchStallSnapshot(
                        &gate_dispatch_stall_state, nullptr, nullptr, nullptr,
                        &gate_dispatch_stall_repeat_samples,
                        &gate_dispatch_stall_sample_ms);
                gate_dispatch_stall_age_ms =
                    (have_gate_dispatch_stall &&
                     gate_dispatch_stall_sample_ms != 0 &&
                     now_ms >= gate_dispatch_stall_sample_ms)
                        ? (now_ms - gate_dispatch_stall_sample_ms)
                        : 0;
              }
            }
          }
        }
        auto is_callback_starvation_dispatch_stall =
            [](const std::string& state) -> bool {
          return state == "startup_prolog_helper_only" ||
                 state == "gpu_bootstrap_entry_loop_before_callback" ||
                 state == "title_local_privilege_gate_before_gpu_dispatch" ||
                 state == "title_local_busy_loop_without_import_dispatch" ||
                 state == "title_local_busy_loop_before_gpu_dispatch" ||
                 state == "busy_loop_before_gpu_dispatch";
        };
        auto callback_dispatch_stall_blocked_at =
            [](const std::string& state) -> const char* {
          if (state == "startup_prolog_helper_only") {
            return "startup_prolog_helper_only";
          }
          if (state == "gpu_bootstrap_entry_loop_before_callback") {
            return "gpu_bootstrap_entry_loop_before_callback";
          }
          if (state == "title_local_privilege_gate_before_gpu_dispatch") {
            return "title_local_privilege_gate_before_gpu_dispatch";
          }
          if (state == "title_local_busy_loop_without_import_dispatch") {
            return "title_local_busy_loop_without_import_dispatch";
          }
          if (state == "title_local_busy_loop_before_gpu_dispatch") {
            return "title_local_busy_loop_before_gpu_dispatch";
          }
          if (state == "busy_loop_before_gpu_dispatch") {
            return "busy_loop_before_gpu_dispatch";
          }
          return "recovery_disabled_by_config";
        };
        auto callback_dispatch_stall_summary =
            [](const std::string& state) -> const char* {
          if (state == "startup_prolog_helper_only") {
            return "guest main thread remains in same-module prolog helper "
                   "dispatch before any loader or GPU callback registration "
                   "path executes";
          }
          if (state == "gpu_bootstrap_entry_loop_before_callback") {
            return "guest main thread remains in the early GPU bootstrap "
                   "entry loop before callback registration";
          }
          if (state == "title_local_privilege_gate_before_gpu_dispatch") {
            return "guest main thread remains behind a title-local privilege "
                   "gate before any loader or GPU dispatch path reaches "
                   "callback registration";
          }
          if (state == "title_local_busy_loop_without_import_dispatch") {
            return "guest main thread remains in a title-local busy loop "
                   "without loader/import dispatch before callback "
                   "registration";
          }
          if (state == "title_local_busy_loop_before_gpu_dispatch") {
            return "guest main thread remains in a title-local busy loop "
                   "before callback registration";
          }
          if (state == "busy_loop_before_gpu_dispatch") {
            return "guest main thread remains in a busy loop before callback "
                   "registration";
          }
          return "callback recovery remains disabled by config";
        };
        bool gate_startup_prolog_helper_grace =
            IsStartupPrologHelperGraceWindow(gate_dispatch_stall_state,
                                             gate_dispatch_stall_age_ms,
                                             gate_dispatch_stall_repeat_samples);
        bool gate_callback_startup_gate_ready_effective =
            gate_callback_startup_gate_ready &&
            !gate_startup_prolog_helper_grace;
        bool gate_startup_prereqs_ready_effective =
            gate_startup_bridge_ready &&
            gate_callback_startup_gate_ready_effective &&
            gate_bootstrap_or_runtime_seen;
        std::string gate_unmet_startup_conditions_effective =
            FormatStartupGateUnmetConditions(
                gate_startup_bridge_ready,
                gate_callback_startup_gate_ready_effective,
                gate_bootstrap_or_runtime_seen);
        if (!gate_dispatch_path_gate_ready) {
          if (!gate_unmet_startup_conditions_effective.empty()) {
            gate_unmet_startup_conditions_effective.append(",");
          }
          gate_unmet_startup_conditions_effective.append(
              "dispatch_path_gate_ready");
        }
        bool gate_callback_starvation_stall =
            !gate_startup_prolog_helper_grace &&
            is_callback_starvation_dispatch_stall(gate_dispatch_stall_state);
        bool gate_bridge_only_callback_starvation =
            !gate_startup_prolog_helper_grace &&
            gate_dispatch_evidence.guest_main_dispatch_bridge_total != 0 &&
            gate_runtime_bootstrap_total == 0 && gate_runtime_dispatch_total == 0 &&
            gate_vd_init_count == 0 && gate_vd_cb_count == 0 &&
            gate_vd_swap_count == 0 && gate_mmio_rb_base_writes == 0 &&
            gate_mmio_rb_cntl_writes == 0 && gate_mmio_rb_wptr_writes == 0 &&
            gate_proc_stats.loader_module_handle_calls_total == 0 &&
            gate_proc_stats.loader_proc_lookup_calls_total == 0 &&
            gate_proc_stats.proc_lookup_calls == 0 &&
            gate_proc_stats.proc_lookup_resolved == 0 &&
            gate_proc_stats.proc_lookup_missing == 0;
        const char* blocked_at = "ready_for_recovery";
        std::string blocked_detail =
            cvars::gpu_no_force_allow_callback_recovery_on_startup_gate
                ? "startup gate prerequisites satisfied; waiting for callback "
                  "recovery"
                : "startup gate prerequisites satisfied; callback recovery "
                  "remains disabled by config";
        if (!guest_main_thread_running) {
          blocked_at = "guest_main_thread_running";
          blocked_detail = "guest main thread is not running";
        } else if (module_load_stats.load_inflight) {
          blocked_at = "module_load_inflight";
          blocked_detail = fmt::format(
              "module load still inflight (load_calls={} progress_count={} "
              "loaded_exec_modules={})",
              module_load_stats.load_calls, module_load_stats.load_progress_count,
              module_load_stats.loaded_executable_user_modules);
        } else if (!gate_executable_present) {
          blocked_at = "executable_module";
          blocked_detail = "executable module is not visible yet";
        } else if (gate_executable_entry == 0) {
          blocked_at = "executable_entry";
          blocked_detail = "executable module entry point is zero";
        } else if (!gate_startup_bridge_ready) {
          blocked_at = "startup_bridge_ready";
          blocked_detail = fmt::format(
              "startup bridge not ready yet "
              "(ring_configured={} guest_main_running={} module_ready={} "
              "rb_base={:08X} rb_size={:08X})",
              gate_ring_configured ? "YES" : "NO",
              guest_main_thread_running ? "YES" : "NO",
              gate_module_ready ? "YES" : "NO", gate_rb_state.primary_buffer_ptr,
              gate_rb_state.primary_buffer_size);
        } else if (!gate_callback_startup_gate_ready_effective) {
          blocked_at = "callback_startup_gate_ready";
          if (gate_startup_prolog_helper_grace) {
            blocked_at = "startup_prolog_helper_grace";
            blocked_detail = fmt::format(
                "guest main thread has only reached the initial startup "
                "prolog window after processor dispatch entry "
                "(cached_age_ms={} cached_repeat_samples={})",
                gate_dispatch_stall_age_ms, gate_dispatch_stall_repeat_samples);
          } else if (!gate_ring_ready) {
            blocked_detail = fmt::format(
                "ring handshake incomplete "
                "(ring_init={} init_ack={} rb_base={:08X} rb_size={:08X})",
                gate_rb_state.ring_buffer_initialized ? "YES" : "NO",
                gate_rb_state.initialization_acknowledged ? "YES" : "NO",
                gate_rb_state.primary_buffer_ptr, gate_rb_state.primary_buffer_size);
          } else if (!gate_dispatch_path_gate_ready) {
            blocked_at = "dispatch_path_gate_ready";
            blocked_detail = fmt::format(
                "no dynamic XexGetProcedureAddress lookup, static sc2 thunk "
                "dispatch, or xboxkrnl_video export entry observed yet "
                "(runtime_dispatch_total={} runtime_bootstrap_total={} "
                "critical_runtime_dispatch_total={} "
                "guest_main_dispatch_bridge_total={} proc_lookup_calls={})",
                gate_runtime_dispatch_total, gate_runtime_bootstrap_total,
                gate_dispatch_evidence.critical_runtime_dispatch_total,
                gate_dispatch_evidence.guest_main_dispatch_bridge_total,
                gate_proc_stats.proc_lookup_calls);
          } else if (!gate_bootstrap_or_runtime_seen) {
            blocked_detail = fmt::format(
                "no Vd/MMIO/runtime bootstrap activity observed "
                "(vd_init={} vd_cb={} vd_swap={} mmio_base={} mmio_cntl={} "
                "mmio_wptr={} runtime_total={} proc_lookup_calls={})",
                gate_vd_init_count, gate_vd_cb_count, gate_vd_swap_count,
                gate_mmio_rb_base_writes, gate_mmio_rb_cntl_writes,
                gate_mmio_rb_wptr_writes, gate_runtime_bootstrap_total,
                gate_proc_stats.proc_lookup_calls);
          } else {
            blocked_detail =
                "callback startup gate prerequisites not fully satisfied";
          }
        } else if (!cvars::gpu_no_force_allow_callback_recovery_on_startup_gate &&
                   gate_callback_starvation_stall) {
          blocked_at =
              callback_dispatch_stall_blocked_at(gate_dispatch_stall_state);
          blocked_detail = fmt::format(
              "{} (cached_age_ms={} cached_repeat_samples={}); startup gate "
              "prerequisites are satisfied, but callback recovery remains "
              "disabled by config",
              callback_dispatch_stall_summary(gate_dispatch_stall_state),
              gate_dispatch_stall_age_ms, gate_dispatch_stall_repeat_samples);
        } else if (!cvars::gpu_no_force_allow_callback_recovery_on_startup_gate &&
                   gate_bridge_only_callback_starvation) {
          blocked_at = "guest_dispatch_bridge_callback_starvation";
          blocked_detail = fmt::format(
              "guest dispatch bridge executed, but no loader, callback, MMIO, "
              "or tracked runtime GPU progress followed "
              "(guest_main_dispatch_bridge_total={} stall_state={} "
              "stall_age_ms={} stall_repeat_samples={} "
              "loader_module_handle_calls_total={} "
              "loader_proc_lookup_calls_total={}); startup gate prerequisites "
              "are satisfied, but callback recovery remains disabled by config",
              gate_dispatch_evidence.guest_main_dispatch_bridge_total,
              gate_dispatch_stall_state.empty() ? "<none>"
                                                : gate_dispatch_stall_state,
              gate_dispatch_stall_age_ms, gate_dispatch_stall_repeat_samples,
              gate_proc_stats.loader_module_handle_calls_total,
              gate_proc_stats.loader_proc_lookup_calls_total);
        } else if (!cvars::gpu_no_force_allow_callback_recovery_on_startup_gate &&
                   gate_startup_prolog_helper_grace) {
          blocked_at = "startup_prolog_helper_grace";
          blocked_detail = fmt::format(
              "guest main thread has only reached the initial startup prolog "
              "window after processor dispatch entry "
              "(cached_age_ms={} cached_repeat_samples={}); callback "
              "recovery remains disabled by config until that window "
              "persists",
              gate_dispatch_stall_age_ms, gate_dispatch_stall_repeat_samples);
        } else if (!cvars::gpu_no_force_allow_callback_recovery_on_startup_gate) {
          blocked_at = "recovery_disabled_by_config";
        }
        XELOGW(
            "RING BUFFER: callback startup gate {} (vblank_id={} age={}ms "
            "blocked_at={} detail='{}' guest_main_running={} module_ready={} "
            "ring_ready={} startup_bridge_ready={} "
            "callback_startup_gate_ready={} dispatch_path_gate_ready={} "
            "dispatch_evidence={} guest_main_dispatch_bridge_total={} "
            "bootstrap_or_runtime_seen={} "
            "unmet_startup_conditions='{}' no_force_profile={} "
            "startup_gate_recovery_enabled={} startup_prereq_hard_gate={})",
            gate_startup_prereqs_ready_effective ? "READY" : "WAITING",
            vblank_id,
            since_first_vblank, blocked_at, blocked_detail,
            guest_main_thread_running ? "YES" : "NO",
            gate_module_ready ? "YES" : "NO",
            gate_ring_ready ? "YES" : "NO",
            gate_startup_bridge_ready ? "YES" : "NO",
            gate_callback_startup_gate_ready_effective ? "YES" : "NO",
            gate_dispatch_path_gate_ready ? "YES" : "NO",
            DispatchPathEvidenceCaseName(
                gate_dispatch_evidence.evidence_case),
            gate_dispatch_evidence.guest_main_dispatch_bridge_total,
            gate_bootstrap_or_runtime_seen ? "YES" : "NO",
            gate_unmet_startup_conditions_effective,
            cvars::gpu_no_force_run_profile ? "ON" : "OFF",
            cvars::gpu_no_force_allow_callback_recovery_on_startup_gate
                ? "YES"
                : "NO",
            cvars::gpu_no_force_callback_startup_prereq_hard_gate ? "YES"
                                                                   : "NO");
      }
    }

    bool callback_recovered = TryAutoRecoverMissingInterruptCallback(
        now_ms, since_first_vblank, "callback-missing-vblank");
    if (!callback_recovered) {
      if (cvars::gpu_callback_missing_hard_gate_fallback && command_processor_) {
        uint32_t hard_gate_after_vblank =
            cvars::gpu_callback_missing_hard_gate_after_vblank;
        if (hard_gate_after_vblank == 0 || vblank_id >= hard_gate_after_vblank) {
          auto hard_rb_state = command_processor_->GetRingBufferDebugState();
          bool hard_ring_configured =
              hard_rb_state.ring_buffer_initialized &&
              hard_rb_state.primary_buffer_ptr != 0 &&
              hard_rb_state.primary_buffer_size != 0;
          bool hard_ring_ready =
              hard_ring_configured &&
              hard_rb_state.initialization_acknowledged;
          auto hard_executable_module =
              kernel_state_ ? kernel_state_->GetExecutableModule() : nullptr;
          bool hard_module_ready =
              hard_executable_module != nullptr &&
              hard_executable_module->entry_point() != 0 &&
              !module_load_stats.load_inflight;
          auto hard_proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
          auto hard_find_runtime_stat = [&](uint16_t ordinal)
              -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
            for (const auto& stat : hard_proc_stats.ordinal_runtime_stats) {
              if (stat.ordinal == ordinal) {
                return &stat;
              }
            }
            return nullptr;
          };
          const auto* hard_stat_01C3 = hard_find_runtime_stat(0x01C3);
          const auto* hard_stat_01D5 = hard_find_runtime_stat(0x01D5);
          const auto* hard_stat_025B = hard_find_runtime_stat(0x025B);
          uint64_t hard_vd_init_count =
              vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
          uint64_t hard_vd_cb_count =
              vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
          uint64_t hard_vd_swap_count =
              vd_swap_state_.count.load(std::memory_order_relaxed);
          uint64_t hard_mmio_base_count =
              mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
          uint64_t hard_mmio_cntl_count =
              mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
          uint64_t hard_mmio_wptr_count =
              mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
          DispatchPathEvidence hard_dispatch_evidence =
              ClassifyDispatchPathEvidence(hard_proc_stats);
          uint64_t hard_runtime_bootstrap_total =
              GetGpuBootstrapOrdinalRuntimeTotal(hard_proc_stats, 0x01C2) +
              GetGpuBootstrapOrdinalRuntimeTotal(hard_proc_stats, 0x01C3) +
              GetGpuBootstrapOrdinalRuntimeTotal(hard_proc_stats, 0x01D5) +
              GetGpuBootstrapOrdinalRuntimeTotal(hard_proc_stats, 0x025B);
          uint64_t hard_runtime_dispatch_total =
              hard_dispatch_evidence.combined_runtime_dispatch_total;
          bool hard_dispatch_path_gate_ready =
              hard_dispatch_evidence.evidence_case !=
              DispatchPathEvidenceCase::kNone;
          const char* hard_dispatch_path_blocked_at = "ready";
          std::string hard_dispatch_path_detail;
          switch (hard_dispatch_evidence.evidence_case) {
            case DispatchPathEvidenceCase::kCriticalGpuRuntimeDispatch:
              hard_dispatch_path_detail =
                  "critical GPU runtime dispatch observed via dynamic lookup, "
                  "static thunk dispatch, or export entry";
              break;
            case DispatchPathEvidenceCase::kGuestMainDispatchBridge:
              hard_dispatch_path_detail = fmt::format(
                  "guest main thread entered the dispatch bridge before "
                  "tracked GPU ordinals "
                  "(guest_main_dispatch_bridge_total={} "
                  "critical_runtime_dispatch_total={})",
                  hard_dispatch_evidence.guest_main_dispatch_bridge_total,
                  hard_dispatch_evidence.critical_runtime_dispatch_total);
              break;
            case DispatchPathEvidenceCase::kNone:
              hard_dispatch_path_detail =
                  "critical GPU runtime dispatch not observed yet";
              break;
          }
          bool hard_never_state =
              hard_vd_init_count == 0 && hard_vd_cb_count == 0 &&
              hard_vd_swap_count == 0 && hard_mmio_base_count == 0 &&
              hard_mmio_cntl_count == 0 && hard_mmio_wptr_count == 0 &&
              hard_runtime_bootstrap_total == 0 &&
              hard_runtime_dispatch_total == 0 &&
              hard_proc_stats.proc_lookup_calls == 0 &&
              hard_proc_stats.proc_lookup_resolved == 0 &&
              hard_proc_stats.proc_lookup_missing == 0;
          bool hard_bootstrap_activity = !hard_never_state;
          bool hard_startup_bridge_ready =
              hard_ring_configured && guest_main_thread_running &&
              hard_module_ready;
          auto append_startup_condition =
              [](std::string* unmet_conditions, const char* condition) {
                if (!unmet_conditions || !condition || !condition[0]) {
                  return;
                }
                if (!unmet_conditions->empty()) {
                  unmet_conditions->append(",");
                }
                unmet_conditions->append(condition);
              };
          auto evaluate_hard_import_probe =
              [&](uint16_t ordinal, const char** out_blocked_at,
                  std::string* out_detail) -> bool {
            if (out_blocked_at) {
              *out_blocked_at = "module_visibility";
            }
            if (out_detail) {
              *out_detail = "executable module/import library not ready";
            }
            if (!hard_executable_module || !hard_executable_module->xex_module()) {
              return false;
            }
            const auto* import_libs =
                hard_executable_module->xex_module()->import_libraries();
            if (!import_libs) {
              if (out_blocked_at) {
                *out_blocked_at = "import_libraries";
              }
              if (out_detail) {
                *out_detail = "xex import library table is missing";
              }
              return false;
            }

            uint32_t value_addr = 0;
            uint32_t thunk_addr = 0;
            for (const auto& lib : *import_libs) {
              if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
                continue;
              }
              for (const auto& import_fn : lib.imports) {
                if (import_fn.ordinal != ordinal) {
                  continue;
                }
                value_addr = import_fn.value_address;
                thunk_addr = import_fn.thunk_address;
                break;
              }
              if (value_addr || thunk_addr) {
                break;
              }
            }

            const char* ordinal_name =
                kernel::xboxkrnl::GetGpuBootstrapOrdinalName(ordinal);
            if (!value_addr || !thunk_addr) {
              if (out_blocked_at) {
                *out_blocked_at = "static_import_wiring";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} missing static import wiring "
                    "(value_addr={:08X} thunk_addr={:08X})",
                    static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                    thunk_addr);
              }
              return false;
            }
            if (!kernel_state_ || !kernel_state_->memory()) {
              if (out_blocked_at) {
                *out_blocked_at = "memory";
              }
              if (out_detail) {
                *out_detail = "kernel memory is unavailable";
              }
              return false;
            }

            uint32_t value_word = 0;
            uint32_t thunk_word0 = 0;
            uint32_t thunk_word1 = 0;
            bool value_committed = false;
            bool value_translated = false;
            bool thunk0_committed = false;
            bool thunk0_translated = false;
            bool thunk1_committed = false;
            bool thunk1_translated = false;
            ReadGuestWordSnapshot(kernel_state_->memory(), value_addr,
                                  &value_word, &value_committed,
                                  &value_translated);
            ReadGuestWordSnapshot(kernel_state_->memory(), thunk_addr,
                                  &thunk_word0, &thunk0_committed,
                                  &thunk0_translated);
            ReadGuestWordSnapshot(kernel_state_->memory(), thunk_addr + 4,
                                  &thunk_word1, &thunk1_committed,
                                  &thunk1_translated);
            if (!value_committed || !value_translated) {
              if (out_blocked_at) {
                *out_blocked_at = "value_probe";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} value probe unreadable "
                    "(value_addr={:08X} committed={} translated={})",
                    static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                    value_committed ? "YES" : "NO",
                    value_translated ? "YES" : "NO");
              }
              return false;
            }
            if (!thunk0_committed || !thunk0_translated || !thunk1_committed ||
                !thunk1_translated) {
              if (out_blocked_at) {
                *out_blocked_at = "thunk_probe";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} thunk probe unreadable "
                    "(thunk_addr={:08X} committed0={} translated0={} "
                    "committed1={} translated1={})",
                    static_cast<uint32_t>(ordinal), ordinal_name, thunk_addr,
                    thunk0_committed ? "YES" : "NO",
                    thunk0_translated ? "YES" : "NO",
                    thunk1_committed ? "YES" : "NO",
                    thunk1_translated ? "YES" : "NO");
              }
              return false;
            }
            if (value_word != thunk_addr) {
              if (out_blocked_at) {
                *out_blocked_at = "value_not_patched_to_thunk";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} value slot not patched to thunk "
                    "(value_addr={:08X} value_word={:08X} thunk_addr={:08X})",
                    static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                    value_word, thunk_addr);
              }
              return false;
            }
            bool thunk_sc2_stub =
                thunk_word0 == 0x44000042u && thunk_word1 == 0x4E800020u;
            if (!thunk_sc2_stub) {
              if (out_blocked_at) {
                *out_blocked_at = "thunk_not_sc2_stub";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} thunk stub mismatch "
                    "(thunk_addr={:08X} w0={:08X} w1={:08X})",
                    static_cast<uint32_t>(ordinal), ordinal_name, thunk_addr,
                    thunk_word0, thunk_word1);
              }
              return false;
            }

            const cpu::Function* value_fn =
                (processor_ && value_word) ? processor_->LookupFunction(value_word)
                                           : nullptr;
            const cpu::Function* thunk_fn =
                (processor_ && thunk_addr) ? processor_->LookupFunction(thunk_addr)
                                           : nullptr;
            if (!value_fn ||
                value_fn->behavior() != cpu::Function::Behavior::kExtern) {
              if (out_blocked_at) {
                *out_blocked_at = "value_lookup";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} value lookup not extern "
                    "(value_word={:08X} has_fn={} behavior={})",
                    static_cast<uint32_t>(ordinal), ordinal_name, value_word,
                    value_fn ? "YES" : "NO",
                    value_fn ? static_cast<int>(value_fn->behavior()) : -1);
              }
              return false;
            }
            if (!thunk_fn ||
                thunk_fn->behavior() != cpu::Function::Behavior::kExtern) {
              if (out_blocked_at) {
                *out_blocked_at = "thunk_lookup";
              }
              if (out_detail) {
                *out_detail = fmt::format(
                    "ordinal=0x{:03X} name={} thunk lookup not extern "
                    "(thunk_addr={:08X} has_fn={} behavior={})",
                    static_cast<uint32_t>(ordinal), ordinal_name, thunk_addr,
                    thunk_fn ? "YES" : "NO",
                    thunk_fn ? static_cast<int>(thunk_fn->behavior()) : -1);
              }
              return false;
            }

            if (out_blocked_at) {
              *out_blocked_at = "ready";
            }
            if (out_detail) {
              *out_detail = fmt::format(
                  "ordinal=0x{:03X} name={} value_addr={:08X} thunk_addr={:08X} "
                  "value_word={:08X} stub=sc2/blr extern-ready",
                  static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                  thunk_addr, value_word);
            }
            return true;
          };
          const char* hard_probe_01C3_blocked_at = "module_visibility";
          const char* hard_probe_01D5_blocked_at = "module_visibility";
          const char* hard_probe_025B_blocked_at = "module_visibility";
          std::string hard_probe_01C3_detail;
          std::string hard_probe_01D5_detail;
          std::string hard_probe_025B_detail;
          bool hard_probe_01C3_ready = evaluate_hard_import_probe(
              0x01C3, &hard_probe_01C3_blocked_at, &hard_probe_01C3_detail);
          bool hard_probe_01D5_ready = evaluate_hard_import_probe(
              0x01D5, &hard_probe_01D5_blocked_at, &hard_probe_01D5_detail);
          bool hard_probe_025B_ready = evaluate_hard_import_probe(
              0x025B, &hard_probe_025B_blocked_at, &hard_probe_025B_detail);
          bool hard_import_probe_gate_ready =
              hard_probe_01C3_ready && hard_probe_01D5_ready &&
              hard_probe_025B_ready;
          const char* hard_import_probe_blocked_at = "ready";
          std::string hard_import_probe_detail =
              "critical GPU import probes are valid";
          if (!hard_probe_01C3_ready) {
            hard_import_probe_blocked_at = hard_probe_01C3_blocked_at;
            hard_import_probe_detail = hard_probe_01C3_detail;
          } else if (!hard_probe_01D5_ready) {
            hard_import_probe_blocked_at = hard_probe_01D5_blocked_at;
            hard_import_probe_detail = hard_probe_01D5_detail;
          } else if (!hard_probe_025B_ready) {
            hard_import_probe_blocked_at = hard_probe_025B_blocked_at;
            hard_import_probe_detail = hard_probe_025B_detail;
          }
          bool hard_callback_startup_gate_ready =
              hard_import_probe_gate_ready && hard_dispatch_path_gate_ready &&
              hard_ring_ready && hard_startup_bridge_ready &&
              hard_bootstrap_activity;
          bool hard_startup_prereqs_ready =
              hard_startup_bridge_ready && hard_callback_startup_gate_ready &&
              hard_bootstrap_activity;
          std::string hard_unmet_startup_conditions =
              FormatStartupGateUnmetConditions(hard_startup_bridge_ready,
                                               hard_callback_startup_gate_ready,
                                               hard_bootstrap_activity);
          if (!hard_import_probe_gate_ready) {
            append_startup_condition(&hard_unmet_startup_conditions,
                                     "import_probe_gate_ready");
          }
          if (hard_import_probe_gate_ready && !hard_dispatch_path_gate_ready) {
            hard_dispatch_path_blocked_at = "critical_dispatch_paths";
            hard_dispatch_path_detail = fmt::format(
                "no dynamic XexGetProcedureAddress lookup, static sc2 thunk "
                "dispatch, or xboxkrnl_video export entry observed after valid "
                "critical import probes "
                "(dispatch_evidence={} guest_main_dispatch_bridge_total={} "
                "critical_runtime_dispatch_total={} "
                "01C3 dyn={} res={} miss={} inv={} fb={} thunk={} export={} "
                "01D5 dyn={} res={} miss={} inv={} fb={} thunk={} export={} "
                "025B dyn={} res={} miss={} inv={} fb={} thunk={} export={})",
                DispatchPathEvidenceCaseName(
                    hard_dispatch_evidence.evidence_case),
                hard_dispatch_evidence.guest_main_dispatch_bridge_total,
                hard_dispatch_evidence.critical_runtime_dispatch_total,
                hard_stat_01C3 ? hard_stat_01C3->dynamic_lookup_calls : 0,
                hard_stat_01C3
                    ? hard_stat_01C3->dynamic_lookup_resolved_calls
                    : 0,
                hard_stat_01C3 ? hard_stat_01C3->dynamic_lookup_missing_calls
                               : 0,
                hard_stat_01C3
                    ? hard_stat_01C3->dynamic_lookup_invalid_module_calls
                    : 0,
                hard_stat_01C3 ? hard_stat_01C3->dynamic_lookup_fallback_hits
                               : 0,
                hard_stat_01C3 ? hard_stat_01C3->static_thunk_calls : 0,
                hard_stat_01C3 ? hard_stat_01C3->export_entry_calls : 0,
                hard_stat_01D5 ? hard_stat_01D5->dynamic_lookup_calls : 0,
                hard_stat_01D5
                    ? hard_stat_01D5->dynamic_lookup_resolved_calls
                    : 0,
                hard_stat_01D5 ? hard_stat_01D5->dynamic_lookup_missing_calls
                               : 0,
                hard_stat_01D5
                    ? hard_stat_01D5->dynamic_lookup_invalid_module_calls
                    : 0,
                hard_stat_01D5 ? hard_stat_01D5->dynamic_lookup_fallback_hits
                               : 0,
                hard_stat_01D5 ? hard_stat_01D5->static_thunk_calls : 0,
                hard_stat_01D5 ? hard_stat_01D5->export_entry_calls : 0,
                hard_stat_025B ? hard_stat_025B->dynamic_lookup_calls : 0,
                hard_stat_025B
                    ? hard_stat_025B->dynamic_lookup_resolved_calls
                    : 0,
                hard_stat_025B ? hard_stat_025B->dynamic_lookup_missing_calls
                               : 0,
                hard_stat_025B
                    ? hard_stat_025B->dynamic_lookup_invalid_module_calls
                    : 0,
                hard_stat_025B ? hard_stat_025B->dynamic_lookup_fallback_hits
                               : 0,
                hard_stat_025B ? hard_stat_025B->static_thunk_calls : 0,
                hard_stat_025B ? hard_stat_025B->export_entry_calls : 0);
            append_startup_condition(&hard_unmet_startup_conditions,
                                     "dispatch_path_gate_ready");
          }
          bool hard_startup_prereq_hard_gate =
              cvars::gpu_no_force_run_profile &&
              cvars::gpu_no_force_callback_startup_prereq_hard_gate;
          static std::atomic<uint64_t> hard_fallback_last_ms{0};
          uint64_t fallback_interval_ms = static_cast<uint64_t>(
              cvars::gpu_callback_missing_hard_gate_interval_ms);
          uint64_t last_fallback_ms =
              hard_fallback_last_ms.load(std::memory_order_relaxed);
          bool fallback_log_due =
              last_fallback_ms == 0 || fallback_interval_ms == 0 ||
              now_ms < last_fallback_ms ||
              now_ms - last_fallback_ms >= fallback_interval_ms;

          bool hard_fallback_ready = guest_main_thread_running &&
                                     hard_module_ready && hard_ring_ready &&
                                     hard_never_state;
          bool hard_all_never_stalemate =
              hard_startup_prereq_hard_gate && !hard_startup_prereqs_ready &&
              hard_import_probe_gate_ready && hard_dispatch_path_gate_ready &&
              hard_fallback_ready;

          if (!hard_import_probe_gate_ready) {
            if (fallback_log_due) {
              hard_fallback_last_ms.store(now_ms, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: CALLBACK HARD GATE import-probe WAITING: fallback "
                  "blocked until critical GPU import probes are valid "
                  "(blocked_at={} detail='{}' probe_01C3={} probe_01D5={} "
                  "probe_025B={} startup_bridge_ready={} bootstrap_activity={} "
                  "guest_main_running={} module_ready={} ring_ready={})",
                  hard_import_probe_blocked_at, hard_import_probe_detail,
                  hard_probe_01C3_ready ? "READY" : "WAITING",
                  hard_probe_01D5_ready ? "READY" : "WAITING",
                  hard_probe_025B_ready ? "READY" : "WAITING",
                  hard_startup_bridge_ready ? "YES" : "NO",
                  hard_bootstrap_activity ? "YES" : "NO",
                  guest_main_thread_running ? "YES" : "NO",
                  hard_module_ready ? "YES" : "NO",
                  hard_ring_ready ? "YES" : "NO");
            }
          } else if (!hard_dispatch_path_gate_ready) {
            if (fallback_log_due) {
              hard_fallback_last_ms.store(now_ms, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: CALLBACK HARD GATE dispatch-path WAITING: fallback "
                  "blocked until at least one critical GPU dispatch path is "
                  "observed "
                  "(blocked_at={} detail='{}' startup_bridge_ready={} "
                  "import_probe_gate_ready={} bootstrap_activity={} "
                  "guest_main_running={} module_ready={} ring_ready={})",
                  hard_dispatch_path_blocked_at, hard_dispatch_path_detail,
                  hard_startup_bridge_ready ? "YES" : "NO",
                  hard_import_probe_gate_ready ? "YES" : "NO",
                  hard_bootstrap_activity ? "YES" : "NO",
                  guest_main_thread_running ? "YES" : "NO",
                  hard_module_ready ? "YES" : "NO",
                  hard_ring_ready ? "YES" : "NO");
            }
          } else if (hard_all_never_stalemate) {
            if (fallback_log_due) {
              hard_fallback_last_ms.store(now_ms, std::memory_order_relaxed);
              XELOGE(
                  "DEBUG: CALLBACK HARD GATE stalemate: all-<never> startup state "
                  "persists with ring/module/main ready; forcing mandatory "
                  "fallback initialization + guest bootstrap probe despite "
                  "unmet startup prerequisites "
                  "(unmet_startup_conditions='{}' "
                  "startup_bridge_ready={} import_probe_gate_ready={} "
                  "callback_startup_gate_ready={} "
                  "bootstrap_activity={} guest_main_running={} "
                  "module_ready={} ring_ready={})",
                  hard_unmet_startup_conditions,
                  hard_startup_bridge_ready ? "YES" : "NO",
                  hard_import_probe_gate_ready ? "YES" : "NO",
                  hard_callback_startup_gate_ready ? "YES" : "NO",
                  hard_bootstrap_activity ? "YES" : "NO",
                  guest_main_thread_running ? "YES" : "NO",
                  hard_module_ready ? "YES" : "NO",
                  hard_ring_ready ? "YES" : "NO");
              AttemptFallbackInitialization();
              InjectGuestBootstrapDebugActivity(
                  "callback-hard-gate-bootstrap-probe", since_first_vblank);
              callback_recovered =
                  interrupt_callback_ != 0 ||
                  TryAutoRecoverMissingInterruptCallback(
                      now_ms, since_first_vblank,
                      "callback-hard-gate-stalemate");
            }
          } else if (hard_startup_prereq_hard_gate &&
                     !hard_startup_prereqs_ready) {
            if (fallback_log_due) {
              hard_fallback_last_ms.store(now_ms, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: CALLBACK HARD GATE startup-prereq WAITING: fallback "
                  "blocked until startup prerequisites are met "
                  "(unmet_startup_conditions='{}' "
                  "startup_bridge_ready={} import_probe_gate_ready={} "
                  "callback_startup_gate_ready={} "
                  "bootstrap_activity={} guest_main_running={} "
                  "module_ready={} ring_ready={})",
                  hard_unmet_startup_conditions,
                  hard_startup_bridge_ready ? "YES" : "NO",
                  hard_import_probe_gate_ready ? "YES" : "NO",
                  hard_callback_startup_gate_ready ? "YES" : "NO",
                  hard_bootstrap_activity ? "YES" : "NO",
                  guest_main_thread_running ? "YES" : "NO",
                  hard_module_ready ? "YES" : "NO",
                  hard_ring_ready ? "YES" : "NO");
            }
          } else {
            if (hard_fallback_ready && fallback_log_due) {
              hard_fallback_last_ms.store(now_ms, std::memory_order_relaxed);
              XELOGE(
                  "DEBUG: CALLBACK HARD GATE fallback: callback missing in all-"
                  "<never> startup state (vblank_id={} age={}ms) - "
                  "attempting mandatory fallback initialization + guest "
                  "bootstrap probe",
                  vblank_id, since_first_vblank);
              AttemptFallbackInitialization();
              InjectGuestBootstrapDebugActivity(
                  "callback-hard-gate-bootstrap-probe", since_first_vblank);
              callback_recovered =
                  interrupt_callback_ != 0 ||
                  TryAutoRecoverMissingInterruptCallback(
                      now_ms, since_first_vblank,
                      "callback-hard-gate-fallback");
            }
          }
        }
      }
    }
    static std::atomic<uint64_t> no_cb_log_count{0};
    uint64_t cb_id = no_cb_log_count.fetch_add(1) + 1;
    if (cb_id <= 5 || (cb_id % 120) == 0) {
      uint64_t vd_cb_count =
          vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_init_count =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_swap_count =
          vd_swap_state_.count.load(std::memory_order_relaxed);
      uint64_t callback_set_ms =
          last_interrupt_callback_set_ms_.load(std::memory_order_relaxed);
      uint64_t callback_dispatch_ms =
          last_interrupt_dispatch_ms_.load(std::memory_order_relaxed);
      uint64_t callback_set_age =
          (callback_set_ms && now_ms >= callback_set_ms) ? (now_ms - callback_set_ms)
                                                         : 0;
      uint64_t callback_dispatch_age =
          (callback_dispatch_ms && now_ms >= callback_dispatch_ms)
              ? (now_ms - callback_dispatch_ms)
              : 0;
      auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
      XexCheckExecutablePrivilegeRuntimeState xexcheck_runtime_state =
          ClassifyXexCheckExecutablePrivilegeRuntimeState(proc_stats);
      auto find_ordinal_stat = [&](uint16_t ordinal)
          -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
        for (const auto& stat : proc_stats.ordinal_runtime_stats) {
          if (stat.ordinal == ordinal) {
            return &stat;
          }
        }
        return nullptr;
      };
      const auto* stat_01C2 = find_ordinal_stat(0x01C2);
      const auto* stat_01C3 = find_ordinal_stat(0x01C3);
      const auto* stat_01D5 = find_ordinal_stat(0x01D5);
        const auto* stat_025B = find_ordinal_stat(0x025B);
      bool thunk_counter_observation_enabled =
          cvars::trace_gpu_bootstrap || cvars::log_gpu_import_thunks;
      uint64_t host_debug_swap_age =
          (bootstrap_state.debug_host_swap_last_ms &&
           now_ms >= bootstrap_state.debug_host_swap_last_ms)
              ? (now_ms - bootstrap_state.debug_host_swap_last_ms)
              : 0;
      uint64_t wptr_update_total =
          wptr_update_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_mmio =
          wptr_source_mmio_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_cp_packet =
          wptr_source_cp_packet_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_debug_swap =
          wptr_source_debug_swap_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_other =
          wptr_source_other_count_.load(std::memory_order_relaxed);
      uint64_t wptr_guest_driven_count =
          wptr_source_mmio + wptr_source_cp_packet;
      uint32_t wptr_last_source_id =
          last_wptr_source_id_.load(std::memory_order_relaxed);
      uint32_t callback_wait_watch_object =
          kernel::xboxkrnl::GetWaitWatchObjectPtr();
      std::string callback_missing_stall_state;
      std::string callback_missing_stall_targets;
      uint32_t callback_missing_stall_repeat_samples = 0;
      uint64_t callback_missing_stall_sample_ms = 0;
      uint64_t callback_missing_stall_age_ms = 0;
      bool callback_missing_startup_prolog_helper_grace = false;
      bool callback_missing_stall_has_xex_privilege_scan = false;
      bool callback_missing_stall_xex_privilege_branch_only = false;
      if (kernel_state_) {
        auto* emulator = kernel_state_->emulator();
        if (emulator) {
          if (emulator->GetLastGpuDispatchStallSnapshot(
                  &callback_missing_stall_state, nullptr, nullptr,
                  &callback_missing_stall_targets,
                  &callback_missing_stall_repeat_samples,
                  &callback_missing_stall_sample_ms) &&
              callback_missing_stall_sample_ms != 0 &&
              now_ms >= callback_missing_stall_sample_ms) {
            callback_missing_stall_age_ms =
                now_ms - callback_missing_stall_sample_ms;
          }
          callback_missing_startup_prolog_helper_grace =
              IsStartupPrologHelperGraceWindow(
                  callback_missing_stall_state, callback_missing_stall_age_ms,
                  callback_missing_stall_repeat_samples);
          bool early_guest_main_startup_window = since_first_vblank < 25;
          bool should_sample_callback_missing_stall =
              interrupt_callback_ == 0 &&
              proc_stats.guest_main_dispatch_bridge_calls_total != 0 &&
              vd_init_count == 0 && vd_cb_count == 0 && vd_swap_count == 0 &&
              proc_stats.loader_module_handle_calls_total == 0 &&
              proc_stats.loader_proc_lookup_calls_total == 0 &&
              proc_stats.proc_lookup_calls == 0 &&
              proc_stats.proc_lookup_resolved == 0 &&
              proc_stats.proc_lookup_missing == 0 &&
              !callback_missing_startup_prolog_helper_grace &&
              !early_guest_main_startup_window;
          if (should_sample_callback_missing_stall) {
            static std::atomic<uint64_t>
                callback_missing_dispatch_stall_sample_last_ms{0};
            uint64_t sample_interval_ms = std::min<uint64_t>(
                static_cast<uint64_t>(
                    cvars::gpu_vblank_callback_startup_gate_log_interval_ms),
                50);
            uint64_t last_sample_ms =
                callback_missing_dispatch_stall_sample_last_ms.load(
                    std::memory_order_relaxed);
            bool sample_due =
                last_sample_ms == 0 || sample_interval_ms == 0 ||
                now_ms < last_sample_ms ||
                now_ms - last_sample_ms >= sample_interval_ms;
            if (sample_due) {
              callback_missing_dispatch_stall_sample_last_ms.store(
                  now_ms, std::memory_order_relaxed);
              emulator->LogMainThreadGpuDispatchStallSample(
                  "gpu-callback-missing-stall", 0, 0, 0);
              if (emulator->GetLastGpuDispatchStallSnapshot(
                      &callback_missing_stall_state, nullptr, nullptr,
                      &callback_missing_stall_targets,
                      &callback_missing_stall_repeat_samples,
                      &callback_missing_stall_sample_ms) &&
                  callback_missing_stall_sample_ms != 0 &&
                  now_ms >= callback_missing_stall_sample_ms) {
                callback_missing_stall_age_ms =
                    now_ms - callback_missing_stall_sample_ms;
              } else {
                callback_missing_stall_age_ms = 0;
              }
              callback_missing_startup_prolog_helper_grace =
                  IsStartupPrologHelperGraceWindow(
                      callback_missing_stall_state,
                      callback_missing_stall_age_ms,
                      callback_missing_stall_repeat_samples);
            }
          }
        }
      }
      callback_missing_stall_has_xex_privilege_scan =
          callback_missing_stall_targets.find("xex_privilege_check=YES") !=
          std::string::npos;
      callback_missing_stall_xex_privilege_branch_only =
          callback_missing_stall_has_xex_privilege_scan &&
          proc_stats.xex_check_executable_privilege_calls == 0;
      if (callback_missing_startup_prolog_helper_grace) {
        XELOGW(
            "RING BUFFER: callback-missing diagnostics deferred during "
            "startup prolog helper grace "
            "(vblank_id={} stall_age_ms={} repeat_samples={} "
            "guest_main_dispatch_bridge_calls_total={})",
            vblank_id, callback_missing_stall_age_ms,
            callback_missing_stall_repeat_samples,
            proc_stats.guest_main_dispatch_bridge_calls_total);
      } else {
        XELOGW(
            "DEBUG: GPU interrupt callback not set (vblank_id={}, read_ptr={:08X}, "
            "write_ptr={:08X})",
            vblank_id, read_ptr, write_ptr);
        XELOGW(
            "RING BUFFER: callback-missing probe vd_cb_count={} "
            "vd_init_count={} vd_swap_count={} callback_set_age_ms={} "
            "callback_dispatch_age_ms={} forced_callback={:08X}",
            vd_cb_count, vd_init_count, vd_swap_count, callback_set_age,
            callback_dispatch_age, forced_interrupt_callback_);
        XELOGW(
            "RING BUFFER: callback-missing runtime stats proc_lookup_calls={} "
            "proc_lookup_resolved={} proc_lookup_missing={} "
            "proc_lookup_invalid_module={} proc_lookup_fallback_hits={} "
            "loader_module_handle_calls_total={} "
            "loader_proc_lookup_calls_total={} "
            "guest_main_dispatch_bridge_calls_total={} "
            "guest_main_dispatch_stall_state={} "
            "guest_main_dispatch_stall_age_ms={} "
            "guest_main_dispatch_stall_repeat_samples={} "
            "xex_priv_scan_seen={} xex_check_privilege_calls={} "
            "xex_check_privilege_static_thunk_calls={} "
            "xex_check_privilege_export_entry_calls={} "
            "xex_check_runtime_state={} xex_priv_branch_only={} "
            "wait_watch_obj={:08X} thunk_counter_observation={} "
            "(trace_gpu_bootstrap={}, log_gpu_import_thunks={}) "
            "cp_host_debug_swaps={} cp_host_debug_swap_age_ms={} "
            "cp_host_debug_swap_ring={} cp_host_debug_swap_direct={} "
            "wptr_updates(total={},guest={},last_source={},mmio={},"
            "cp_packet={},debug_swap={},other={})",
            proc_stats.proc_lookup_calls, proc_stats.proc_lookup_resolved,
            proc_stats.proc_lookup_missing,
            proc_stats.proc_lookup_invalid_module,
            proc_stats.proc_lookup_fallback_hits,
            proc_stats.loader_module_handle_calls_total,
            proc_stats.loader_proc_lookup_calls_total,
            proc_stats.guest_main_dispatch_bridge_calls_total,
            callback_missing_stall_state.empty()
                ? "<none>"
                : callback_missing_stall_state,
            callback_missing_stall_age_ms,
            callback_missing_stall_repeat_samples,
            callback_missing_stall_has_xex_privilege_scan ? "YES" : "NO",
            proc_stats.xex_check_executable_privilege_calls,
            proc_stats.xex_check_executable_privilege_static_thunk_calls,
            proc_stats.xex_check_executable_privilege_export_entry_calls,
            XexCheckExecutablePrivilegeRuntimeStateName(xexcheck_runtime_state),
            callback_missing_stall_xex_privilege_branch_only ? "YES" : "NO",
            callback_wait_watch_object,
            thunk_counter_observation_enabled ? "ON" : "OFF",
            cvars::trace_gpu_bootstrap ? "ON" : "OFF",
            cvars::log_gpu_import_thunks ? "ON" : "OFF",
            bootstrap_state.debug_host_swap_injection_count,
            host_debug_swap_age,
            bootstrap_state.debug_host_swap_ring_injection_count,
            bootstrap_state.debug_host_swap_direct_issue_count,
            wptr_update_total, wptr_guest_driven_count,
            WptrUpdateSourceName(wptr_last_source_id), wptr_source_mmio,
            wptr_source_cp_packet, wptr_source_debug_swap, wptr_source_other);
      }
      XELOGW(
          "RING BUFFER: callback-missing ordinal activity "
          "01C2(dyn={},res={},miss={},inv={},fb={},thunk={},export={}) "
          "01C3(dyn={},res={},miss={},inv={},fb={},thunk={},export={}) "
          "01D5(dyn={},res={},miss={},inv={},fb={},thunk={},export={}) "
          "025B(dyn={},res={},miss={},inv={},fb={},thunk={},export={})",
          stat_01C2 ? stat_01C2->dynamic_lookup_calls : 0,
          stat_01C2 ? stat_01C2->dynamic_lookup_resolved_calls : 0,
          stat_01C2 ? stat_01C2->dynamic_lookup_missing_calls : 0,
          stat_01C2 ? stat_01C2->dynamic_lookup_invalid_module_calls : 0,
          stat_01C2 ? stat_01C2->dynamic_lookup_fallback_hits : 0,
          stat_01C2 ? stat_01C2->static_thunk_calls : 0,
          stat_01C2 ? stat_01C2->export_entry_calls : 0,
          stat_01C3 ? stat_01C3->dynamic_lookup_calls : 0,
          stat_01C3 ? stat_01C3->dynamic_lookup_resolved_calls : 0,
          stat_01C3 ? stat_01C3->dynamic_lookup_missing_calls : 0,
          stat_01C3 ? stat_01C3->dynamic_lookup_invalid_module_calls : 0,
          stat_01C3 ? stat_01C3->dynamic_lookup_fallback_hits : 0,
          stat_01C3 ? stat_01C3->static_thunk_calls : 0,
          stat_01C3 ? stat_01C3->export_entry_calls : 0,
          stat_01D5 ? stat_01D5->dynamic_lookup_calls : 0,
          stat_01D5 ? stat_01D5->dynamic_lookup_resolved_calls : 0,
          stat_01D5 ? stat_01D5->dynamic_lookup_missing_calls : 0,
          stat_01D5 ? stat_01D5->dynamic_lookup_invalid_module_calls : 0,
          stat_01D5 ? stat_01D5->dynamic_lookup_fallback_hits : 0,
          stat_01D5 ? stat_01D5->static_thunk_calls : 0,
          stat_01D5 ? stat_01D5->export_entry_calls : 0,
          stat_025B ? stat_025B->dynamic_lookup_calls : 0,
          stat_025B ? stat_025B->dynamic_lookup_resolved_calls : 0,
          stat_025B ? stat_025B->dynamic_lookup_missing_calls : 0,
          stat_025B ? stat_025B->dynamic_lookup_invalid_module_calls : 0,
          stat_025B ? stat_025B->dynamic_lookup_fallback_hits : 0,
          stat_025B ? stat_025B->static_thunk_calls : 0,
          stat_025B ? stat_025B->export_entry_calls : 0);
      kernel::object_ref<kernel::UserModule> executable_module;
      if (kernel_state_) {
        executable_module = kernel_state_->GetExecutableModule();
      }
      auto find_import_slot =
          [&](uint16_t ordinal, uint32_t* out_value_addr,
              uint32_t* out_thunk_addr) {
            if (out_value_addr) {
              *out_value_addr = 0;
            }
            if (out_thunk_addr) {
              *out_thunk_addr = 0;
            }
            if (!executable_module || !executable_module->xex_module()) {
              return;
            }
            const auto* import_libs =
                executable_module->xex_module()->import_libraries();
            if (!import_libs) {
              return;
            }
            for (const auto& lib : *import_libs) {
              if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
                continue;
              }
              for (const auto& import_fn : lib.imports) {
                if (import_fn.ordinal != ordinal) {
                  continue;
                }
                if (out_value_addr) {
                  *out_value_addr = import_fn.value_address;
                }
                if (out_thunk_addr) {
                  *out_thunk_addr = import_fn.thunk_address;
                }
                return;
              }
            }
          };

      auto has_static_import_wiring = [&](uint16_t ordinal,
                                          uint32_t* out_value_addr,
                                          uint32_t* out_thunk_addr) -> bool {
        uint32_t value_addr = 0;
        uint32_t thunk_addr = 0;
        find_import_slot(ordinal, &value_addr, &thunk_addr);
        if (out_value_addr) {
          *out_value_addr = value_addr;
        }
        if (out_thunk_addr) {
          *out_thunk_addr = thunk_addr;
        }
        return value_addr != 0 || thunk_addr != 0;
      };

      auto behavior_name = [](const cpu::Function* fn) -> const char* {
        if (!fn) {
          return "<null>";
        }
        switch (fn->behavior()) {
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
          default:
            return "Unknown";
        }
      };
          auto dump_import_probe = [&](uint16_t ordinal) {
          uint32_t value_addr = 0;
          uint32_t thunk_addr = 0;
          find_import_slot(ordinal, &value_addr, &thunk_addr);

          uint32_t value_word = 0;
          bool value_committed = false;
          bool value_translated = false;
          if (value_addr && kernel_state_) {
            ReadGuestWordSnapshot(kernel_state_->memory(), value_addr, &value_word,
                      &value_committed, &value_translated);
          }
          uint32_t thunk_w0 = 0;
          uint32_t thunk_w1 = 0;
          bool thunk_committed = false;
          bool thunk_translated = false;
          if (thunk_addr && kernel_state_) {
            ReadGuestWordSnapshot(kernel_state_->memory(), thunk_addr, &thunk_w0,
                      &thunk_committed, &thunk_translated);
            bool ignored_committed = false;
            bool ignored_translated = false;
            ReadGuestWordSnapshot(kernel_state_->memory(), thunk_addr + 4,
                      &thunk_w1, &ignored_committed,
                      &ignored_translated);
          }
          bool thunk_sc2_stub = thunk_translated && thunk_w0 == 0x44000042u &&
                     thunk_w1 == 0x4E800020u;
          const cpu::Function* value_fn =
            (processor_ && value_word) ? processor_->LookupFunction(value_word)
                           : nullptr;
          const cpu::Function* thunk_fn =
            (processor_ && thunk_addr) ? processor_->LookupFunction(thunk_addr)
                           : nullptr;

          XELOGW(
            "RING BUFFER: callback-missing import probe ordinal=0x{:03X} "
            "name={} value_addr={:08X} value_committed={} "
            "value_translated={} value_word={:08X} value_fn_behavior={} "
            "value_fn_status={} thunk_addr={:08X} thunk_committed={} "
            "thunk_translated={} thunk_w0={:08X} thunk_w1={:08X} "
            "thunk_sc2_stub={} thunk_fn_behavior={} thunk_fn_status={}",
            static_cast<uint32_t>(ordinal),
            kernel::xboxkrnl::GetGpuBootstrapOrdinalName(ordinal), value_addr,
            value_committed ? "YES" : "NO",
            value_translated ? "YES" : "NO", value_word,
            behavior_name(value_fn),
            value_fn ? static_cast<int>(value_fn->status()) : -1, thunk_addr,
            thunk_committed ? "YES" : "NO",
            thunk_translated ? "YES" : "NO", thunk_w0, thunk_w1,
            thunk_sc2_stub ? "YES" : "NO", behavior_name(thunk_fn),
            thunk_fn ? static_cast<int>(thunk_fn->status()) : -1);
          };

          dump_import_probe(0x01C3);
          dump_import_probe(0x01D5);
          dump_import_probe(0x025B);

      uint32_t value_01D5 = 0, thunk_01D5 = 0;
      uint32_t value_01C3 = 0, thunk_01C3 = 0;
      uint32_t value_025B = 0, thunk_025B = 0;
      bool wired_01D5 =
          has_static_import_wiring(0x01D5, &value_01D5, &thunk_01D5);
      bool wired_01C3 =
          has_static_import_wiring(0x01C3, &value_01C3, &thunk_01C3);
      bool wired_025B =
          has_static_import_wiring(0x025B, &value_025B, &thunk_025B);

      auto evaluate_import_probe_gate =
          [&](uint16_t ordinal, const char** out_blocked_at,
              std::string* out_detail) -> bool {
        if (out_blocked_at) {
          *out_blocked_at = "module_visibility";
        }
        if (out_detail) {
          *out_detail = "executable module/import library not ready";
        }
        uint32_t value_addr = 0;
        uint32_t thunk_addr = 0;
        find_import_slot(ordinal, &value_addr, &thunk_addr);
        const char* ordinal_name =
            kernel::xboxkrnl::GetGpuBootstrapOrdinalName(ordinal);
        if (!value_addr || !thunk_addr) {
          if (out_blocked_at) {
            *out_blocked_at = "static_import_wiring";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} missing static import wiring "
                "(value_addr={:08X} thunk_addr={:08X})",
                static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                thunk_addr);
          }
          return false;
        }
        if (!kernel_state_ || !kernel_state_->memory()) {
          if (out_blocked_at) {
            *out_blocked_at = "memory";
          }
          if (out_detail) {
            *out_detail = "kernel memory is unavailable";
          }
          return false;
        }

        uint32_t value_word = 0;
        uint32_t thunk_word0 = 0;
        uint32_t thunk_word1 = 0;
        bool value_committed = false;
        bool value_translated = false;
        bool thunk0_committed = false;
        bool thunk0_translated = false;
        bool thunk1_committed = false;
        bool thunk1_translated = false;
        ReadGuestWordSnapshot(kernel_state_->memory(), value_addr, &value_word,
                              &value_committed, &value_translated);
        ReadGuestWordSnapshot(kernel_state_->memory(), thunk_addr, &thunk_word0,
                              &thunk0_committed, &thunk0_translated);
        ReadGuestWordSnapshot(kernel_state_->memory(), thunk_addr + 4,
                              &thunk_word1, &thunk1_committed,
                              &thunk1_translated);
        if (!value_committed || !value_translated) {
          if (out_blocked_at) {
            *out_blocked_at = "value_probe";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} value probe unreadable "
                "(value_addr={:08X} committed={} translated={})",
                static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                value_committed ? "YES" : "NO",
                value_translated ? "YES" : "NO");
          }
          return false;
        }
        if (!thunk0_committed || !thunk0_translated || !thunk1_committed ||
            !thunk1_translated) {
          if (out_blocked_at) {
            *out_blocked_at = "thunk_probe";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} thunk probe unreadable "
                "(thunk_addr={:08X} committed0={} translated0={} committed1={} "
                "translated1={})",
                static_cast<uint32_t>(ordinal), ordinal_name, thunk_addr,
                thunk0_committed ? "YES" : "NO",
                thunk0_translated ? "YES" : "NO",
                thunk1_committed ? "YES" : "NO",
                thunk1_translated ? "YES" : "NO");
          }
          return false;
        }
        if (value_word != thunk_addr) {
          if (out_blocked_at) {
            *out_blocked_at = "value_not_patched_to_thunk";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} value slot not patched to thunk "
                "(value_addr={:08X} value_word={:08X} thunk_addr={:08X})",
                static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
                value_word, thunk_addr);
          }
          return false;
        }
        bool thunk_sc2_stub =
            thunk_word0 == 0x44000042u && thunk_word1 == 0x4E800020u;
        if (!thunk_sc2_stub) {
          if (out_blocked_at) {
            *out_blocked_at = "thunk_not_sc2_stub";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} thunk stub mismatch "
                "(thunk_addr={:08X} w0={:08X} w1={:08X})",
                static_cast<uint32_t>(ordinal), ordinal_name, thunk_addr,
                thunk_word0, thunk_word1);
          }
          return false;
        }

        const cpu::Function* value_fn =
            (processor_ && value_word) ? processor_->LookupFunction(value_word)
                                       : nullptr;
        const cpu::Function* thunk_fn =
            (processor_ && thunk_addr) ? processor_->LookupFunction(thunk_addr)
                                       : nullptr;
        if (!value_fn ||
            value_fn->behavior() != cpu::Function::Behavior::kExtern) {
          if (out_blocked_at) {
            *out_blocked_at = "value_lookup";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} value lookup not extern "
                "(value_word={:08X} has_fn={} behavior={})",
                static_cast<uint32_t>(ordinal), ordinal_name, value_word,
                value_fn ? "YES" : "NO",
                value_fn ? static_cast<int>(value_fn->behavior()) : -1);
          }
          return false;
        }
        if (!thunk_fn ||
            thunk_fn->behavior() != cpu::Function::Behavior::kExtern) {
          if (out_blocked_at) {
            *out_blocked_at = "thunk_lookup";
          }
          if (out_detail) {
            *out_detail = fmt::format(
                "ordinal=0x{:03X} name={} thunk lookup not extern "
                "(thunk_addr={:08X} has_fn={} behavior={})",
                static_cast<uint32_t>(ordinal), ordinal_name, thunk_addr,
                thunk_fn ? "YES" : "NO",
                thunk_fn ? static_cast<int>(thunk_fn->behavior()) : -1);
          }
          return false;
        }

        if (out_blocked_at) {
          *out_blocked_at = "ready";
        }
        if (out_detail) {
          *out_detail = fmt::format(
              "ordinal=0x{:03X} name={} value_addr={:08X} thunk_addr={:08X} "
              "value_word={:08X} stub=sc2/blr extern-ready",
              static_cast<uint32_t>(ordinal), ordinal_name, value_addr,
              thunk_addr, value_word);
        }
        return true;
      };
      const char* probe_01C3_blocked_at = "module_visibility";
      const char* probe_01D5_blocked_at = "module_visibility";
      const char* probe_025B_blocked_at = "module_visibility";
      std::string probe_01C3_detail;
      std::string probe_01D5_detail;
      std::string probe_025B_detail;
      bool probe_01C3_ready = evaluate_import_probe_gate(
          0x01C3, &probe_01C3_blocked_at, &probe_01C3_detail);
      bool probe_01D5_ready = evaluate_import_probe_gate(
          0x01D5, &probe_01D5_blocked_at, &probe_01D5_detail);
      bool probe_025B_ready = evaluate_import_probe_gate(
          0x025B, &probe_025B_blocked_at, &probe_025B_detail);
      bool import_probe_gate_ready =
          probe_01C3_ready && probe_01D5_ready && probe_025B_ready;
      const char* import_probe_blocked_at = "ready";
      std::string import_probe_detail =
          "critical GPU static import probes are valid";
      if (!probe_01C3_ready) {
        import_probe_blocked_at = probe_01C3_blocked_at;
        import_probe_detail = probe_01C3_detail;
      } else if (!probe_01D5_ready) {
        import_probe_blocked_at = probe_01D5_blocked_at;
        import_probe_detail = probe_01D5_detail;
      } else if (!probe_025B_ready) {
        import_probe_blocked_at = probe_025B_blocked_at;
        import_probe_detail = probe_025B_detail;
      }
      XELOGW(
          "RING BUFFER: import probe gate {} (blocked_at={} detail='{}' "
          "probe_01C3={} probe_01D5={} probe_025B={})",
          import_probe_gate_ready ? "READY" : "WAITING",
          import_probe_blocked_at, import_probe_detail,
          probe_01C3_ready ? "READY" : "WAITING",
          probe_01D5_ready ? "READY" : "WAITING",
          probe_025B_ready ? "READY" : "WAITING");

      auto runtime_stat_total =
          [](const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* stat)
              -> uint64_t { return GetGpuBootstrapOrdinalRuntimeTotal(stat); };
      uint64_t runtime_01C2_total = runtime_stat_total(stat_01C2);
      uint64_t runtime_01C3_total = runtime_stat_total(stat_01C3);
      uint64_t runtime_01D5_total = runtime_stat_total(stat_01D5);
      uint64_t runtime_025B_total = runtime_stat_total(stat_025B);
      DispatchPathEvidence dispatch_evidence =
          ClassifyDispatchPathEvidence(proc_stats);
      bool dispatch_path_gate_ready =
          dispatch_evidence.evidence_case != DispatchPathEvidenceCase::kNone;
      std::string cached_dispatch_stall_state;
      std::string cached_dispatch_stall_detail;
      std::string cached_dispatch_stall_chain;
      std::string cached_dispatch_stall_targets;
      uint32_t cached_dispatch_stall_repeat_samples = 0;
      uint64_t cached_dispatch_stall_ms = 0;
      bool have_cached_dispatch_stall = false;
      uint64_t cached_dispatch_stall_age_ms = 0;
      uint64_t mmio_base_guest =
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
      uint64_t mmio_cntl_guest =
          mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed);
      uint64_t mmio_wptr_guest =
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
      if (kernel_state_) {
        auto* emulator = kernel_state_->emulator();
        if (emulator) {
          bool should_sample_callback_starvation_stall =
              interrupt_callback_ == 0 && import_probe_gate_ready &&
              dispatch_evidence.evidence_case ==
                  DispatchPathEvidenceCase::kGuestMainDispatchBridge &&
              runtime_01C2_total == 0 && runtime_01C3_total == 0 &&
              runtime_01D5_total == 0 && runtime_025B_total == 0 &&
              vd_init_count == 0 && vd_cb_count == 0 && vd_swap_count == 0 &&
              mmio_base_guest == 0 && mmio_cntl_guest == 0 &&
              mmio_wptr_guest == 0 &&
              proc_stats.loader_module_handle_calls_total == 0 &&
              proc_stats.loader_proc_lookup_calls_total == 0 &&
              proc_stats.proc_lookup_calls == 0 &&
              proc_stats.proc_lookup_resolved == 0 &&
              proc_stats.proc_lookup_missing == 0;
          if (should_sample_callback_starvation_stall) {
            static std::atomic<uint64_t>
                callback_starvation_dispatch_stall_sample_last_ms{0};
            uint64_t sample_interval_ms = std::min<uint64_t>(
                static_cast<uint64_t>(
                    cvars::gpu_vblank_callback_startup_gate_log_interval_ms),
                50);
            uint64_t last_sample_ms =
                callback_starvation_dispatch_stall_sample_last_ms.load(
                    std::memory_order_relaxed);
            bool sample_due =
                last_sample_ms == 0 || sample_interval_ms == 0 ||
                now_ms < last_sample_ms ||
                now_ms - last_sample_ms >= sample_interval_ms;
            if (sample_due) {
              callback_starvation_dispatch_stall_sample_last_ms.store(
                  now_ms, std::memory_order_relaxed);
              emulator->LogMainThreadGpuDispatchStallSample(
                  "gpu-callback-starvation-stall", thunk_01C3, thunk_01D5,
                  thunk_025B);
            }
          }
          have_cached_dispatch_stall = emulator->GetLastGpuDispatchStallSnapshot(
              &cached_dispatch_stall_state, &cached_dispatch_stall_detail,
              &cached_dispatch_stall_chain, &cached_dispatch_stall_targets,
              &cached_dispatch_stall_repeat_samples,
              &cached_dispatch_stall_ms);
          if (have_cached_dispatch_stall && cached_dispatch_stall_ms &&
              now_ms >= cached_dispatch_stall_ms) {
            cached_dispatch_stall_age_ms = now_ms - cached_dispatch_stall_ms;
          }
        }
      }
      auto is_callback_starvation_dispatch_stall =
          [](const std::string& state) -> bool {
        return state == "startup_prolog_helper_only" ||
               state == "gpu_bootstrap_entry_loop_before_callback" ||
               state == "title_local_privilege_gate_before_gpu_dispatch" ||
               state == "title_local_busy_loop_without_import_dispatch" ||
               state == "title_local_busy_loop_before_gpu_dispatch" ||
               state == "busy_loop_before_gpu_dispatch";
      };
      auto callback_dispatch_stall_blocked_at =
          [](const std::string& state) -> const char* {
        if (state == "startup_prolog_helper_only") {
          return "startup_prolog_helper_only";
        }
        if (state == "gpu_bootstrap_entry_loop_before_callback") {
          return "gpu_bootstrap_entry_loop_before_callback";
        }
        if (state == "title_local_privilege_gate_before_gpu_dispatch") {
          return "title_local_privilege_gate_before_gpu_dispatch";
        }
        if (state == "title_local_busy_loop_without_import_dispatch") {
          return "title_local_busy_loop_without_import_dispatch";
        }
        if (state == "title_local_busy_loop_before_gpu_dispatch") {
          return "title_local_busy_loop_before_gpu_dispatch";
        }
        if (state == "busy_loop_before_gpu_dispatch") {
          return "busy_loop_before_gpu_dispatch";
        }
        return "interrupt_callback";
      };
      auto callback_dispatch_stall_summary =
          [](const std::string& state) -> const char* {
        if (state == "startup_prolog_helper_only") {
          return "guest main thread remains in same-module prolog helper "
                 "dispatch before any loader or GPU callback registration "
                 "path executes";
        }
        if (state == "gpu_bootstrap_entry_loop_before_callback") {
          return "guest main thread remains in the early GPU bootstrap entry "
                 "loop before callback registration";
        }
        if (state == "title_local_privilege_gate_before_gpu_dispatch") {
          return "guest main thread remains behind a title-local privilege "
                 "gate before any loader or GPU dispatch path reaches "
                 "callback registration";
        }
        if (state == "title_local_busy_loop_without_import_dispatch") {
          return "guest main thread remains in a title-local busy loop without "
                 "loader/import dispatch before callback registration";
        }
        if (state == "title_local_busy_loop_before_gpu_dispatch") {
          return "guest main thread remains in a title-local busy loop before "
                 "callback registration";
        }
        if (state == "busy_loop_before_gpu_dispatch") {
          return "guest main thread remains in a busy loop before callback "
                 "registration";
        }
        return "interrupt callback not registered after valid import probes "
               "and bootstrap activity";
      };
      bool cached_startup_prolog_helper_grace =
          IsStartupPrologHelperGraceWindow(cached_dispatch_stall_state,
                                           cached_dispatch_stall_age_ms,
                                           cached_dispatch_stall_repeat_samples);
      bool cached_callback_starvation_stall =
          have_cached_dispatch_stall &&
          !cached_startup_prolog_helper_grace &&
          is_callback_starvation_dispatch_stall(cached_dispatch_stall_state);
      bool cached_busy_loop_before_loader =
          have_cached_dispatch_stall &&
          (cached_dispatch_stall_state == "gpu_bootstrap_entry_loop_before_callback" ||
           cached_dispatch_stall_state ==
               "title_local_busy_loop_without_import_dispatch" ||
           cached_dispatch_stall_state ==
               "title_local_busy_loop_before_gpu_dispatch" ||
           cached_dispatch_stall_state == "busy_loop_before_gpu_dispatch");
      bool guest_dispatch_bridge_only_activity =
          proc_stats.guest_main_dispatch_bridge_calls_total != 0 &&
          vd_init_count == 0 && vd_cb_count == 0 && vd_swap_count == 0 &&
          mmio_base_guest == 0 && mmio_cntl_guest == 0 && mmio_wptr_guest == 0 &&
          runtime_01C2_total == 0 && runtime_01C3_total == 0 &&
          runtime_01D5_total == 0 && runtime_025B_total == 0 &&
          proc_stats.proc_lookup_calls == 0 &&
          proc_stats.proc_lookup_resolved == 0 &&
          proc_stats.proc_lookup_missing == 0;
      bool foundation_effect_01C3_seen =
          vd_init_count != 0 || mmio_base_guest != 0 || mmio_cntl_guest != 0 ||
          mmio_wptr_guest != 0;
      bool foundation_effect_01D5_seen =
          vd_cb_count != 0 || interrupt_callback_ != 0;
      bool foundation_effect_025B_seen =
          vd_swap_count != 0 || mmio_wptr_guest != 0;
      CriticalGpuOrdinalFoundationState foundation_01C3 =
          ClassifyCriticalGpuOrdinalFoundation(
              0x01C3, wired_01C3, probe_01C3_ready, proc_stats, stat_01C3,
              dispatch_evidence.evidence_case, foundation_effect_01C3_seen,
              callback_missing_stall_xex_privilege_branch_only);
      CriticalGpuOrdinalFoundationState foundation_01D5 =
          ClassifyCriticalGpuOrdinalFoundation(
              0x01D5, wired_01D5, probe_01D5_ready, proc_stats, stat_01D5,
              dispatch_evidence.evidence_case, foundation_effect_01D5_seen,
              callback_missing_stall_xex_privilege_branch_only);
      CriticalGpuOrdinalFoundationState foundation_025B =
          ClassifyCriticalGpuOrdinalFoundation(
              0x025B, wired_025B, probe_025B_ready, proc_stats, stat_025B,
              dispatch_evidence.evidence_case, foundation_effect_025B_seen,
              callback_missing_stall_xex_privilege_branch_only);
      auto select_foundation_summary_state =
          [&]() -> CriticalGpuOrdinalPipelineState {
        constexpr std::array<CriticalGpuOrdinalPipelineState, 14> kPriority = {
            CriticalGpuOrdinalPipelineState::kStaticImportWiringMissing,
            CriticalGpuOrdinalPipelineState::kStaticImportProbeInvalid,
            CriticalGpuOrdinalPipelineState::kAwaitingDispatchPath,
            CriticalGpuOrdinalPipelineState::
                kDispatchBridgeBeforePrivilegeExportEntry,
            CriticalGpuOrdinalPipelineState::
                kPrivilegeThunkBeforeExportEntry,
            CriticalGpuOrdinalPipelineState::
                kPrivilegeExportEntryBeforeLoaderOrStaticDispatch,
            CriticalGpuOrdinalPipelineState::
                kDispatchBridgeBeforeLoaderOrStaticDispatch,
            CriticalGpuOrdinalPipelineState::kLoaderOrLookupSeenButOrdinalIdle,
            CriticalGpuOrdinalPipelineState::kLookupPendingResolution,
            CriticalGpuOrdinalPipelineState::kLookupMissing,
            CriticalGpuOrdinalPipelineState::kLookupInvalidModule,
            CriticalGpuOrdinalPipelineState::
                kLookupResolvedWithoutRuntimeEntry,
            CriticalGpuOrdinalPipelineState::
                kThunkDispatchWithoutExportEntry,
            CriticalGpuOrdinalPipelineState::
                kExportEntryWithoutGuestEffect,
        };
        for (CriticalGpuOrdinalPipelineState candidate : kPriority) {
          if (foundation_01C3.state == candidate ||
              foundation_01D5.state == candidate ||
              foundation_025B.state == candidate) {
            return candidate;
          }
        }
        return CriticalGpuOrdinalPipelineState::kRuntimeValidated;
      };
      CriticalGpuOrdinalPipelineState foundation_summary_state =
          select_foundation_summary_state();
      const char* foundation_next_missing_transition =
          CriticalGpuOrdinalNextTransitionName(foundation_summary_state);
      bool critical_foundation_static_ready =
          probe_01C3_ready && probe_01D5_ready && probe_025B_ready;
      bool critical_foundation_runtime_valid = foundation_01C3.runtime_valid &&
                                               foundation_01D5.runtime_valid &&
                                               foundation_025B.runtime_valid;
      const char* dispatch_path_gate_blocked_at = "ready";
      std::string dispatch_path_gate_detail;
      switch (dispatch_evidence.evidence_case) {
        case DispatchPathEvidenceCase::kCriticalGpuRuntimeDispatch:
          dispatch_path_gate_detail =
              "critical GPU runtime dispatch observed via dynamic lookup, "
              "static thunk, or export entry";
          break;
        case DispatchPathEvidenceCase::kGuestMainDispatchBridge:
          dispatch_path_gate_blocked_at = "ready_via_guest_main_dispatch_bridge";
          dispatch_path_gate_detail = fmt::format(
              "guest main thread entered the dispatch bridge before tracked "
              "GPU ordinals (guest_main_dispatch_bridge_total={} "
              "critical_runtime_dispatch_total={} "
              "next_missing_transition={})",
              dispatch_evidence.guest_main_dispatch_bridge_total,
              dispatch_evidence.critical_runtime_dispatch_total,
              foundation_next_missing_transition);
          if (cached_callback_starvation_stall) {
            dispatch_path_gate_detail = fmt::format(
                "{}; cached_state={} cached_age_ms={} "
                "cached_repeat_samples={} cached_detail='{}'",
                dispatch_path_gate_detail, cached_dispatch_stall_state,
                cached_dispatch_stall_age_ms,
                cached_dispatch_stall_repeat_samples,
                cached_dispatch_stall_detail);
          }
          break;
        case DispatchPathEvidenceCase::kNone:
          dispatch_path_gate_detail =
              "critical GPU runtime dispatch observed via dynamic lookup, "
              "static thunk, or export entry";
          break;
      }
      if (!import_probe_gate_ready) {
        dispatch_path_gate_blocked_at = "import_probe_gate";
        dispatch_path_gate_detail = fmt::format(
            "{} ({})", import_probe_blocked_at, import_probe_detail);
      } else if (dispatch_evidence.evidence_case ==
                 DispatchPathEvidenceCase::kNone) {
        dispatch_path_gate_blocked_at = "critical_dispatch_paths";
        dispatch_path_gate_detail = fmt::format(
            "no dynamic XexGetProcedureAddress lookup, static sc2 thunk "
            "dispatch, or xboxkrnl_video export entry observed after valid "
            "critical import probes "
            "(01C3 dyn={} res={} miss={} inv={} fb={} thunk={} export={} "
            "01D5 dyn={} res={} miss={} inv={} fb={} thunk={} export={} "
            "025B dyn={} res={} miss={} inv={} fb={} thunk={} export={})",
            stat_01C3 ? stat_01C3->dynamic_lookup_calls : 0,
            stat_01C3 ? stat_01C3->dynamic_lookup_resolved_calls : 0,
            stat_01C3 ? stat_01C3->dynamic_lookup_missing_calls : 0,
            stat_01C3 ? stat_01C3->dynamic_lookup_invalid_module_calls : 0,
            stat_01C3 ? stat_01C3->dynamic_lookup_fallback_hits : 0,
            stat_01C3 ? stat_01C3->static_thunk_calls : 0,
            stat_01C3 ? stat_01C3->export_entry_calls : 0,
            stat_01D5 ? stat_01D5->dynamic_lookup_calls : 0,
            stat_01D5 ? stat_01D5->dynamic_lookup_resolved_calls : 0,
            stat_01D5 ? stat_01D5->dynamic_lookup_missing_calls : 0,
            stat_01D5 ? stat_01D5->dynamic_lookup_invalid_module_calls : 0,
            stat_01D5 ? stat_01D5->dynamic_lookup_fallback_hits : 0,
            stat_01D5 ? stat_01D5->static_thunk_calls : 0,
            stat_01D5 ? stat_01D5->export_entry_calls : 0,
            stat_025B ? stat_025B->dynamic_lookup_calls : 0,
            stat_025B ? stat_025B->dynamic_lookup_resolved_calls : 0,
            stat_025B ? stat_025B->dynamic_lookup_missing_calls : 0,
            stat_025B ? stat_025B->dynamic_lookup_invalid_module_calls : 0,
            stat_025B ? stat_025B->dynamic_lookup_fallback_hits : 0,
            stat_025B ? stat_025B->static_thunk_calls : 0,
            stat_025B ? stat_025B->export_entry_calls : 0);
        if (have_cached_dispatch_stall) {
          if (cached_dispatch_stall_state == "guest_main_context_not_ready") {
            dispatch_path_gate_blocked_at = "guest_main_context_not_ready";
          } else if (cached_dispatch_stall_state ==
                         "startup_prolog_helper_only" &&
                     !cached_startup_prolog_helper_grace) {
            dispatch_path_gate_blocked_at = "startup_prolog_helper_only";
          } else if (cached_dispatch_stall_state ==
                     "title_local_privilege_gate_before_gpu_dispatch") {
            dispatch_path_gate_blocked_at =
                "title_local_privilege_gate_before_gpu_dispatch";
          } else if (cached_dispatch_stall_state ==
                     "title_local_busy_loop_without_import_dispatch") {
            dispatch_path_gate_blocked_at =
                "title_local_busy_loop_without_import_dispatch";
          } else if (cached_dispatch_stall_state ==
                     "title_local_busy_loop_before_gpu_dispatch") {
            dispatch_path_gate_blocked_at =
                "title_local_busy_loop_before_gpu_dispatch";
          } else if (cached_dispatch_stall_state ==
                     "busy_loop_before_gpu_dispatch") {
            dispatch_path_gate_blocked_at = "busy_loop_before_gpu_dispatch";
          }
          dispatch_path_gate_detail = fmt::format(
              "{} cached_state={} cached_repeat_samples={} cached_age_ms={} "
              "cached_detail='{}' cached_chain='{}' cached_targets='{}'",
              dispatch_path_gate_detail, cached_dispatch_stall_state,
              cached_dispatch_stall_repeat_samples,
              cached_dispatch_stall_age_ms, cached_dispatch_stall_detail,
              cached_dispatch_stall_chain.empty()
                  ? "<none>"
                  : cached_dispatch_stall_chain,
              cached_dispatch_stall_targets.empty()
                  ? "<none>"
                  : cached_dispatch_stall_targets);
        }
      }
      XELOGW(
          "RING BUFFER: dispatch path gate {} (blocked_at={} detail='{}' "
          "import_probe_gate_ready={} dispatch_evidence={} "
          "guest_main_dispatch_bridge_total={} runtime_01C3={} "
          "runtime_01D5={} runtime_025B={})",
          dispatch_path_gate_ready ? "READY" : "WAITING",
          dispatch_path_gate_blocked_at, dispatch_path_gate_detail,
          import_probe_gate_ready ? "YES" : "NO",
          DispatchPathEvidenceCaseName(dispatch_evidence.evidence_case),
          dispatch_evidence.guest_main_dispatch_bridge_total, runtime_01C3_total,
          runtime_01D5_total, runtime_025B_total);
      kernel::object_ref<kernel::XModule> xboxkrnl_module;
      if (kernel_state_) {
        xboxkrnl_module = kernel_state_->GetModule("xboxkrnl.exe");
      }
      bool executable_xex_ready =
          executable_module && executable_module->xex_module();
      bool executable_import_libraries_ready =
          executable_xex_ready &&
          executable_module->xex_module()->import_libraries() != nullptr;
      bool xex_getproc_env_ready =
          kernel_state_ && executable_module && executable_xex_ready &&
          executable_import_libraries_ready && xboxkrnl_module;
      bool xex_loader_guest_api_seen =
          proc_stats.loader_module_handle_calls_total != 0 ||
          proc_stats.loader_proc_lookup_calls_total != 0;
      bool guest_main_dispatch_bridge_seen =
          proc_stats.guest_main_dispatch_bridge_calls_total != 0;
      bool xex_getproc_guest_lookup_seen =
          proc_stats.proc_lookup_calls != 0 || runtime_01C3_total != 0 ||
          runtime_01D5_total != 0 || runtime_025B_total != 0;
      bool cached_pre_gpu_privilege_gate =
          have_cached_dispatch_stall &&
          cached_dispatch_stall_state ==
              "title_local_privilege_gate_before_gpu_dispatch";
      bool cached_privilege_scan_seen =
          cached_pre_gpu_privilege_gate &&
          cached_dispatch_stall_targets.find("xex_privilege_check=YES") !=
              std::string::npos;
      bool cached_privilege_scan_without_call =
          cached_privilege_scan_seen &&
          proc_stats.xex_check_executable_privilege_calls == 0;
      const char* xex_getproc_blocked_at = "ready";
      std::string xex_getproc_detail =
          "dynamic lookup environment is ready";
      if (!kernel_state_) {
        xex_getproc_blocked_at = "kernel_state";
        xex_getproc_detail = "kernel_state_ is null";
      } else if (!executable_module) {
        xex_getproc_blocked_at = "executable_module";
        xex_getproc_detail = "executable module is not visible yet";
      } else if (!executable_xex_ready) {
        xex_getproc_blocked_at = "executable_xex";
        xex_getproc_detail = "executable module has no xex_module";
      } else if (!executable_import_libraries_ready) {
        xex_getproc_blocked_at = "import_libraries";
        xex_getproc_detail = "executable xex has no import library table";
      } else if (!xboxkrnl_module) {
        xex_getproc_blocked_at = "xboxkrnl_module";
        xex_getproc_detail = "xboxkrnl.exe module is not registered";
      } else if (!xex_loader_guest_api_seen && cached_pre_gpu_privilege_gate) {
        xex_getproc_blocked_at = "title_local_privilege_gate_before_gpu_dispatch";
        if (cached_privilege_scan_without_call) {
          if (xexcheck_runtime_state ==
              XexCheckExecutablePrivilegeRuntimeState::
                  kStaticThunkWithoutExportEntry) {
            xex_getproc_detail = fmt::format(
                "dynamic lookup environment is ready, but guest-main dispatch "
                "is still stalled after calling the static thunk for "
                "XexCheckExecutablePrivilege {} time(s); the export body has "
                "not been entered yet and no loader or GPU path executes "
                "(cached_detail='{}')",
                proc_stats.xex_check_executable_privilege_static_thunk_calls,
                cached_dispatch_stall_detail);
          } else {
            xex_getproc_detail = fmt::format(
                "dynamic lookup environment is ready, but guest-main dispatch "
                "is still stalled before entering XexCheckExecutablePrivilege "
                "itself; bounded branch scans see it as the next kernel-export "
                "path before any loader or GPU path executes "
                "(cached_detail='{}')",
                cached_dispatch_stall_detail);
          }
        } else {
          xex_getproc_detail = fmt::format(
              "dynamic lookup environment is ready, but guest-main dispatch is "
              "still stalled behind a title-local privilege gate before any "
              "loader or GPU path executes (cached_detail='{}')",
              cached_dispatch_stall_detail);
        }
      } else if (!xex_loader_guest_api_seen && cached_busy_loop_before_loader) {
        xex_getproc_blocked_at =
            cached_dispatch_stall_state ==
                    "gpu_bootstrap_entry_loop_before_callback"
                ? "guest_bootstrap_entry_loop_before_loader"
                : "guest_busy_loop_before_loader";
        xex_getproc_detail = fmt::format(
            "guest main thread entered the dispatch bridge {} time(s), but "
            "remains in {} before any loader or tracked GPU path executes "
            "(cached_age_ms={} cached_repeat_samples={} cached_detail='{}')",
            proc_stats.guest_main_dispatch_bridge_calls_total,
            cached_dispatch_stall_state, cached_dispatch_stall_age_ms,
            cached_dispatch_stall_repeat_samples,
            cached_dispatch_stall_detail);
      } else if (!xex_loader_guest_api_seen && guest_main_dispatch_bridge_seen) {
        xex_getproc_blocked_at = "guest_dispatch_bridge_before_loader";
        xex_getproc_detail = fmt::format(
            "guest main thread entered the dispatch bridge {} time(s), but "
            "the title has not entered XexGetModuleHandle/"
            "XexGetProcedureAddress yet",
            proc_stats.guest_main_dispatch_bridge_calls_total);
      } else if (!xex_loader_guest_api_seen) {
        xex_getproc_blocked_at = "guest_loader_call_absent";
        xex_getproc_detail =
            "dynamic lookup environment is ready, but the guest has not "
            "entered XexGetModuleHandle/XexGetProcedureAddress yet";
      } else if (!xex_getproc_guest_lookup_seen) {
        xex_getproc_blocked_at = "critical_gpu_call_absent";
        xex_getproc_detail =
            "guest entered the loader API path, but no critical GPU export "
            "lookup or tracked runtime dispatch has occurred yet";
      } else if (proc_stats.proc_lookup_missing != 0) {
        xex_getproc_blocked_at = "guest_lookup_missing";
        xex_getproc_detail =
            "guest attempted XexGetProcedureAddress, but at least one lookup "
            "missed the critical GPU exports";
      } else if (proc_stats.proc_lookup_invalid_module != 0) {
        xex_getproc_blocked_at = "invalid_module_handle";
        xex_getproc_detail =
            "guest attempted XexGetProcedureAddress with an invalid module "
            "handle";
      }
      XELOGW(
          "RING BUFFER: XexGetProcedureAddress status env_ready={} "
          "guest_loader_api_seen={} guest_dispatch_bridge_seen={} "
          "critical_gpu_lookup_seen={} "
          "blocked_at={} detail='{}' "
          "executable_present={} executable_hmodule={:08X} executable_xex={} "
          "import_libraries={} xboxkrnl_present={} xboxkrnl_hmodule={:08X} "
          "critical_imports={} loader_module_handle_calls_total={} "
          "loader_proc_lookup_calls_total={} "
          "guest_main_dispatch_bridge_calls_total={} module_handle_calls={} "
          "module_handle_failures={} proc_lookup_calls={} "
          "proc_lookup_resolved={} proc_lookup_missing={} "
          "proc_lookup_invalid_module={} proc_lookup_fallback_hits={} "
          "xex_check_privilege_static_thunk_calls={} "
          "xex_check_privilege_export_entry_calls={} "
          "xex_check_runtime_state={}",
          xex_getproc_env_ready ? "YES" : "NO",
          xex_loader_guest_api_seen ? "YES" : "NO",
          guest_main_dispatch_bridge_seen ? "YES" : "NO",
          xex_getproc_guest_lookup_seen ? "YES" : "NO",
          xex_getproc_blocked_at, xex_getproc_detail,
          executable_module ? "YES" : "NO",
          executable_module ? executable_module->hmodule_ptr() : 0,
          executable_xex_ready ? "YES" : "NO",
          executable_import_libraries_ready ? "YES" : "NO",
          xboxkrnl_module ? "YES" : "NO",
          xboxkrnl_module ? xboxkrnl_module->hmodule_ptr() : 0,
          (wired_01C3 && wired_01D5 && wired_025B) ? "YES" : "NO",
          proc_stats.loader_module_handle_calls_total,
          proc_stats.loader_proc_lookup_calls_total,
          proc_stats.guest_main_dispatch_bridge_calls_total,
          proc_stats.module_handle_calls, proc_stats.module_handle_failures,
          proc_stats.proc_lookup_calls, proc_stats.proc_lookup_resolved,
          proc_stats.proc_lookup_missing, proc_stats.proc_lookup_invalid_module,
          proc_stats.proc_lookup_fallback_hits,
          proc_stats.xex_check_executable_privilege_static_thunk_calls,
          proc_stats.xex_check_executable_privilege_export_entry_calls,
          XexCheckExecutablePrivilegeRuntimeStateName(xexcheck_runtime_state));
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
      auto get_function_export = [](const cpu::Function* function)
          -> const cpu::Export* {
        auto* guest_function =
            dynamic_cast<const cpu::GuestFunction*>(function);
        return guest_function ? guest_function->export_data() : nullptr;
      };
      struct XexLoaderApiImportProbe {
        const char* api_name;
        bool static_imported;
        uint16_t ordinal;
        uint32_t value_addr;
        uint32_t thunk_addr;
        bool value_committed;
        bool value_translated;
        uint32_t value_word;
        bool thunk_committed;
        bool thunk_translated;
        uint32_t thunk_w0;
        uint32_t thunk_w1;
        bool thunk_sc2_stub;
        const char* value_fn_behavior;
        int value_fn_status;
        const char* thunk_fn_behavior;
        int thunk_fn_status;
        uint32_t module_proc;
        bool same_export;
        bool probe_valid;
      };
      std::array<XexLoaderApiImportProbe, 2> xex_loader_api_probes = {{
          {"XexGetModuleHandle", false, 0, 0, 0, false, false, 0, false,
           false, 0, 0, false, "Missing", -1, "Missing", -1, 0, false,
           false},
          {"XexGetProcedureAddress", false, 0, 0, 0, false, false, 0, false,
           false, 0, 0, false, "Missing", -1, "Missing", -1, 0, false,
           false},
      }};
      XexLoaderApiImportProbe xex_check_privilege_import_probe = {
          "XexCheckExecutablePrivilege", false, 0, 0, 0, false, false, 0,
          false, false, 0, 0, false, "Missing", -1, "Missing", -1, 0, false,
          false};
      auto* xex_loader_resolver =
          processor_ ? processor_->export_resolver() : nullptr;
      auto* xex_loader_memory = kernel_state_ ? kernel_state_->memory() : nullptr;
      if (executable_import_libraries_ready && xex_loader_resolver) {
        const auto* import_libs =
            executable_module->xex_module()->import_libraries();
        if (import_libs) {
          for (const auto& lib : *import_libs) {
            if (lib.name != "xboxkrnl" && lib.name != "xboxkrnl.exe") {
              continue;
            }
            for (const auto& import_fn : lib.imports) {
              auto* export_entry = xex_loader_resolver->GetExportByOrdinal(
                  "xboxkrnl.exe", import_fn.ordinal);
              if (!export_entry || !export_entry->name) {
                continue;
              }
              std::string_view export_name(export_entry->name);
              if (export_name == xex_check_privilege_import_probe.api_name) {
                xex_check_privilege_import_probe.static_imported = true;
                xex_check_privilege_import_probe.ordinal = import_fn.ordinal;
                xex_check_privilege_import_probe.value_addr =
                    import_fn.value_address;
                xex_check_privilege_import_probe.thunk_addr =
                    import_fn.thunk_address;
                continue;
              }
              for (auto& probe : xex_loader_api_probes) {
                if (export_name != probe.api_name) {
                  continue;
                }
                probe.static_imported = true;
                probe.ordinal = import_fn.ordinal;
                probe.value_addr = import_fn.value_address;
                probe.thunk_addr = import_fn.thunk_address;
              }
            }
          }
        }
      }
      auto finalize_import_probe = [&](XexLoaderApiImportProbe* probe) {
        if (!probe->static_imported) {
          return;
        }
        ReadGuestWordSnapshot(xex_loader_memory, probe->value_addr,
                              &probe->value_word, &probe->value_committed,
                              &probe->value_translated);
        ReadGuestWordSnapshot(xex_loader_memory, probe->thunk_addr,
                              &probe->thunk_w0, &probe->thunk_committed,
                              &probe->thunk_translated);
        if (xex_loader_memory && probe->thunk_addr) {
          auto thunk_safe = xex_loader_memory->TranslateVirtualSafe<uint32_t*>(
              probe->thunk_addr);
          if (thunk_safe.success && thunk_safe.pointer) {
            probe->thunk_w0 = xe::load_and_swap<uint32_t>(thunk_safe.pointer);
            probe->thunk_w1 = xe::load_and_swap<uint32_t>(thunk_safe.pointer + 1);
            probe->thunk_sc2_stub =
                probe->thunk_w0 == 0x44000042 && probe->thunk_w1 == 0x4E800020;
          }
        }
        auto* value_fn =
            (processor_ && probe->value_word)
                ? processor_->LookupFunction(probe->value_word)
                : nullptr;
        auto* thunk_fn =
            (processor_ && probe->thunk_addr)
                ? processor_->LookupFunction(probe->thunk_addr)
                : nullptr;
        probe->value_fn_behavior = describe_function_behavior(value_fn);
        probe->value_fn_status =
            value_fn ? static_cast<int>(value_fn->status()) : -1;
        probe->thunk_fn_behavior = describe_function_behavior(thunk_fn);
        probe->thunk_fn_status =
            thunk_fn ? static_cast<int>(thunk_fn->status()) : -1;
        probe->module_proc =
            xboxkrnl_module
                ? xboxkrnl_module->GetProcAddressByOrdinal(probe->ordinal)
                : 0;
        auto* module_fn =
            probe->module_proc ? processor_->LookupFunction(probe->module_proc)
                               : nullptr;
        auto* module_export = get_function_export(module_fn);
        auto* thunk_export = get_function_export(thunk_fn);
        probe->same_export = module_export && thunk_export &&
                             module_export->ordinal == thunk_export->ordinal;
        probe->probe_valid =
            probe->value_addr != 0 && probe->thunk_addr != 0 &&
            probe->value_translated && probe->thunk_translated &&
            probe->value_word == probe->thunk_addr && probe->thunk_sc2_stub &&
            value_fn &&
            value_fn->behavior() == cpu::Function::Behavior::kExtern &&
            thunk_fn &&
            thunk_fn->behavior() == cpu::Function::Behavior::kExtern &&
            probe->module_proc != 0 && probe->same_export;
      };
      for (auto& probe : xex_loader_api_probes) {
        finalize_import_probe(&probe);
      }
      finalize_import_probe(&xex_check_privilege_import_probe);
      bool xex_loader_imports_present = false;
      bool xex_loader_import_probes_valid = true;
      const char* xex_loader_import_invalid_name = nullptr;
      for (const auto& probe : xex_loader_api_probes) {
        if (!probe.static_imported) {
          continue;
        }
        xex_loader_imports_present = true;
        if (!probe.probe_valid && !xex_loader_import_invalid_name) {
          xex_loader_import_invalid_name = probe.api_name;
        }
        xex_loader_import_probes_valid =
            xex_loader_import_probes_valid && probe.probe_valid;
      }
      bool xex_check_privilege_import_present =
          xex_check_privilege_import_probe.static_imported;
      bool xex_check_privilege_import_probe_valid =
          xex_check_privilege_import_present &&
          xex_check_privilege_import_probe.probe_valid;
      const char* xex_getproc_health_state = "ready";
      std::string xex_getproc_health_detail =
          "dynamic loader environment and probes are healthy";
      if (!xex_getproc_env_ready) {
        xex_getproc_health_state = "env_not_ready";
        xex_getproc_health_detail = xex_getproc_detail;
      } else if (!xex_loader_imports_present) {
        xex_getproc_health_state = "title_has_no_static_loader_imports";
        xex_getproc_health_detail =
            "executable has no static imports for XexGetModuleHandle or "
            "XexGetProcedureAddress; dynamic loader API path is not present "
            "in the title import surface";
      } else if (!xex_loader_import_probes_valid) {
        xex_getproc_health_state = "loader_import_probe_invalid";
        xex_getproc_health_detail = fmt::format(
            "static loader import probe for {} is not valid",
            xex_loader_import_invalid_name ? xex_loader_import_invalid_name
                                           : "<unknown>");
      } else if (!xex_loader_guest_api_seen && cached_pre_gpu_privilege_gate) {
        xex_getproc_health_state = "title_pre_gpu_privilege_gate";
        if (cached_privilege_scan_without_call) {
          if (!xex_check_privilege_import_present) {
            xex_getproc_health_detail = fmt::format(
                "guest-main dispatch analysis shows a title-local privilege "
                "gate before any XexGetModuleHandle/XexGetProcedureAddress or "
                "GPU dispatch path executes; bounded branch scans already see "
                "XexCheckExecutablePrivilege, but the title has no static "
                "import surface for ordinal 0x194 (cached_detail='{}')",
                cached_dispatch_stall_detail);
          } else if (!xex_check_privilege_import_probe_valid) {
            xex_getproc_health_detail = fmt::format(
                "guest-main dispatch analysis shows a title-local privilege "
                "gate before any XexGetModuleHandle/XexGetProcedureAddress or "
                "GPU dispatch path executes; bounded branch scans already see "
                "XexCheckExecutablePrivilege, but the static import probe for "
                "ordinal 0x194 is not valid yet (cached_detail='{}')",
                cached_dispatch_stall_detail);
          } else if (xexcheck_runtime_state ==
                     XexCheckExecutablePrivilegeRuntimeState::
                         kStaticThunkWithoutExportEntry) {
            xex_getproc_health_detail = fmt::format(
                "guest-main dispatch analysis shows a title-local privilege "
                "gate before any XexGetModuleHandle/XexGetProcedureAddress or "
                "GPU dispatch path executes; bounded branch scans already see "
                "XexCheckExecutablePrivilege, the static import probe for "
                "ordinal 0x194 is valid, and the static thunk executed {} "
                "time(s), but the export body has not been entered yet "
                "(cached_detail='{}')",
                proc_stats.xex_check_executable_privilege_static_thunk_calls,
                cached_dispatch_stall_detail);
          } else {
            xex_getproc_health_detail = fmt::format(
                "guest-main dispatch analysis shows a title-local privilege "
                "gate before any XexGetModuleHandle/XexGetProcedureAddress or "
                "GPU dispatch path executes; bounded branch scans already see "
                "XexCheckExecutablePrivilege, the static import probe for "
                "ordinal 0x194 is valid, but neither the static thunk nor the "
                "export body has been observed yet (cached_detail='{}')",
                cached_dispatch_stall_detail);
          }
        } else {
          xex_getproc_health_detail = fmt::format(
              "guest-main dispatch analysis shows a title-local privilege gate "
              "before any XexGetModuleHandle/XexGetProcedureAddress or GPU "
              "dispatch path executes (cached_detail='{}')",
              cached_dispatch_stall_detail);
        }
      } else if (!xex_loader_guest_api_seen && cached_busy_loop_before_loader) {
        xex_getproc_health_state =
            cached_dispatch_stall_state ==
                    "gpu_bootstrap_entry_loop_before_callback"
                ? "guest_bootstrap_entry_loop_before_loader"
                : "guest_busy_loop_before_loader";
        xex_getproc_health_detail = fmt::format(
            "guest main thread entered the dispatch bridge {} time(s), but "
            "remains in {} before any loader API or tracked GPU export path "
            "executes (cached_age_ms={} cached_repeat_samples={} "
            "cached_detail='{}')",
            proc_stats.guest_main_dispatch_bridge_calls_total,
            cached_dispatch_stall_state, cached_dispatch_stall_age_ms,
            cached_dispatch_stall_repeat_samples,
            cached_dispatch_stall_detail);
      } else if (!xex_loader_guest_api_seen &&
                 proc_stats.xex_check_executable_privilege_calls != 0) {
        xex_getproc_health_state = "title_pre_gpu_privilege_gate";
        xex_getproc_health_detail = fmt::format(
            "title has not entered XexGetModuleHandle/XexGetProcedureAddress; "
            "XexCheckExecutablePrivilege executed {} time(s) before any "
            "critical GPU dispatch path (true={} false={} "
            "last_privilege={} last_flags={:08X} last_result={})",
            proc_stats.xex_check_executable_privilege_calls,
            proc_stats.xex_check_executable_privilege_true,
            proc_stats.xex_check_executable_privilege_false,
            proc_stats.xex_check_executable_privilege_last_privilege,
            proc_stats.xex_check_executable_privilege_last_flags,
            proc_stats.xex_check_executable_privilege_last_result);
      } else if (!xex_loader_guest_api_seen && guest_main_dispatch_bridge_seen) {
        xex_getproc_health_state = "guest_dispatch_bridge_before_loader";
        xex_getproc_health_detail = fmt::format(
            "guest main thread entered the dispatch bridge {} time(s), but "
            "the title has not entered XexGetModuleHandle/"
            "XexGetProcedureAddress or tracked GPU exports yet",
            proc_stats.guest_main_dispatch_bridge_calls_total);
      } else if (!xex_loader_guest_api_seen) {
        xex_getproc_health_state = "host_loader_ready_guest_never_called";
        xex_getproc_health_detail =
            "title statically imports XexGetModuleHandle/XexGetProcedureAddress "
            "and host probes resolve them, but the guest never entered the "
            "loader APIs";
      } else if (!xex_getproc_guest_lookup_seen) {
        xex_getproc_health_state = "guest_loader_active_non_gpu_only";
        xex_getproc_health_detail = fmt::format(
            "guest has entered the loader API path "
            "(module_handle_calls_total={} proc_lookup_calls_total={}), but "
            "has not looked up tracked GPU exports yet",
            proc_stats.loader_module_handle_calls_total,
            proc_stats.loader_proc_lookup_calls_total);
      } else if (proc_stats.module_handle_failures != 0) {
        xex_getproc_health_state = "guest_module_handle_failed";
        xex_getproc_health_detail =
            "guest attempted XexGetModuleHandle, but at least one module "
            "handle lookup failed";
      } else if (proc_stats.proc_lookup_missing != 0) {
        xex_getproc_health_state = "guest_lookup_missing";
        xex_getproc_health_detail =
            "guest attempted XexGetProcedureAddress, but at least one "
            "critical GPU export lookup missed";
      } else if (proc_stats.proc_lookup_invalid_module != 0) {
        xex_getproc_health_state = "guest_invalid_module";
        xex_getproc_health_detail =
            "guest attempted XexGetProcedureAddress with an invalid module "
            "handle";
      } else {
        xex_getproc_health_state = "guest_loader_active";
        xex_getproc_health_detail =
            "guest has entered the loader API path for critical GPU ordinals";
      }
      XELOGW(
          "RING BUFFER: XexGetProcedureAddress health state={} detail='{}' "
          "static_getmodule_import={} static_getproc_import={} "
          "static_xexcheck_import={} xexcheck_import_probe_valid={} "
          "loader_import_probes_valid={} guest_loader_api_seen={} "
          "guest_dispatch_bridge_seen={} critical_gpu_lookup_seen={} "
          "host_self_test_ready={} loader_module_handle_calls_total={} "
          "loader_proc_lookup_calls_total={} "
          "guest_main_dispatch_bridge_calls_total={} module_handle_calls={} "
          "proc_lookup_calls={} "
          "xex_check_privilege_static_thunk_calls={} "
          "xex_check_privilege_export_entry_calls={} "
          "xex_check_runtime_state={} "
          "xex_check_privilege_calls={} xex_check_privilege_true={} "
          "xex_check_privilege_false={} last_privilege={} last_flags={:08X} "
          "last_result={} xex_check_branch_scan_seen={} "
          "xex_check_branch_only={}",
          xex_getproc_health_state, xex_getproc_health_detail,
          xex_loader_api_probes[0].static_imported ? "YES" : "NO",
          xex_loader_api_probes[1].static_imported ? "YES" : "NO",
          xex_check_privilege_import_present ? "YES" : "NO",
          xex_check_privilege_import_probe_valid ? "YES" : "NO",
          xex_loader_import_probes_valid ? "YES" : "NO",
          xex_loader_guest_api_seen ? "YES" : "NO",
          guest_main_dispatch_bridge_seen ? "YES" : "NO",
          xex_getproc_guest_lookup_seen ? "YES" : "NO",
          xex_getproc_env_ready ? "YES" : "NO",
          proc_stats.loader_module_handle_calls_total,
          proc_stats.loader_proc_lookup_calls_total,
          proc_stats.guest_main_dispatch_bridge_calls_total,
          proc_stats.module_handle_calls, proc_stats.proc_lookup_calls,
          proc_stats.xex_check_executable_privilege_static_thunk_calls,
          proc_stats.xex_check_executable_privilege_export_entry_calls,
          XexCheckExecutablePrivilegeRuntimeStateName(xexcheck_runtime_state),
          proc_stats.xex_check_executable_privilege_calls,
          proc_stats.xex_check_executable_privilege_true,
          proc_stats.xex_check_executable_privilege_false,
          proc_stats.xex_check_executable_privilege_last_privilege,
          proc_stats.xex_check_executable_privilege_last_flags,
          proc_stats.xex_check_executable_privilege_last_result,
          cached_privilege_scan_seen ? "YES" : "NO",
          cached_privilege_scan_without_call ? "YES" : "NO");
      XELOGW(
          "RING BUFFER: title pre-gpu kernel health "
          "xex_check_privilege_calls={} true={} false={} "
          "static_thunk_calls={} export_entry_calls={} "
          "runtime_state={} "
          "last_privilege={} last_flags={:08X} last_result={} "
          "guest_loader_api_seen={} xex_check_branch_scan_seen={} "
          "xex_check_branch_only={} static_xexcheck_import={} "
          "xexcheck_import_probe_valid={}",
          proc_stats.xex_check_executable_privilege_calls,
          proc_stats.xex_check_executable_privilege_true,
          proc_stats.xex_check_executable_privilege_false,
          proc_stats.xex_check_executable_privilege_static_thunk_calls,
          proc_stats.xex_check_executable_privilege_export_entry_calls,
          XexCheckExecutablePrivilegeRuntimeStateName(xexcheck_runtime_state),
          proc_stats.xex_check_executable_privilege_last_privilege,
          proc_stats.xex_check_executable_privilege_last_flags,
          proc_stats.xex_check_executable_privilege_last_result,
          xex_loader_guest_api_seen ? "YES" : "NO",
          cached_privilege_scan_seen ? "YES" : "NO",
          cached_privilege_scan_without_call ? "YES" : "NO",
          xex_check_privilege_import_present ? "YES" : "NO",
          xex_check_privilege_import_probe_valid ? "YES" : "NO");
      XELOGW(
          "RING BUFFER: title pre-gpu import name={} ordinal=0x{:03X} "
          "static_imported={} value_addr={:08X} thunk_addr={:08X} "
          "value_word={:08X} thunk_sc2_stub={} value_fn_behavior={} "
          "value_fn_status={} thunk_fn_behavior={} thunk_fn_status={} "
          "module_proc={:08X} same_export={} probe_valid={}",
          xex_check_privilege_import_probe.api_name,
          xex_check_privilege_import_probe.ordinal,
          xex_check_privilege_import_present ? "YES" : "NO",
          xex_check_privilege_import_probe.value_addr,
          xex_check_privilege_import_probe.thunk_addr,
          xex_check_privilege_import_probe.value_word,
          xex_check_privilege_import_probe.thunk_sc2_stub ? "YES" : "NO",
          xex_check_privilege_import_probe.value_fn_behavior,
          xex_check_privilege_import_probe.value_fn_status,
          xex_check_privilege_import_probe.thunk_fn_behavior,
          xex_check_privilege_import_probe.thunk_fn_status,
          xex_check_privilege_import_probe.module_proc,
          xex_check_privilege_import_probe.same_export ? "YES" : "NO",
          xex_check_privilege_import_probe_valid ? "YES" : "NO");
      XELOGW(
          "RING BUFFER: title pre-gpu runtime name=XexCheckExecutablePrivilege "
          "ordinal=0x194 static_thunk_calls={} export_entry_calls={} "
          "call_count={} runtime_state={}",
          proc_stats.xex_check_executable_privilege_static_thunk_calls,
          proc_stats.xex_check_executable_privilege_export_entry_calls,
          proc_stats.xex_check_executable_privilege_calls,
          XexCheckExecutablePrivilegeRuntimeStateName(xexcheck_runtime_state));
      for (const auto& probe : xex_loader_api_probes) {
        XELOGW(
            "RING BUFFER: XexGetProcedureAddress loader import name={} "
            "ordinal=0x{:03X} static_imported={} value_addr={:08X} "
            "thunk_addr={:08X} value_word={:08X} thunk_sc2_stub={} "
            "value_fn_behavior={} value_fn_status={} thunk_fn_behavior={} "
            "thunk_fn_status={} module_proc={:08X} same_export={} "
            "probe_valid={}",
            probe.api_name, probe.ordinal,
            probe.static_imported ? "YES" : "NO", probe.value_addr,
            probe.thunk_addr, probe.value_word,
            probe.thunk_sc2_stub ? "YES" : "NO", probe.value_fn_behavior,
            probe.value_fn_status, probe.thunk_fn_behavior,
            probe.thunk_fn_status, probe.module_proc,
            probe.same_export ? "YES" : "NO",
            probe.probe_valid ? "YES" : "NO");
      }
      struct XexGetProcSelfTestProbe {
        uint16_t ordinal;
        const char* name;
        uint32_t static_thunk;
        bool export_present;
        bool export_implemented;
        uint32_t module_proc;
        bool same_entry_addr;
        bool same_export;
        uint16_t module_export_ordinal;
        uint16_t static_export_ordinal;
        const char* proc_behavior;
        int proc_status;
      };
      std::array<XexGetProcSelfTestProbe, 3> xex_getproc_self_tests = {{
          {0x01C3, "VdInitializeRingBuffer", thunk_01C3, false, false, 0,
           false, false, 0, 0, "Missing", -1},
          {0x01D5, "VdSetGraphicsInterruptCallback", thunk_01D5, false, false,
           0, false, false, 0, 0, "Missing", -1},
          {0x025B, "VdSwap", thunk_025B, false, false, 0, false, false, 0, 0,
           "Missing", -1},
      }};
      auto* xex_getproc_resolver =
          processor_ ? processor_->export_resolver() : nullptr;
      bool xex_getproc_self_test_ready =
          xex_getproc_env_ready && xex_getproc_resolver && processor_;
      const char* xex_getproc_self_test_blocked_at = "ready";
      std::string xex_getproc_self_test_detail =
          "host self-test resolves critical GPU ordinals through xboxkrnl.exe";
      if (!xex_getproc_env_ready) {
        xex_getproc_self_test_blocked_at = xex_getproc_blocked_at;
        xex_getproc_self_test_detail = xex_getproc_detail;
        xex_getproc_self_test_ready = false;
      } else if (!processor_) {
        xex_getproc_self_test_blocked_at = "processor";
        xex_getproc_self_test_detail = "processor_ is null";
        xex_getproc_self_test_ready = false;
      } else if (!xex_getproc_resolver) {
        xex_getproc_self_test_blocked_at = "export_resolver";
        xex_getproc_self_test_detail = "processor export resolver is null";
        xex_getproc_self_test_ready = false;
      } else {
        for (auto& self_test : xex_getproc_self_tests) {
          auto* export_entry =
              xex_getproc_resolver->GetExportByOrdinal("xboxkrnl.exe",
                                                       self_test.ordinal);
          self_test.export_present = export_entry != nullptr;
          self_test.export_implemented =
              export_entry && export_entry->is_implemented();
          self_test.module_proc = xboxkrnl_module
                                      ? xboxkrnl_module->GetProcAddressByOrdinal(
                                            self_test.ordinal)
                                      : 0;
          auto* proc_fn =
              self_test.module_proc ? processor_->LookupFunction(self_test.module_proc)
                                    : nullptr;
          self_test.proc_behavior = describe_function_behavior(proc_fn);
          self_test.proc_status = proc_fn ? static_cast<int>(proc_fn->status())
                                          : -1;
          auto* proc_export = get_function_export(proc_fn);
          self_test.module_export_ordinal =
              proc_export ? proc_export->ordinal : 0;
          auto* static_thunk_fn =
              self_test.static_thunk
                  ? processor_->LookupFunction(self_test.static_thunk)
                  : nullptr;
          auto* static_thunk_export = get_function_export(static_thunk_fn);
          self_test.static_export_ordinal =
              static_thunk_export ? static_thunk_export->ordinal : 0;
          self_test.same_entry_addr =
              self_test.static_thunk != 0 &&
              self_test.module_proc == self_test.static_thunk;
          self_test.same_export =
              proc_export && static_thunk_export &&
              proc_export->ordinal == static_thunk_export->ordinal;
          if (!self_test.export_present) {
            if (xex_getproc_self_test_ready) {
              xex_getproc_self_test_blocked_at = "export_missing";
              xex_getproc_self_test_detail = fmt::format(
                  "xboxkrnl.exe has no export entry for ordinal 0x{:03X} ({})",
                  self_test.ordinal, self_test.name);
              xex_getproc_self_test_ready = false;
            }
            continue;
          }
          if (!self_test.export_implemented) {
            if (xex_getproc_self_test_ready) {
              xex_getproc_self_test_blocked_at = "export_unimplemented";
              xex_getproc_self_test_detail = fmt::format(
                  "xboxkrnl.exe export 0x{:03X} ({}) is present but not "
                  "implemented",
                  self_test.ordinal, self_test.name);
              xex_getproc_self_test_ready = false;
            }
            continue;
          }
          if (!self_test.module_proc) {
            if (xex_getproc_self_test_ready) {
              xex_getproc_self_test_blocked_at = "module_getproc";
              xex_getproc_self_test_detail = fmt::format(
                  "xboxkrnl.exe GetProcAddressByOrdinal failed for ordinal "
                  "0x{:03X} ({})",
                  self_test.ordinal, self_test.name);
              xex_getproc_self_test_ready = false;
            }
            continue;
          }
          if (!proc_fn) {
            if (xex_getproc_self_test_ready) {
              xex_getproc_self_test_blocked_at = "proc_lookup";
              xex_getproc_self_test_detail = fmt::format(
                  "processor lookup failed for xboxkrnl.exe ordinal 0x{:03X} "
                  "({}) guest proc {:08X}",
                  self_test.ordinal, self_test.name, self_test.module_proc);
              xex_getproc_self_test_ready = false;
            }
            continue;
          }
          if (proc_fn->behavior() != cpu::Function::Behavior::kExtern) {
            if (xex_getproc_self_test_ready) {
              xex_getproc_self_test_blocked_at = "proc_behavior";
              xex_getproc_self_test_detail = fmt::format(
                  "xboxkrnl.exe ordinal 0x{:03X} ({}) resolved to guest proc "
                  "{:08X}, but behavior is {} instead of Extern",
                  self_test.ordinal, self_test.name, self_test.module_proc,
                  self_test.proc_behavior);
              xex_getproc_self_test_ready = false;
            }
            continue;
          }
          if (self_test.static_thunk != 0 && !self_test.same_entry_addr &&
              !self_test.same_export) {
            if (xex_getproc_self_test_ready) {
              xex_getproc_self_test_blocked_at = "proc_export_mismatch";
              xex_getproc_self_test_detail = fmt::format(
                  "xboxkrnl.exe GetProcAddressByOrdinal returned {:08X} for "
                  "ordinal 0x{:03X} ({}), but it does not resolve to the same "
                  "GPU export as static thunk {:08X} "
                  "(module_export_ordinal=0x{:03X} static_export_ordinal=0x{:03X})",
                  self_test.module_proc, self_test.ordinal, self_test.name,
                  self_test.static_thunk, self_test.module_export_ordinal,
                  self_test.static_export_ordinal);
              xex_getproc_self_test_ready = false;
            }
          }
        }
      }
      XELOGW(
          "RING BUFFER: XexGetProcedureAddress self-test {} "
          "(blocked_at={} detail='{}' env_ready={} resolver_ready={} "
          "proc_01C3={:08X} proc_01D5={:08X} proc_025B={:08X})",
          xex_getproc_self_test_ready ? "READY" : "WAITING",
          xex_getproc_self_test_blocked_at, xex_getproc_self_test_detail,
          xex_getproc_env_ready ? "YES" : "NO",
          xex_getproc_resolver ? "YES" : "NO",
          xex_getproc_self_tests[0].module_proc,
          xex_getproc_self_tests[1].module_proc,
          xex_getproc_self_tests[2].module_proc);
      for (const auto& self_test : xex_getproc_self_tests) {
        XELOGW(
            "RING BUFFER: XexGetProcedureAddress self-test ordinal=0x{:03X} "
            "name={} export_present={} export_implemented={} module_proc={:08X} "
            "static_thunk={:08X} same_entry_addr={} same_export={} "
            "module_export_ordinal=0x{:03X} static_export_ordinal=0x{:03X} "
            "proc_behavior={} proc_status={}",
            self_test.ordinal, self_test.name,
            self_test.export_present ? "YES" : "NO",
            self_test.export_implemented ? "YES" : "NO",
            self_test.module_proc, self_test.static_thunk,
            self_test.same_entry_addr ? "YES" : "NO",
            self_test.same_export ? "YES" : "NO",
            self_test.module_export_ordinal, self_test.static_export_ordinal,
            self_test.proc_behavior, self_test.proc_status);
      }
      uint64_t swap_packets =
          command_processor_ ? command_processor_->swap_packet_count() : 0;
      uint64_t debug_host_swaps =
          bootstrap_state.debug_host_swap_injection_count;
      bool guest_swap_packet_seen = swap_packets > debug_host_swaps;
      bool bootstrap_activity_seen =
          vd_init_count != 0 || vd_cb_count != 0 || vd_swap_count != 0 ||
          mmio_base_guest != 0 || mmio_cntl_guest != 0 || mmio_wptr_guest != 0 ||
          proc_stats.guest_main_dispatch_bridge_calls_total != 0 ||
          runtime_01C2_total != 0 || runtime_01C3_total != 0 ||
          runtime_01D5_total != 0 || proc_stats.proc_lookup_calls != 0 ||
          proc_stats.proc_lookup_resolved != 0 ||
          proc_stats.proc_lookup_missing != 0;
      bool runtime_gpu_bootstrap_activity_seen =
          vd_init_count != 0 || vd_cb_count != 0 || vd_swap_count != 0 ||
          mmio_base_guest != 0 || mmio_cntl_guest != 0 || mmio_wptr_guest != 0 ||
          runtime_01C2_total != 0 || runtime_01C3_total != 0 ||
          runtime_01D5_total != 0 || runtime_025B_total != 0 ||
          proc_stats.proc_lookup_calls != 0 ||
          proc_stats.proc_lookup_resolved != 0 ||
          proc_stats.proc_lookup_missing != 0;
      bool callback_guest_registration_seen =
          vd_cb_count != 0 || runtime_01D5_total != 0;
      bool producer_entry_seen =
          vd_swap_count != 0 || runtime_025B_total != 0 ||
          mmio_wptr_guest != 0 || guest_swap_packet_seen;
      bool no_swap_hard_gate_ready = false;
      const char* no_swap_gate_blocked_at = "ready";
      std::string no_swap_gate_detail =
          "critical GPU startup prerequisites are satisfied";
      if (!import_probe_gate_ready) {
        no_swap_gate_blocked_at = "import_probe_gate";
        no_swap_gate_detail = fmt::format(
            "{} ({})", import_probe_blocked_at, import_probe_detail);
      } else if (!dispatch_path_gate_ready) {
        no_swap_gate_blocked_at = "dispatch_path_gate";
        no_swap_gate_detail = dispatch_path_gate_detail;
      } else if (!bootstrap_activity_seen) {
        no_swap_gate_blocked_at = "bootstrap_activity";
        no_swap_gate_detail =
            "no Vd/MMIO/bootstrap-runtime or guest dispatch-bridge activity "
            "observed after valid critical import probes";
      } else if (interrupt_callback_ == 0 &&
                 cached_startup_prolog_helper_grace) {
        no_swap_gate_blocked_at = "startup_prolog_helper_grace";
        no_swap_gate_detail = fmt::format(
            "guest main thread has only reached the initial startup prolog "
            "window after processor dispatch entry "
            "(cached_age_ms={} cached_repeat_samples={} cached_detail='{}')",
            cached_dispatch_stall_age_ms, cached_dispatch_stall_repeat_samples,
            cached_dispatch_stall_detail);
      } else if (interrupt_callback_ == 0 && cached_callback_starvation_stall) {
        no_swap_gate_blocked_at =
            callback_dispatch_stall_blocked_at(cached_dispatch_stall_state);
        no_swap_gate_detail = fmt::format(
            "{} (cached_age_ms={} cached_repeat_samples={} "
            "cached_detail='{}' cached_chain='{}' cached_targets='{}')",
            callback_dispatch_stall_summary(cached_dispatch_stall_state),
            cached_dispatch_stall_age_ms, cached_dispatch_stall_repeat_samples,
            cached_dispatch_stall_detail,
            cached_dispatch_stall_chain.empty()
                ? "<none>"
                : cached_dispatch_stall_chain,
            cached_dispatch_stall_targets.empty()
                ? "<none>"
                : cached_dispatch_stall_targets);
      } else if (interrupt_callback_ == 0) {
        if (!runtime_gpu_bootstrap_activity_seen &&
            guest_dispatch_bridge_only_activity) {
          no_swap_gate_blocked_at = "guest_dispatch_bridge_before_callback";
          no_swap_gate_detail = fmt::format(
              "guest main dispatch bridge executed {} time(s), but no runtime "
              "GPU ordinal/thunk/export activity, Vd call, MMIO write, or "
              "loader lookup followed yet; callback remains unset "
              "(next_missing_transition={} runtime_foundation={} "
              "runtime_01C3={} runtime_01D5={} runtime_025B={})",
              proc_stats.guest_main_dispatch_bridge_calls_total,
              foundation_next_missing_transition,
              CriticalGpuOrdinalPipelineStateName(foundation_summary_state),
              CriticalGpuOrdinalPipelineStateName(foundation_01C3.state),
              CriticalGpuOrdinalPipelineStateName(foundation_01D5.state),
              CriticalGpuOrdinalPipelineStateName(foundation_025B.state));
        } else {
          no_swap_gate_blocked_at = "interrupt_callback";
          no_swap_gate_detail =
              "interrupt callback not registered after valid import probes and "
              "runtime GPU bootstrap activity";
        }
      } else if (!callback_guest_registration_seen) {
        no_swap_gate_blocked_at = "callback_guest_registration";
        no_swap_gate_detail = fmt::format(
            "interrupt callback is set but guest callback registration is "
            "missing (vd_set_cb_calls={} runtime_01D5_total={})",
            vd_cb_count, runtime_01D5_total);
      } else if (!producer_entry_seen) {
        no_swap_gate_blocked_at = "producer_entry";
        no_swap_gate_detail = fmt::format(
            "no VdSwap/runtime 0x025B/MMIO-WPTR/guest swap packet activity "
            "(vd_swap_calls={} runtime_025B_total={} mmio_wptr={} "
            "swap_packets={} debug_host_swaps={} guest_swap_packets={})",
            vd_swap_count, runtime_025B_total, mmio_wptr_guest, swap_packets,
            debug_host_swaps, guest_swap_packet_seen ? "YES" : "NO");
      } else {
        no_swap_hard_gate_ready = true;
        no_swap_gate_detail =
            "guest reached producer path after import/callback validation";
      }
      XELOGW(
          "RING BUFFER: no-swap hard gate {} (blocked_at={} detail='{}' "
          "import_probe_gate_ready={} dispatch_path_gate_ready={} "
          "bootstrap_activity={} callback_set={} "
          "callback_guest_registration={} producer_entry={})",
          no_swap_hard_gate_ready ? "READY" : "WAITING",
          no_swap_gate_blocked_at, no_swap_gate_detail,
          import_probe_gate_ready ? "YES" : "NO",
          dispatch_path_gate_ready ? "YES" : "NO",
          bootstrap_activity_seen ? "YES" : "NO",
          interrupt_callback_ != 0 ? "YES" : "NO",
          callback_guest_registration_seen ? "YES" : "NO",
          producer_entry_seen ? "YES" : "NO");
      std::string bootstrap_activity_detail =
          bootstrap_activity_seen
              ? guest_dispatch_bridge_only_activity
                    ? fmt::format(
                          "guest main dispatch bridge observed {} time(s), "
                          "but no Vd/MMIO/runtime GPU bootstrap activity has "
                          "followed yet",
                          proc_stats.guest_main_dispatch_bridge_calls_total)
                    : "guest bootstrap or dispatch-bridge activity has been "
                      "observed"
              : dispatch_path_gate_ready
                    ? "no Vd/MMIO/bootstrap-runtime or guest dispatch-bridge "
                      "activity observed after dispatch path activation"
                    : "blocked behind dispatch_path_gate; no critical GPU "
                      "dispatch path has executed yet";
      std::string callback_set_detail =
          (interrupt_callback_ != 0)
              ? "graphics interrupt callback is registered"
              : cached_startup_prolog_helper_grace
                    ? fmt::format(
                          "guest main thread has only reached the initial "
                          "startup prolog window after dispatch entry "
                          "(cached_age_ms={} cached_repeat_samples={} "
                          "cached_detail='{}')",
                          cached_dispatch_stall_age_ms,
                          cached_dispatch_stall_repeat_samples,
                          cached_dispatch_stall_detail)
              : cached_callback_starvation_stall
                    ? fmt::format(
                          "{} (cached_age_ms={} cached_repeat_samples={} "
                          "cached_detail='{}')",
                          callback_dispatch_stall_summary(
                              cached_dispatch_stall_state),
                          cached_dispatch_stall_age_ms,
                          cached_dispatch_stall_repeat_samples,
                          cached_dispatch_stall_detail)
                    : (runtime_01D5_total != 0 || vd_cb_count != 0)
                    ? "VdSetGraphicsInterruptCallback path executed, but "
                      "callback remained unset"
                    : "VdSetGraphicsInterruptCallback never executed, so "
                      "callback remains unset";
      std::string callback_guest_registration_detail =
          callback_guest_registration_seen
              ? "guest callback registration has been observed"
              : cached_startup_prolog_helper_grace
                    ? fmt::format(
                          "guest callback registration is not expected yet "
                          "because the guest is still in the initial startup "
                          "prolog window (cached_age_ms={} "
                          "cached_repeat_samples={} vd_set_cb_calls={} "
                          "runtime_01D5_total={})",
                          cached_dispatch_stall_age_ms,
                          cached_dispatch_stall_repeat_samples, vd_cb_count,
                          runtime_01D5_total)
              : cached_callback_starvation_stall
                    ? fmt::format(
                          "{} (cached_age_ms={} cached_repeat_samples={} "
                          "vd_set_cb_calls={} runtime_01D5_total={})",
                          callback_dispatch_stall_summary(
                              cached_dispatch_stall_state),
                          cached_dispatch_stall_age_ms,
                          cached_dispatch_stall_repeat_samples, vd_cb_count,
                          runtime_01D5_total)
                    : dispatch_path_gate_ready
                    ? fmt::format(
                          "guest callback registration missing "
                          "(vd_set_cb_calls={} runtime_01D5_total={})",
                          vd_cb_count, runtime_01D5_total)
                    : "blocked behind dispatch_path_gate; 0x01D5 path never "
                      "executed";
      std::string producer_entry_detail =
          producer_entry_seen
              ? "producer path has been observed"
              : dispatch_path_gate_ready
                    ? fmt::format(
                          "no producer entry yet "
                          "(vd_swap_calls={} runtime_025B_total={} "
                          "mmio_wptr={} swap_packets={} debug_host_swaps={})",
                          vd_swap_count, runtime_025B_total, mmio_wptr_guest,
                          swap_packets, debug_host_swaps)
                    : "blocked behind dispatch_path_gate; 0x025B path never "
                      "executed";
      XELOGW(
          "RING BUFFER: blocked dependency matrix "
          "bootstrap_activity={} bootstrap_detail='{}' "
          "callback_set={} callback_set_detail='{}' "
          "callback_guest_registration={} callback_guest_registration_detail='{}' "
          "producer_entry={} producer_entry_detail='{}'",
          bootstrap_activity_seen ? "YES" : "NO", bootstrap_activity_detail,
          interrupt_callback_ != 0 ? "YES" : "NO", callback_set_detail,
          callback_guest_registration_seen ? "YES" : "NO",
          callback_guest_registration_detail,
          producer_entry_seen ? "YES" : "NO", producer_entry_detail);
      XELOGW(
          "RING BUFFER: critical ordinal foundation summary "
          "static_ready={} runtime_valid={} foundation_state={} "
          "next_missing_transition={} dispatch_evidence={} "
          "guest_main_dispatch_bridge_total={} loader_module_handle_calls_total={} "
          "loader_proc_lookup_calls_total={} proc_lookup_calls={} "
          "xex_priv_branch_only={} "
          "xexcheck_static_thunk_calls={} xexcheck_export_entry_calls={} "
          "xexcheck_runtime_state={} stall_state={} stall_age_ms={}",
          critical_foundation_static_ready ? "YES" : "NO",
          critical_foundation_runtime_valid ? "YES" : "NO",
          CriticalGpuOrdinalPipelineStateName(foundation_summary_state),
          foundation_next_missing_transition,
          DispatchPathEvidenceCaseName(dispatch_evidence.evidence_case),
          proc_stats.guest_main_dispatch_bridge_calls_total,
          proc_stats.loader_module_handle_calls_total,
          proc_stats.loader_proc_lookup_calls_total, proc_stats.proc_lookup_calls,
          callback_missing_stall_xex_privilege_branch_only ? "YES" : "NO",
          proc_stats.xex_check_executable_privilege_static_thunk_calls,
          proc_stats.xex_check_executable_privilege_export_entry_calls,
          XexCheckExecutablePrivilegeRuntimeStateName(xexcheck_runtime_state),
          callback_missing_stall_state.empty() ? "<none>"
                                              : callback_missing_stall_state,
          callback_missing_stall_age_ms);
      auto log_critical_ordinal_foundation =
          [&](uint16_t ordinal,
              const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* stat,
              bool static_wiring_found, bool import_probe_ready,
              uint32_t value_addr, uint32_t thunk_addr,
              bool side_effect_seen,
              const CriticalGpuOrdinalFoundationState& foundation) {
            XELOGW(
                "RING BUFFER: critical ordinal foundation ordinal=0x{:03X} "
                "name={} role={} static_wiring={} import_probe={} "
                "runtime_state={} next_transition={} runtime_valid={} "
                "expected_effect={} side_effect_seen={} value_addr={:08X} "
                "thunk_addr={:08X} dyn={} res={} miss={} inv={} fb={} "
                "thunk={} export={}",
                static_cast<uint32_t>(ordinal),
                kernel::xboxkrnl::GetGpuBootstrapOrdinalName(ordinal),
                foundation.role, static_wiring_found ? "YES" : "NO",
                import_probe_ready ? "READY" : "WAITING",
                CriticalGpuOrdinalPipelineStateName(foundation.state),
                foundation.next_transition,
                foundation.runtime_valid ? "YES" : "NO",
                foundation.expected_effect, side_effect_seen ? "YES" : "NO",
                value_addr, thunk_addr,
                stat ? stat->dynamic_lookup_calls : 0,
                stat ? stat->dynamic_lookup_resolved_calls : 0,
                stat ? stat->dynamic_lookup_missing_calls : 0,
                stat ? stat->dynamic_lookup_invalid_module_calls : 0,
                stat ? stat->dynamic_lookup_fallback_hits : 0,
                stat ? stat->static_thunk_calls : 0,
                stat ? stat->export_entry_calls : 0);
          };
      log_critical_ordinal_foundation(0x01C3, stat_01C3, wired_01C3,
                                      probe_01C3_ready, value_01C3, thunk_01C3,
                                      foundation_effect_01C3_seen,
                                      foundation_01C3);
      log_critical_ordinal_foundation(0x01D5, stat_01D5, wired_01D5,
                                      probe_01D5_ready, value_01D5, thunk_01D5,
                                      foundation_effect_01D5_seen,
                                      foundation_01D5);
      log_critical_ordinal_foundation(0x025B, stat_025B, wired_025B,
                                      probe_025B_ready, value_025B, thunk_025B,
                                      foundation_effect_025B_seen,
                                      foundation_025B);
      if (import_probe_gate_ready && !dispatch_path_gate_ready && kernel_state_) {
        auto* emulator = kernel_state_->emulator();
        if (emulator) {
          std::string prior_dispatch_stall_state;
          bool have_prior_dispatch_stall = emulator->GetLastGpuDispatchStallSnapshot(
              &prior_dispatch_stall_state, nullptr, nullptr, nullptr, nullptr,
              nullptr);
          static std::atomic<uint64_t>
              dispatch_stall_main_thread_sample_last_ms{0};
          uint64_t sample_interval_ms = static_cast<uint64_t>(
              cvars::gpu_vblank_callback_startup_gate_log_interval_ms);
          if (have_prior_dispatch_stall &&
              prior_dispatch_stall_state == "guest_main_context_not_ready") {
            sample_interval_ms =
                sample_interval_ms == 0
                    ? 0
                    : std::min<uint64_t>(sample_interval_ms, 50);
          }
          uint64_t prev_sample_ms =
              dispatch_stall_main_thread_sample_last_ms.load(
                  std::memory_order_relaxed);
          bool sample_due =
              prev_sample_ms == 0 || sample_interval_ms == 0 ||
              now_ms < prev_sample_ms ||
              now_ms - prev_sample_ms >= sample_interval_ms;
          if (sample_due) {
            dispatch_stall_main_thread_sample_last_ms.store(
                now_ms, std::memory_order_relaxed);
            emulator->LogMainThreadGpuDispatchStallSample(
                "gpu-dispatch-path-stall", thunk_01C3, thunk_01D5, thunk_025B);
          }
          std::string dispatch_stall_state;
          std::string dispatch_stall_detail;
          std::string dispatch_stall_chain;
          std::string dispatch_stall_targets;
          uint32_t dispatch_stall_repeat_samples = 0;
          uint64_t dispatch_stall_sample_ms = 0;
          if (emulator->GetLastGpuDispatchStallSnapshot(
                  &dispatch_stall_state, &dispatch_stall_detail,
                  &dispatch_stall_chain, &dispatch_stall_targets,
                  &dispatch_stall_repeat_samples,
                  &dispatch_stall_sample_ms)) {
            uint64_t dispatch_stall_sample_age_ms =
                (dispatch_stall_sample_ms && now_ms >= dispatch_stall_sample_ms)
                    ? (now_ms - dispatch_stall_sample_ms)
                    : 0;
            XELOGW(
                "RING BUFFER: dispatch stall hard-gate chain state={} "
                "repeat_samples={} sample_age_ms={} detail='{}' chain='{}' "
                "targets='{}'",
                dispatch_stall_state, dispatch_stall_repeat_samples,
                dispatch_stall_sample_age_ms, dispatch_stall_detail,
                dispatch_stall_chain.empty() ? "<none>"
                                             : dispatch_stall_chain,
                dispatch_stall_targets.empty() ? "<none>"
                                               : dispatch_stall_targets);
          }
        }
      }

      if (!critical_foundation_runtime_valid) {
        XELOGW(
            "RING BUFFER: callback-missing diagnosis next_missing_transition={} "
            "foundation_state={} "
            "(static import probes are not runtime validation)",
            foundation_next_missing_transition,
            CriticalGpuOrdinalPipelineStateName(foundation_summary_state));
      }
    }
    uint32_t dump_after = cvars::gpu_debug_interrupt_missing_dump_after_vblank;
    if (dump_after > 0 && vblank_id >= dump_after) {
      static std::atomic<uint64_t> last_missing_cb_dump_ms{0};
      static std::atomic<uint64_t> last_missing_cb_dump_deferred_ms{0};
      uint64_t interval = static_cast<uint64_t>(
          cvars::gpu_debug_interrupt_missing_dump_interval_ms);
      uint64_t last_dump =
          last_missing_cb_dump_ms.load(std::memory_order_relaxed);
      std::string missing_cb_dump_stall_state;
      uint32_t missing_cb_dump_stall_repeat_samples = 0;
      uint64_t missing_cb_dump_stall_sample_ms = 0;
      uint64_t missing_cb_dump_stall_age_ms = 0;
      bool missing_cb_dump_startup_prolog_grace = false;
      if (kernel_state_) {
        if (auto* emulator = kernel_state_->emulator()) {
          if (emulator->GetLastGpuDispatchStallSnapshot(
                  &missing_cb_dump_stall_state, nullptr, nullptr, nullptr,
                  &missing_cb_dump_stall_repeat_samples,
                  &missing_cb_dump_stall_sample_ms) &&
              missing_cb_dump_stall_sample_ms != 0 &&
              now_ms >= missing_cb_dump_stall_sample_ms) {
            missing_cb_dump_stall_age_ms =
                now_ms - missing_cb_dump_stall_sample_ms;
          }
          missing_cb_dump_startup_prolog_grace =
              IsStartupPrologHelperGraceWindow(
                  missing_cb_dump_stall_state, missing_cb_dump_stall_age_ms,
                  missing_cb_dump_stall_repeat_samples);
        }
      }
      if (last_dump == 0 || interval == 0 || now_ms - last_dump >= interval) {
        if (missing_cb_dump_startup_prolog_grace) {
          uint64_t last_deferred =
              last_missing_cb_dump_deferred_ms.load(std::memory_order_relaxed);
          if (last_deferred == 0 || interval == 0 ||
              now_ms - last_deferred >= interval) {
            last_missing_cb_dump_deferred_ms.store(now_ms,
                                                   std::memory_order_relaxed);
            XELOGW(
                "RING BUFFER: callback-missing dump deferred during startup "
                "prolog helper grace "
                "(vblank_id={} since_first_vblank={}ms stall_age_ms={} "
                "repeat_samples={})",
                vblank_id, since_first_vblank, missing_cb_dump_stall_age_ms,
                missing_cb_dump_stall_repeat_samples);
          }
        } else {
          last_missing_cb_dump_ms.store(now_ms, std::memory_order_relaxed);
          XELOGW(
              "DEBUG: GPU interrupt callback missing after {} vblanks "
              "(since_first_vblank={} ms) - dumping early state",
              vblank_id, since_first_vblank);
          DumpNoSwapDebugState(since_first_vblank, read_ptr, write_ptr);
        }
      }
    }
    if (cvars::gpu_debug_force_interrupt_callback_if_missing) {
      if (cvars::gpu_no_force_run_profile) {
        static std::atomic<uint64_t> force_interrupt_no_force_last_log_ms{0};
        LogNoForceProfileSuppressed("FORCED interrupt callback dispatch",
                                    since_first_vblank, now_ms,
                                    &force_interrupt_no_force_last_log_ms);
      } else if (!cvars::gpu_allow_forced_interrupt_callback_fallback) {
        static std::atomic<bool> logged_forced_cb_disabled{false};
        if (!logged_forced_cb_disabled.exchange(true)) {
          XELOGW(
              "DEBUG: Ignoring gpu_debug_force_interrupt_callback_if_missing "
              "because gpu_allow_forced_interrupt_callback_fallback=false");
        }
      } else {
        uint32_t force_after =
            cvars::gpu_debug_force_interrupt_callback_after_vblank;
        if (force_after == 0 || vblank_id >= force_after) {
          ForceFallbackDelaySnapshot forced_interrupt_delay_snapshot;
          if (ShouldDeferForceFallback(kernel_state_,
                                       &forced_interrupt_delay_snapshot)) {
            static std::atomic<uint64_t> forced_interrupt_defer_last_log_ms{0};
            LogForceFallbackDeferred("FORCED interrupt callback dispatch",
                                     since_first_vblank, now_ms,
                                     forced_interrupt_delay_snapshot,
                                     &forced_interrupt_defer_last_log_ms);
          } else {
            if (forced_interrupt_callback_ == 0 &&
                !forced_interrupt_callback_failed_) {
              if (!processor_) {
                XELOGW(
                    "DEBUG: Force interrupt: no processor available to define "
                    "builtin");
                forced_interrupt_callback_failed_ = true;
              } else {
                auto* function = processor_->DefineBuiltin(
                    "ForcedGpuInterruptCallback", &ForcedGpuInterruptCallback,
                    this, nullptr);
                if (!function) {
                  XELOGW("DEBUG: Force interrupt: failed to define builtin callback");
                  forced_interrupt_callback_failed_ = true;
                } else {
                  forced_interrupt_callback_ = function->address();
                  XELOGW("DEBUG: Force interrupt: builtin callback created at {:08X}",
                         forced_interrupt_callback_);
                }
              }
            }
            if (forced_interrupt_callback_ != 0) {
              uint64_t interval = static_cast<uint64_t>(
                  cvars::gpu_debug_force_interrupt_callback_interval_ms);
              uint64_t last_force =
                  last_forced_interrupt_ms_.load(std::memory_order_relaxed);
              if (last_force == 0 || interval == 0 ||
                  now_ms - last_force >= interval) {
                last_forced_interrupt_ms_.store(now_ms,
                                                std::memory_order_relaxed);
                XELOGW(
                    "DEBUG: Force interrupt: delivering vblank via builtin callback "
                    "(cb={:08X}, vblank_id={}, since_first_vblank={} ms)",
                    forced_interrupt_callback_, vblank_id, since_first_vblank);
                bool defer_for_module_load = false;
                if (kernel_state() &&
                    cvars::gpu_defer_interrupt_dispatch_during_module_load) {
                  auto load_stats = kernel_state()->GetUserModuleLoadStats();
                  defer_for_module_load =
                      load_stats.load_inflight &&
                      load_stats.loaded_executable_user_modules == 0;
                  if (defer_for_module_load) {
                    static std::atomic<uint64_t> forced_deferred_count{0};
                    uint64_t forced_defer_id =
                        forced_deferred_count.fetch_add(1) + 1;
                    uint64_t last_dispatch_ms =
                        last_interrupt_dispatch_ms_.load(std::memory_order_relaxed);
                    uint64_t dispatch_silence_ms =
                        (last_dispatch_ms && now_ms >= last_dispatch_ms)
                            ? (now_ms - last_dispatch_ms)
                            : 0;
                    if (forced_defer_id <= 8 || (forced_defer_id % 120) == 0) {
                      XELOGW(
                          "DEBUG: Force interrupt deferred during module load "
                          "(vblank_id={} defer_count={} "
                          "load_progress_count={} dispatch_silence_ms={})",
                          vblank_id, forced_defer_id,
                          load_stats.load_progress_count, dispatch_silence_ms);
                    }
                  }
                }
                if (!defer_for_module_load) {
                  kernel_state()->EmulateCPInterruptDPC(forced_interrupt_callback_,
                                                        0, 0, 2);
                  last_interrupt_dispatch_ms_.store(now_ms,
                                                    std::memory_order_relaxed);
                }

                if (since_first_vblank > 1000 && command_processor_ &&
                    !command_processor_->first_frame_rendered()) {
                  static std::atomic<uint64_t> last_swap_force_ms{0};
                  uint64_t last_swap_force =
                      last_swap_force_ms.load(std::memory_order_relaxed);
                  if (last_swap_force == 0 || now_ms - last_swap_force >= 500) {
                    last_swap_force_ms.store(now_ms, std::memory_order_relaxed);
                    XELOGW(
                        "DEBUG: Force interrupt: delivering proactive SWAP FINISH "
                        "(source=1) to unblock game loop");
                    if (!defer_for_module_load) {
                      kernel_state()->EmulateCPInterruptDPC(
                          forced_interrupt_callback_, 0, 1, 2);
                      last_interrupt_dispatch_ms_.store(
                          now_ms, std::memory_order_relaxed);
                    }
                  }
                }
              }
            }
          }
        }
      }
  }
  }

  if (cvars::gpu_debug_sync_profile && guest_main_thread_ready) {
    static std::atomic<uint64_t> last_sync_profile_ms{0};
    uint64_t interval =
        static_cast<uint64_t>(cvars::gpu_debug_sync_profile_interval_ms);
    uint64_t last_sync = last_sync_profile_ms.load(std::memory_order_relaxed);
    if (last_sync == 0 || interval == 0 || now_ms - last_sync >= interval) {
      last_sync_profile_ms.store(now_ms, std::memory_order_relaxed);
      uint64_t cb_set_ms =
          last_interrupt_callback_set_ms_.load(std::memory_order_relaxed);
      uint64_t cb_dispatch_ms =
          last_interrupt_dispatch_ms_.load(std::memory_order_relaxed);
      uint64_t cb_set_age =
          (cb_set_ms && now_ms >= cb_set_ms) ? (now_ms - cb_set_ms) : 0;
      uint64_t cb_dispatch_age = (cb_dispatch_ms && now_ms >= cb_dispatch_ms)
                                     ? (now_ms - cb_dispatch_ms)
                                     : 0;
      uint64_t wptr_ms = last_wptr_update_ms_.load(std::memory_order_relaxed);
      uint64_t wptr_age =
          (wptr_ms && now_ms >= wptr_ms) ? (now_ms - wptr_ms) : 0;
      uint64_t wptr_update_total =
          wptr_update_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_mmio =
          wptr_source_mmio_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_cp_packet =
          wptr_source_cp_packet_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_debug_swap =
          wptr_source_debug_swap_count_.load(std::memory_order_relaxed);
      uint64_t wptr_source_other =
          wptr_source_other_count_.load(std::memory_order_relaxed);
      uint64_t wptr_guest_driven_count =
          wptr_source_mmio + wptr_source_cp_packet;
      uint32_t wptr_last_source_id =
          last_wptr_source_id_.load(std::memory_order_relaxed);
      uint64_t forced_age =
          (last_forced_interrupt_ms_.load(std::memory_order_relaxed) &&
           now_ms >= last_forced_interrupt_ms_.load(std::memory_order_relaxed))
              ? (now_ms -
                 last_forced_interrupt_ms_.load(std::memory_order_relaxed))
              : 0;
      uint64_t swap_count =
          command_processor_ ? command_processor_->swap_packet_count() : 0;
      uint64_t last_swap_ms =
          command_processor_ ? command_processor_->last_swap_packet_ms() : 0;
      uint64_t last_swap_age = (last_swap_ms && now_ms >= last_swap_ms)
                                   ? (now_ms - last_swap_ms)
                                   : 0;

      XELOGW(
          "DEBUG: GPU/CPU sync: vblank_id={} since_first_vblank={}ms cb_set={} "
          "cb_age={}ms cb_dispatch_age={}ms forced_cb={} forced_age={}ms "
          "read_ptr={:08X} write_ptr={:08X} swap_count={} last_swap_age={}ms "
          "last_wptr_age={}ms wptr_updates(total={},guest={},last_source={},"
          "mmio={},cp_packet={},debug_swap={},other={})",
          vblank_id, since_first_vblank,
          interrupt_callback_ != 0 ? "YES" : "NO", cb_set_age, cb_dispatch_age,
          forced_interrupt_callback_ != 0 ? "YES" : "NO", forced_age, read_ptr,
          write_ptr, swap_count, last_swap_age, wptr_age, wptr_update_total,
          wptr_guest_driven_count, WptrUpdateSourceName(wptr_last_source_id),
          wptr_source_mmio, wptr_source_cp_packet, wptr_source_debug_swap,
          wptr_source_other);

      uint64_t vd_swap_count =
          vd_swap_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_swap_age = 0;
      uint64_t vd_swap_ms =
          vd_swap_state_.time_ms.load(std::memory_order_relaxed);
      if (vd_swap_ms && now_ms >= vd_swap_ms) {
        vd_swap_age = now_ms - vd_swap_ms;
      }
      uint64_t vd_init_count =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_init_age = 0;
      uint64_t vd_init_ms =
          vd_init_ring_buffer_state_.time_ms.load(std::memory_order_relaxed);
      if (vd_init_ms && now_ms >= vd_init_ms) {
        vd_init_age = now_ms - vd_init_ms;
      }
      uint64_t vd_cb_count = vd_set_interrupt_callback_state_.count.load(
          std::memory_order_relaxed);
      uint64_t vd_cb_age = 0;
      uint64_t vd_cb_ms = vd_set_interrupt_callback_state_.time_ms.load(
          std::memory_order_relaxed);
      if (vd_cb_ms && now_ms >= vd_cb_ms) {
        vd_cb_age = now_ms - vd_cb_ms;
      }
      uint64_t vd_rptr_wb_count =
          vd_enable_rptr_writeback_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_rptr_wb_age = 0;
      uint64_t vd_rptr_wb_ms =
          vd_enable_rptr_writeback_state_.time_ms.load(std::memory_order_relaxed);
      if (vd_rptr_wb_ms && now_ms >= vd_rptr_wb_ms) {
        vd_rptr_wb_age = now_ms - vd_rptr_wb_ms;
      }
      uint64_t vd_sys_count =
          vd_get_system_cmd_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_sys_age = 0;
      uint64_t vd_sys_ms =
          vd_get_system_cmd_buffer_state_.time_ms.load(std::memory_order_relaxed);
      if (vd_sys_ms && now_ms >= vd_sys_ms) {
        vd_sys_age = now_ms - vd_sys_ms;
      }
      XELOGW(
          "DEBUG: GPU/CPU sync: VdSwap(count={}, age={}ms) "
          "VdInitRB(count={}, age={}ms) VdRPtrWB(count={}, age={}ms) "
          "VdSetCb(count={}, age={}ms) VdSysCB(count={}, age={}ms)",
          vd_swap_count, vd_swap_age, vd_init_count, vd_init_age,
          vd_rptr_wb_count, vd_rptr_wb_age, vd_cb_count, vd_cb_age,
          vd_sys_count, vd_sys_age);
        uint64_t mmio_rb_wptr_writes =
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed);
        uint64_t mmio_rb_base_writes =
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed);
        uint64_t vd_swap_effective =
          vd_swap_count ? vd_swap_count : (mmio_rb_wptr_writes ? 1 : 0);
        uint64_t vd_init_effective =
          vd_init_count ? vd_init_count : (mmio_rb_base_writes ? 1 : 0);
        XELOGW(
          "DEBUG: GPU/CPU sync (effective): VdSwapEff(count={}, source={}) "
          "VdInitRBEff(count={}, source={})",
          vd_swap_effective,
          vd_swap_count ? "VdSwap" : (mmio_rb_wptr_writes ? "MMIO_WPTR" : "none"),
          vd_init_effective,
          vd_init_count ? "VdInitRB" : (mmio_rb_base_writes ? "MMIO_RB_BASE" : "none"));

      if (command_processor_) {
        auto rb_dbg = command_processor_->GetRingBufferDebugState();
        uint64_t last_write_age =
            (rb_dbg.ring_buffer_last_write_ms &&
             now_ms >= rb_dbg.ring_buffer_last_write_ms)
                ? (now_ms - rb_dbg.ring_buffer_last_write_ms)
                : 0;
        uint64_t last_arm_age =
            (rb_dbg.ring_buffer_watch_last_arm_ms &&
             now_ms >= rb_dbg.ring_buffer_watch_last_arm_ms)
                ? (now_ms - rb_dbg.ring_buffer_watch_last_arm_ms)
                : 0;
        XELOGW(
            "DEBUG: GPU/CPU sync: ring write watch hits={} arms={} "
            "last_write_age={}ms last_arm_age={}ms rearm_pending={}",
            rb_dbg.ring_buffer_watch_hits, rb_dbg.ring_buffer_watch_arm_count,
            last_write_age, last_arm_age,
            rb_dbg.ring_buffer_watch_rearm_requested ? "YES" : "NO");
      }

      if (cvars::gpu_debug_sync_profile_dump_threads) {
        if (since_first_vblank < 25) {
          XELOGW(
              "RING BUFFER: deferring GPU_SYNC thread dump during early "
              "startup window (age={}ms) to avoid perturbing guest-main "
              "dispatch before the initial context settles",
              since_first_vblank);
        } else {
          DumpGuestThreadStates("GPU_SYNC");
        }
      }
    }
  }

  // Heavy debug: detect lack of swap packets and optionally inject a test swap.
  static std::atomic<uint64_t> last_no_swap_log_ms{0};
  static std::atomic<uint64_t> last_force_swap_ms{0};
  static std::atomic<bool> forced_swap_once{false};
  if (command_processor_ && guest_main_thread_ready) {
    uint64_t swap_count = command_processor_->swap_packet_count();
    uint64_t last_swap_ms = command_processor_->last_swap_packet_ms();
    uint64_t guest_output_refresh_attempt_count =
      presenter_ ? presenter_->GetGuestOutputRefreshAttemptCount() : 0;
    uint64_t guest_output_refresh_success_count =
      presenter_ ? presenter_->GetGuestOutputRefreshSuccessCount() : 0;

    if (cvars::gpu_debug_guest_bootstrap_inject_if_stalled) {
      static std::atomic<uint64_t> last_guest_bootstrap_inject_ms{0};
      static std::atomic<bool> guest_bootstrap_injected_once{false};

      auto dbg = command_processor_->GetRingBufferDebugState();
      bool host_debug_swap_only =
          dbg.debug_host_swap_injection_count > 0 && dbg.swap_packet_count > 0;
      bool vd_bootstrap_missing =
          vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed) == 0 &&
          vd_set_interrupt_callback_state_.count.load(std::memory_order_relaxed) ==
              0 &&
          vd_swap_state_.count.load(std::memory_order_relaxed) == 0;
      bool mmio_bootstrap_missing =
          mmio_cp_rb_base_.guest_count.load(std::memory_order_relaxed) == 0 &&
          mmio_cp_rb_cntl_.guest_count.load(std::memory_order_relaxed) == 0 &&
          mmio_cp_rb_wptr_.guest_count.load(std::memory_order_relaxed) == 0;
      bool eligible =
          host_debug_swap_only && vd_bootstrap_missing && mmio_bootstrap_missing &&
          dbg.ring_buffer_initialized && dbg.initialization_acknowledged &&
          since_first_vblank >= cvars::gpu_debug_guest_bootstrap_inject_after_ms;

      bool already_once =
          guest_bootstrap_injected_once.load(std::memory_order_relaxed);
      if (eligible &&
          (!cvars::gpu_debug_guest_bootstrap_inject_once || !already_once)) {
        uint64_t last_inject =
            last_guest_bootstrap_inject_ms.load(std::memory_order_relaxed);
        uint64_t inject_interval =
            static_cast<uint64_t>(cvars::gpu_debug_guest_bootstrap_inject_interval_ms);
        if (last_inject == 0 || inject_interval == 0 ||
            now_ms - last_inject >= inject_interval) {
          last_guest_bootstrap_inject_ms.store(now_ms,
                                               std::memory_order_relaxed);
          guest_bootstrap_injected_once.store(true, std::memory_order_relaxed);
          InjectGuestBootstrapDebugActivity("stalled-host-debug-swap-only",
                                            since_first_vblank);
        }
      }
    }

    if (cvars::gpu_debug_guest_bootstrap_keepalive_ms > 0) {
      static std::atomic<uint64_t> last_guest_keepalive_ms{0};
      auto dbg = command_processor_->GetRingBufferDebugState();
      bool host_debug_swap_only =
          dbg.debug_host_swap_injection_count > 0 && dbg.swap_packet_count > 0;
      bool ring_idle = dbg.read_ptr_index == dbg.write_ptr_index;
      bool no_guest_ring_writes_observed = dbg.ring_buffer_watch_hits == 0;
      if (host_debug_swap_only && ring_idle && no_guest_ring_writes_observed) {
        uint64_t last_keepalive =
            last_guest_keepalive_ms.load(std::memory_order_relaxed);
        uint64_t keepalive_interval =
            static_cast<uint64_t>(cvars::gpu_debug_guest_bootstrap_keepalive_ms);
        if (last_keepalive == 0 || now_ms - last_keepalive >= keepalive_interval) {
          last_guest_keepalive_ms.store(now_ms, std::memory_order_relaxed);
          InjectGuestBootstrapDebugActivity("guest-debug-keepalive",
                                            since_first_vblank);
        }
      }
    }

    static std::atomic<uint64_t> last_zero_refresh_watchdog_ms{0};
    if (guest_output_refresh_success_count == 0) {
      uint64_t last_watchdog =
        last_zero_refresh_watchdog_ms.load(std::memory_order_relaxed);
      if (since_first_vblank >= 250 &&
        (last_watchdog == 0 || now_ms - last_watchdog >= 1000)) {
      last_zero_refresh_watchdog_ms.store(now_ms, std::memory_order_relaxed);
      uint64_t vd_init_ring_count =
        vd_init_ring_buffer_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_set_cb_count = vd_set_interrupt_callback_state_.count.load(
        std::memory_order_relaxed);
      uint64_t vd_swap_count =
        vd_swap_state_.count.load(std::memory_order_relaxed);
      uint64_t vd_sys_cb_count = vd_get_system_cmd_buffer_state_.count.load(
        std::memory_order_relaxed);
      bool rb_initialized = command_processor_->IsRingBufferInitialized();
      auto rb_state = command_processor_->GetRingBufferDebugState();
      XELOGW(
        "DEBUG: ZERO-REFRESH WATCHDOG: vblank_id={} age={}ms swap_count={} "
        "last_swap_ms={} rb_init={} init_ack={} read_ptr={:08X} "
        "write_ptr={:08X} interrupt_callback_set={} "
        "refresh_attempt_count={} refresh_success_count={}",
        vblank_id, since_first_vblank, swap_count, last_swap_ms,
        rb_initialized ? "YES" : "NO",
        rb_state.initialization_acknowledged ? "YES" : "NO",
        rb_state.read_ptr_index, rb_state.write_ptr_index,
        interrupt_callback_ != 0 ? "YES" : "NO",
        guest_output_refresh_attempt_count,
        guest_output_refresh_success_count);
      XELOGW(
        "DEBUG: ZERO-REFRESH WATCHDOG: Vd counters InitRB={} SetCb={} "
        "Swap={} GetSysCB={}",
        vd_init_ring_count, vd_set_cb_count, vd_swap_count,
        vd_sys_cb_count);
      std::string zero_refresh_stall_state;
      uint64_t zero_refresh_stall_sample_ms = 0;
      uint64_t zero_refresh_stall_age_ms = 0;
      if (kernel_state_) {
        auto* emulator = kernel_state_->emulator();
        if (emulator &&
            emulator->GetLastGpuDispatchStallSnapshot(
                &zero_refresh_stall_state, nullptr, nullptr, nullptr, nullptr,
                &zero_refresh_stall_sample_ms) &&
            zero_refresh_stall_sample_ms != 0 &&
            now_ms >= zero_refresh_stall_sample_ms) {
          zero_refresh_stall_age_ms = now_ms - zero_refresh_stall_sample_ms;
        }
      }
        bool producer_dead_no_attempts =
            (guest_output_refresh_attempt_count == 0 &&
             vd_init_ring_count == 0 && vd_set_cb_count == 0 &&
             vd_swap_count == 0 && swap_count == 0);
      if (guest_output_refresh_attempt_count == 0 &&
        vd_init_ring_count == 0 && vd_set_cb_count == 0 &&
        vd_swap_count == 0) {
        if (!zero_refresh_stall_state.empty()) {
          XELOGW(
            "DEBUG: ZERO-REFRESH WATCHDOG diagnosis: RefreshGuestOutput has "
            "never been attempted because no Vd bootstrap/swap activity "
            "is observed yet while guest stall_state={} age_ms={} "
            "(producer path dead before presenter mailbox).",
            zero_refresh_stall_state, zero_refresh_stall_age_ms);
        } else {
          XELOGW(
            "DEBUG: ZERO-REFRESH WATCHDOG diagnosis: RefreshGuestOutput has "
            "never been attempted because no Vd bootstrap/swap activity "
            "is observed yet (producer path dead before presenter mailbox)."
          );
        }
      }
        if (producer_dead_no_attempts &&
            cvars::gpu_debug_dead_pipeline_recover_after_ms > 0 &&
            since_first_vblank >= cvars::gpu_debug_dead_pipeline_recover_after_ms) {
          bool already_recovered =
              dead_pipeline_recovery_done_.load(std::memory_order_relaxed);
          if (!cvars::gpu_debug_dead_pipeline_recover_once || !already_recovered) {
            ForceFallbackDelaySnapshot dead_pipeline_delay_snapshot;
            if (ShouldDeferForceFallback(kernel_state_,
                                         &dead_pipeline_delay_snapshot)) {
              static std::atomic<uint64_t>
                  dead_pipeline_recovery_defer_last_log_ms{0};
              LogForceFallbackDeferred(
                  "DEAD-PIPELINE recovery", since_first_vblank, now_ms,
                  dead_pipeline_delay_snapshot,
                  &dead_pipeline_recovery_defer_last_log_ms);
            } else {
              dead_pipeline_recovery_done_.store(true, std::memory_order_relaxed);
              dead_pipeline_recovery_count_.fetch_add(1, std::memory_order_relaxed);
              last_dead_pipeline_recovery_ms_.store(now_ms,
                                                    std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: DEAD-PIPELINE RECOVERY: forcing fallback ring init and "
                  "injecting host-side swap (age={}ms)",
                  since_first_vblank);
              command_processor_->ForceFallbackInitialization();
              InjectGuestBootstrapDebugActivity("dead-pipeline-recovery",
                                since_first_vblank);
              auto res = GraphicsSystem::GetInternalDisplayResolution();
              uint32_t width = res.first;
              uint32_t height = res.second;
              XELOGI(
                  "DEBUG: DEAD-PIPELINE RECOVERY: queuing host debug swap after "
                  "fallback init request ({}x{})",
                  width, height);
              command_processor_->CallInThread(
                  [this, width, height]() {
                    command_processor_->DebugIssueSwapFromHost(width, height);
                  },
                  "dead-pipeline-recovery-host-swap");
              TryAutoRecoverMissingInterruptCallback(
                  now_ms, since_first_vblank, "dead-pipeline-recovery");
            }
          }
        }
      }

      }

    if (cvars::gpu_log_no_swap_after_ms > 0 &&
        since_first_vblank >= cvars::gpu_log_no_swap_after_ms &&
        swap_count == 0) {
      uint64_t last_log = last_no_swap_log_ms.load(std::memory_order_relaxed);
      uint64_t log_interval =
          static_cast<uint64_t>(cvars::gpu_log_no_swap_interval_ms);
      if (last_log == 0 || log_interval == 0 ||
          now_ms - last_log >= log_interval) {
        last_no_swap_log_ms.store(now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: NO SWAP PACKETS: none observed after {} ms since first vblank. "
          "read_ptr={:08X} write_ptr={:08X} last_swap_ms={} swap_count={} "
          "guest_output_refresh_success_count={}",
          since_first_vblank, read_ptr, write_ptr, last_swap_ms, swap_count,
          guest_output_refresh_success_count);
        if (guest_output_refresh_success_count == 0) {
          XELOGW(
            "RING BUFFER: no-swap correlation detected: guest output never "
            "refreshed successfully; Vulkan presenter will stay in "
            "force_clear_only=YES with reason=missing_guest_output until "
            "the ring/bootstrap path emits real swap output.");
        }
        WarnIfRingBufferNeverInitialized(since_first_vblank);
        uint32_t rb_base = command_processor_->primary_buffer_ptr();
        uint32_t rb_size = command_processor_->primary_buffer_size();
        XELOGI("   ring buffer base={:08X} size={:08X}", rb_base, rb_size);
        if (cvars::gpu_log_no_swap_dump_fetch && register_file_) {
          xenos::xe_gpu_texture_fetch_t fetch =
              register_file_->GetTextureFetch(0);
          DumpFetch0(fetch);
        }
        if (cvars::gpu_log_no_swap_dump_ring && memory_ && rb_base && rb_size) {
          const uint8_t* rb_host = reinterpret_cast<const uint8_t*>(
              memory_->TranslatePhysical(rb_base));
          uint32_t ring_dwords = rb_size / sizeof(uint32_t);
          uint32_t dump_dwords = std::min<uint32_t>(
              ring_dwords, cvars::gpu_log_no_swap_dump_ring_dwords);
          DumpRingWindow(rb_host, ring_dwords, read_ptr, dump_dwords);
        }
        DumpNoSwapDebugState(since_first_vblank, read_ptr, write_ptr);
      }
    }

    if (cvars::gpu_debug_force_swap_after_ms > 0 &&
        since_first_vblank >= cvars::gpu_debug_force_swap_after_ms &&
        (swap_count == 0 || !cvars::gpu_debug_force_swap_once)) {
      bool already_forced = forced_swap_once.load(std::memory_order_relaxed);
      if (!cvars::gpu_debug_force_swap_once || !already_forced) {
        uint64_t last_force =
            last_force_swap_ms.load(std::memory_order_relaxed);
        uint64_t force_interval =
            static_cast<uint64_t>(cvars::gpu_debug_force_swap_interval_ms);
        if (last_force == 0 || force_interval == 0 ||
            now_ms - last_force >= force_interval) {
          last_force_swap_ms.store(now_ms, std::memory_order_relaxed);
          forced_swap_once.store(true, std::memory_order_relaxed);
          auto res = GraphicsSystem::GetInternalDisplayResolution();
          uint32_t width = res.first;
          uint32_t height = res.second;
          XELOGW(
              "DEBUG: scheduling host-side swap injection ({}x{}, "
              "since_first_vblank={} ms)",
              width, height, since_first_vblank);
          command_processor_->CallInThread(
              [this, width, height]() {
                command_processor_->DebugIssueSwapFromHost(width, height);
              },
              "debug-force-host-swap");
        }
      }
    }
  }

  // Dispatch only when the guest has installed a callback.
  if (interrupt_callback_ != 0) {
    // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
    //     something wrong and the CP will block waiting for code that
    //     needs to be run in the interrupt.
    DispatchInterruptCallback(0, 2);
  } else if (cvars::trace_gpu_bootstrap) {
    XELOGI(
        "DEBUG: GPU vblank dispatch skipped: no guest callback installed "
        "(waiting for VdSetGraphicsInterruptCallback)");
  }
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread(
      [&]() { command_processor_->ClearCaches(); },
      "graphics-system-clear-caches");
}

void GraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  if (!cvars::store_shaders) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      auto fence_event = xe::threading::Event::CreateAutoResetEvent(false);
      auto shared_fence =
          std::shared_ptr<xe::threading::Event>(std::move(fence_event));
      command_processor_->CallInThread(
          [this, cache_root, title_id, shared_fence]() {
            XELOGI("DEBUG: GPU THREAD: Starting shader storage initialization...");
            command_processor_->InitializeShaderStorage(cache_root, title_id,
                                                        true);
            XELOGI("DEBUG: GPU THREAD: Shader storage initialization completed!");
            shared_fence->Set();
          },
          "initialize-shader-storage-blocking");
      auto wait_result = xe::threading::Wait(shared_fence.get(), false,
                                             std::chrono::seconds(5));
      if (wait_result == xe::threading::WaitResult::kTimeout) {
        XELOGW(
            "DEBUG: Shader storage blocking wait timed out; continuing launch and "
            "allowing initialization to finish asynchronously.");
      }
    }
  } else {
    command_processor_->CallInThread(
        [this, cache_root, title_id]() {
          command_processor_->InitializeShaderStorage(cache_root, title_id,
                                                      false);
        },
        "initialize-shader-storage-async");
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(cvars::trace_gpu_prefix);
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(cvars::trace_gpu_prefix);
}

void GraphicsSystem::EndTracing() { command_processor_->EndTracing(); }

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

std::pair<uint16_t, uint16_t> GraphicsSystem::GetInternalDisplayResolution() {
  if (cvars::internal_display_resolution >=
      internal_display_resolution_entries.size()) {
    return {cvars::internal_display_resolution_x,
            cvars::internal_display_resolution_y};
  }
  return internal_display_resolution_entries
      [cvars::internal_display_resolution];
}

}  // namespace gpu
}  // namespace xe
