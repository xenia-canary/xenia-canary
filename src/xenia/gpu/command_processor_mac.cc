/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/command_processor_mac.h"
#include "xenia/emulator_mac.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory_mac.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags_mac.h"
#include "xenia/gpu/graphics_system_mac.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/sampler_info.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules_mac.h"
#if !defined(NDEBUG)

#define XE_ENABLE_GPU_REG_WRITE_LOGGING 1
#endif
DEFINE_bool(
    log_guest_driven_gpu_register_written_values, false,
    "Only does anything in debug builds, if set will log every write to a gpu "
    "register done by a guest. Does not log writes that are done by the CP on "
    "its own, just ones the guest makes or instructs it to make.",
    "GPU");

DEFINE_bool(disassemble_pm4, true,
            "Only does anything in debug builds, if set will disassemble and "
            "log all PM4 packets sent to the CP.",
            "GPU");

DEFINE_bool(
    log_ringbuffer_kickoff_initiator_bts, false,
    "Only does anything in debug builds, if set will log the pseudo-stacktrace "
    "of the guest thread that wrote the new read position.",
    "GPU");

DEFINE_uint32(
    gpu_idle_abort_ms, 0,
    "If >0, abort the running title when the ring buffer stays idle for this "
    "many milliseconds after initialization (no CP_RB_WPTR updates).",
    "GPU");
DEFINE_bool(
    gpu_idle_abort_terminate_title, true,
    "When gpu_idle_abort_ms triggers, terminate the title instead of only "
    "logging.",
    "GPU");
DEFINE_uint32(
    gpu_zero_rwptr_abort_ms, 0,
    "If >0, abort the running title when both read/write pointers stay zero "
    "for this many milliseconds after ring buffer initialization.",
    "GPU");
DEFINE_bool(
    gpu_zero_rwptr_abort_terminate_title, false,
    "When gpu_zero_rwptr_abort_ms triggers, terminate the title instead of "
    "only logging (useful as a hard-stop during debugging).",
    "GPU");
DEFINE_uint32(
    gpu_seed_ringbuffer_nop_dwords, 0,
    "If >0, seed the ring buffer with this many PM4 NOP dwords on "
    "initialization and set WPTR accordingly. Helps avoid zero R/W pointer "
    "abort when games never write CP_RB_WPTR. This is debug-only, requires "
    "gpu_allow_pm4_nop_ring_seed=true, and is intentionally ignored when "
    "gpu_zero_rwptr_abort_ms is 0.",
    "GPU");
DEFINE_bool(
    gpu_allow_pm4_nop_ring_seed, false,
    "Debug safety gate for gpu_seed_ringbuffer_nop_dwords. Keep disabled in "
    "normal investigation; when enabled, PM4 NOP seeding modifies only the "
    "ring-buffer memory, not guest executable code.",
    "GPU");
DEFINE_bool(
    gpu_force_wptr_on_rb_change, false,
    "If enabled, when the ring buffer memory changes without a CP_RB_WPTR "
    "update and both read/write pointers are zero, infer a write pointer "
    "and kick the worker thread. Debug compatibility fallback only.",
    "GPU");
DEFINE_bool(
  gpu_auto_force_wptr_on_rb_change_if_idle, false,
  "Debug: Automatically infer and apply a write pointer when ring-buffer "
  "memory changes without CP_RB_WPTR and the ring is idle "
  "(read_ptr == write_ptr for >100ms).",
  "GPU");
DEFINE_bool(
    gpu_reset_wptr_on_rb_clear, false,
    "If enabled, when the ring buffer appears fully cleared (all zero) and "
    "read/write pointers are equal but non-zero, reset read/write pointers "
    "back to 0 to match the cleared ring state (debug compatibility only).",
    "GPU");
DEFINE_bool(gpu_debug_worker_verbose, false,
            "Debug: Log detailed GPU worker loop activity and state snapshots.",
            "GPU");
DEFINE_int32(gpu_debug_worker_every_n, 1,
             "Debug: Log every N GPU worker loop iterations when verbose "
             "logging is enabled.",
             "GPU");
DEFINE_bool(
    gpu_debug_wait_results, false,
    "Debug: Log WaitAny results and wake reasons in the GPU worker thread.",
    "GPU");
DEFINE_int32(gpu_debug_wait_every_n, 1,
             "Debug: Log every N WaitAny results when wait logging is enabled.",
             "GPU");
DEFINE_uint32(gpu_waitany_timeout_ms, 100,
        "Worker WaitAny timeout in milliseconds before polling state.",
        "GPU");
DEFINE_uint32(
    gpu_waitany_privilege_gate_timeout_ms, 1000,
    "Worker WaitAny timeout in milliseconds while the guest is stalled in a "
    "title-local privilege gate before XexCheckExecutablePrivilege or any "
    "loader / GPU dispatch path is entered.",
    "GPU");
DEFINE_bool(
    gpu_waitany_include_module_load_event, true,
    "Include KernelState module-load heartbeat event in GPU worker WaitAny "
    "to reduce blind timeout polling while modules are inflight.",
    "GPU");
DEFINE_bool(
    gpu_waitany_include_module_load_event_after_ring_ready, false,
    "Include KernelState module-load heartbeat event in GPU worker WaitAny "
    "even after ring initialization has been acknowledged. Disabled by "
    "default to avoid wake-thrash once the ring is ready.",
    "GPU");
DEFINE_bool(
  gpu_worker_suspend_until_module_load_success, true,
  "Strict startup gate: suspend GPU worker loop processing until an "
  "executable user module is visible / LoadUserModule success is observed.",
  "GPU");
DEFINE_uint32(
  gpu_worker_suspend_module_gate_wait_ms, 100,
  "Wait timeout in milliseconds while GPU worker is suspended by strict "
  "module-load gate.",
  "GPU");
DEFINE_uint32(
  gpu_worker_suspend_module_gate_log_interval_ms, 1000,
  "Heartbeat interval in milliseconds while strict GPU worker module-load "
  "gate is active.",
  "GPU");
DEFINE_bool(
  gpu_worker_preloop_bootstrap_gate, true,
  "Strict startup gate: keep GPU worker loop #1 suspended until bootstrap "
  "prerequisites are observed without forcing fallback actions.",
  "GPU");
DEFINE_bool(
  gpu_worker_preloop_bootstrap_gate_require_producer_entry, false,
  "When pre-loop bootstrap gate is enabled, also require producer-path entry "
  "(VdSwap/runtime ordinal 0x025B/MMIO-WPTR/guest swap packet) before loop "
  "#1.",
  "GPU");
DEFINE_uint32(
  gpu_worker_preloop_bootstrap_gate_wait_ms, 25,
  "Wait timeout in milliseconds between pre-loop bootstrap gate checks.",
  "GPU");
DEFINE_uint32(
  gpu_worker_preloop_bootstrap_gate_log_interval_ms, 1000,
  "Heartbeat interval in milliseconds while pre-loop bootstrap gate is "
  "active.",
  "GPU");
DEFINE_uint32(
  gpu_worker_preloop_bootstrap_gate_timeout_ms, 0,
  "If >0, allow pre-loop bootstrap gate to time out after this many "
  "milliseconds and continue without forcing.",
  "GPU");
DEFINE_bool(
  gpu_worker_preloop_bootstrap_gate_allow_startup_prolog_escape, true,
  "Allow the pre-loop bootstrap gate to open when the guest main thread is "
  "persistently sampled in same-module startup prolog helpers or a "
  "title-local pre-GPU privilege gate before any GPU bootstrap activity "
  "appears. This avoids circular startup waits without forcing GPU behavior.",
  "GPU");
DEFINE_uint32(
  gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms, 1000,
  "Minimum persistent age in milliseconds for a startup prolog-helper "
  "dispatch stall before the pre-loop bootstrap gate may open without guest "
  "GPU bootstrap activity.",
  "GPU");
DEFINE_bool(
  gpu_worker_allow_loop_forcing, false,
  "Allow loop-time fallback/host-swap forcing nudges. Disabled by default "
  "to keep worker loops non-forcing during diagnosis.",
  "GPU");
DEFINE_uint32(
  gpu_waitany_zero_rw_recover_after_timeouts, 0,
  "When ring is initialized but read/write pointers stay zero, issue a "
  "non-invasive worker wake nudge after this many consecutive WaitAny "
  "timeouts (0 disables).",
  "GPU");
DEFINE_uint32(
  gpu_producer_entry_grace_ms, 2000,
  "How long producer_entry_missing may persist after callback registration "
  "before being treated as overdue.",
  "GPU");
DEFINE_uint32(
  gpu_producer_entry_nudge_after_ms, 1000,
  "How long producer_entry_missing may persist before issuing host-swap "
  "nudges.",
  "GPU");
DEFINE_uint32(
  gpu_producer_entry_nudge_interval_ms, 750,
  "Minimum interval between producer_entry_missing host-swap nudges.",
  "GPU");
DEFINE_bool(gpu_debug_rb_dump_on_activity, false,
            "Debug: Dump ring buffer windows when GPU activity is detected.",
            "GPU");
DEFINE_bool(gpu_debug_rb_dump_on_idle, false,
            "Debug: Dump ring buffer windows periodically while idle.", "GPU");
DEFINE_bool(
  gpu_debug_rb_scan_on_idle, false,
  "Debug: Scan ring buffer memory while idle to detect changes without "
  "CP_RB_WPTR updates. Disabled by default because large host reads may "
  "be unsafe on some platforms when mappings/protections are transient.",
  "GPU");
DEFINE_bool(
    gpu_debug_rb_dump_on_wait, false,
    "Debug: Dump ring buffer windows before worker sleeps (read==write).",
    "GPU");
DEFINE_uint32(gpu_debug_rb_dump_dwords, 64,
              "Debug: Number of ring buffer dwords to dump in snapshots.",
              "GPU");
DEFINE_uint32(
    gpu_debug_rb_dump_window_dwords, 32,
    "Debug: Number of ring buffer dwords to dump around read/write pointers.",
    "GPU");
DEFINE_uint32(
    gpu_debug_rb_snapshot_interval_ms, 5000,
    "Debug: Interval (ms) between ring buffer state snapshots while idle.",
    "GPU");
DEFINE_bool(gpu_debug_worker_trace_pending, false,
            "Debug: Log pending function queue depth in the worker loop.",
            "GPU");
DEFINE_bool(
    gpu_debug_watch_ringbuffer_writes, false,
    "Debug: Enable physical memory write watch for the ring buffer to detect "
    "guest writes (re-arms periodically).",
    "GPU");
DEFINE_uint32(gpu_debug_watch_ringbuffer_rearm_ms, 2000,
              "Debug: Interval (ms) to re-arm ring buffer write watch.", "GPU");
DEFINE_uint32(gpu_debug_watch_ringbuffer_log_every_n, 1,
              "Debug: Log every N ring buffer write watch hits.", "GPU");
DEFINE_uint32(
    gpu_debug_watch_ringbuffer_dump_dwords, 16,
    "Debug: Number of ring buffer dwords to dump around read/write pointers "
    "on write watch hits.",
    "GPU");
DEFINE_bool(
    gpu_debug_watch_ringbuffer_exact_unwatch, true,
    "Debug: Request exact page unwatching for ring-buffer write-watch hits. "
    "Keeps watch coverage tighter after each fault.",
    "GPU");
DEFINE_bool(
    gpu_debug_watch_ringbuffer_rearm_on_hit, true,
    "Debug: Request immediate ring-buffer watch re-arm after each watch hit.",
    "GPU");
DEFINE_bool(
    gpu_debug_watch_ringbuffer_log_protection, true,
    "Debug: Log ring-buffer guest page protection snapshots before/after "
    "watch arm and on watch hits.",
    "GPU");
DEFINE_bool(
  gpu_debug_watch_ringbuffer_force_rw_fallback, false,
  "Debug: If ring watch arm leaves ring pages non-writable and no host "
  "query visibility is available, force ring pages back to RW/WC to avoid "
  "stuck no-access states during diagnosis.",
  "GPU");
DEFINE_uint32(
  gpu_debug_watch_ringbuffer_force_rw_after_idle_ms, 250,
  "Debug: Minimum idle duration before forcing ring pages back to RW/WC "
  "when watch protection appears stuck.",
  "GPU");
DEFINE_bool(gpu_log_pm4_swap_verbose, false,
            "Debug: Log PM4 XE_SWAP packet details and payload.", "GPU");
DEFINE_int32(
    gpu_log_pm4_swap_every_n, 1,
    "Debug: Log every N PM4 XE_SWAP packets when verbose logging is enabled.",
    "GPU");
DEFINE_bool(gpu_log_pm4_indirect_verbose, false,
            "Debug: Log PM4 indirect buffer packet details and dumps.", "GPU");
DEFINE_int32(gpu_log_pm4_indirect_every_n, 1,
             "Debug: Log every N PM4 indirect buffer packets when verbose "
             "logging is enabled.",
             "GPU");
DEFINE_int32(gpu_log_pm4_indirect_dump_dwords, 32,
             "Debug: Number of dwords to dump from PM4 indirect buffers.",
             "GPU");
DEFINE_bool(gpu_ignore_unimplemented_opcode, true,
            "Debug: Skip unknown PM4 opcodes instead of asserting.", "GPU");
DEFINE_bool(
    gpu_compat_mac_ringbuffer_double_signal, false,
    "Debug compatibility mode (macOS): send extra ring-buffer init signals "
    "and wait/nudge for acknowledgment.",
    "GPU");

DEFINE_bool(clear_memory_page_state, false,
            "Refresh state of memory pages to enable gpu written data. (Use "
            "for 'Team Ninja' Games to fix missing character models)",
            "GPU");

DEFINE_string(
    readback_resolve, "none",
    "Controls CPU readback of render-to-texture resolve results.\n"
    " fast: Read from previous frame (1 frame delay, no GPU stall, slight "
    "performance hit)\n"
    " full: Wait for GPU to finish (accurate but slow, GPU-CPU sync stall)\n"
    " none: Disable readback completely (some games render better without it)",
    "GPU");

UPDATE_from_string(readback_resolve, 2025, 12, 4, 21, "fast");

DEFINE_bool(
    readback_memexport, false,
    "Read data written by memory export in shaders on the CPU. "
    "This may be needed in some games (but many only access exported data on "
    "the GPU, and this flag isn't needed to handle such behavior), but causes "
    "mid-frame synchronization, so it has a huge performance impact.",
    "GPU");

namespace xe {
namespace gpu {

namespace {
uint64_t NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
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

const char* WaitResultToString(xe::threading::WaitResult result) {
  switch (result) {
    case xe::threading::WaitResult::kSuccess:
      return "success";
    case xe::threading::WaitResult::kTimeout:
      return "timeout";
    case xe::threading::WaitResult::kUserCallback:
      return "user_callback";
    case xe::threading::WaitResult::kFailed:
      return "failed";
    case xe::threading::WaitResult::kAbandoned:
      return "abandoned";
    default:
      return "unknown";
  }
}

void DumpRingBufferWindow(const char* label, const uint8_t* buffer,
                          uint32_t ring_dwords, uint32_t start_dword,
                          uint32_t count) {
  if (!buffer || ring_dwords == 0 || count == 0) {
    XELOGI("   {}: <ring buffer unavailable>", label ? label : "ring");
    return;
  }
  uint32_t clamped_count = std::min<uint32_t>(count, ring_dwords);
  XELOGI("   {} (start={}, count={}, ring_dwords={}):", label ? label : "ring",
         start_dword, clamped_count, ring_dwords);
  for (uint32_t i = 0; i < clamped_count; ++i) {
    uint32_t index = (start_dword + i) % ring_dwords;
    uint32_t value =
        xe::load_and_swap<uint32_t>(buffer + index * sizeof(uint32_t));
    XELOGI("      rb[{:04X}] = {:08X}", index, value);
  }
}

void DumpRingBufferSummary(const char* label, const uint8_t* buffer,
                           uint32_t ring_dwords, uint32_t read_ptr,
                           uint32_t write_ptr, uint32_t rb_base,
                           uint32_t rb_size, uint32_t rptr_writeback) {
  XELOGI(
      "   {}: rb_base={:08X} rb_size={:08X} ring_dwords={} "
      "read_ptr={:08X} write_ptr={:08X} rptr_wb={:08X} host={}",
      label ? label : "ring", rb_base, rb_size, ring_dwords, read_ptr,
      write_ptr, rptr_writeback, static_cast<const void*>(buffer));
}

bool IsRingDebugReadLikelySafe(kernel::KernelState* kernel_state,
                               uint32_t rb_base, uint32_t rb_size) {
  if (!kernel_state || rb_base == 0 || rb_size == 0) {
    return false;
  }
  auto* memory = kernel_state->memory();
  if (!memory) {
    return false;
  }
  auto* heap = memory->LookupHeap(rb_base);
  if (!heap) {
    return false;
  }

  auto has_read_access = [&](uint32_t addr) {
    uint32_t guest_protect = 0;
    if (!heap->QueryProtect(addr, &guest_protect)) {
      return false;
    }
    return (guest_protect & kMemoryProtectRead) != 0;
  };

  uint32_t rb_end = rb_base + rb_size - 1;
  uint32_t rb_mid = rb_base + (rb_size / 2);
  return has_read_access(rb_base) && has_read_access(rb_mid) &&
         has_read_access(rb_end);
}

std::pair<uint32_t, uint32_t> RingBufferWriteWatchCallback(
    void* context_ptr, uint32_t physical_address_start, uint32_t length,
    bool exact_range) {
  auto* cp = reinterpret_cast<CommandProcessor*>(context_ptr);
  if (cp) {
    cp->OnRingBufferWriteWatch(physical_address_start, length, exact_range);
  }
  if (cvars::gpu_debug_watch_ringbuffer_exact_unwatch && length != 0) {
    // Keep the watch window tight after a fault so subsequent writes are less
    // likely to go unobserved between re-arms.
    return {physical_address_start, length};
  }
  // No preference - let the memory system decide the unwatch range.
  return {0, UINT32_MAX};
}

uint32_t RingWatchPhaseIdFromName(const char* phase) {
  if (!phase) {
    return 0;
  }
  std::string_view phase_sv(phase);
  if (phase_sv == "watch-arm-before") {
    return 1;
  }
  if (phase_sv == "watch-arm-after") {
    return 2;
  }
  if (phase_sv == "watch-hit-callback") {
    return 3;
  }
  if (phase_sv == "watch-hit-post-unprotect") {
    return 4;
  }
  if (phase_sv == "watch-stuck-check") {
    return 5;
  }
  return 0;
}

const char* RingWatchPhaseNameFromId(uint32_t phase_id) {
  switch (phase_id) {
    case 1:
      return "watch-arm-before";
    case 2:
      return "watch-arm-after";
    case 3:
      return "watch-hit-callback";
    case 4:
      return "watch-hit-post-unprotect";
    case 5:
      return "watch-stuck-check";
    default:
      return "unknown";
  }
}
}  // namespace

// This should be written completely differently with support for different
// types.
void SaveGPUSetting(GPUSetting setting, uint64_t value) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      OVERRIDE_bool(clear_memory_page_state, static_cast<bool>(value));
      break;
    case GPUSetting::ReadbackMemexport:
      OVERRIDE_bool(readback_memexport, static_cast<bool>(value));
      break;
  }
}

bool GetGPUSetting(GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      return cvars::clear_memory_page_state;
    case GPUSetting::ReadbackMemexport:
      return cvars::readback_memexport;
    default:
      return false;
  }
}

ReadbackResolveMode GetReadbackResolveMode() {
  auto mode_str = cvars::readback_resolve;
  if (mode_str == "fast") {
    return ReadbackResolveMode::kFast;
  } else if (mode_str == "full") {
    return ReadbackResolveMode::kFull;
  } else {
    return ReadbackResolveMode::kDisabled;
  }
}

void SetReadbackResolveMode(const std::string& mode) {
  cvars::readback_resolve = mode;
}

void gpu::CommandProcessor::LogWorkerPipelineChecklist(uint64_t loop_id,
                                                       size_t pending_count) {
  pipeline_check_loop_count_.store(loop_id, std::memory_order_relaxed);

  bool worker_ready = worker_setup_complete_.load(std::memory_order_relaxed);
  bool ring_init = ring_buffer_initialized_.load(std::memory_order_relaxed);
  bool init_ack =
      initialization_acknowledged_.load(std::memory_order_relaxed);
  uint32_t write_ptr = write_ptr_index_.load(std::memory_order_relaxed);
  uint32_t read_ptr = read_ptr_index_;
  bool ring_has_work = read_ptr != write_ptr;
  if (ring_has_work) {
    pipeline_check_idle_loop_streak_.store(0, std::memory_order_relaxed);
  } else {
    pipeline_check_idle_loop_streak_.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t idle_streak =
      pipeline_check_idle_loop_streak_.load(std::memory_order_relaxed);

  bool rb_valid = primary_buffer_ptr_ != 0 && primary_buffer_size_ != 0;
  uint32_t ring_dwords =
      primary_buffer_size_ ? (primary_buffer_size_ / sizeof(uint32_t)) : 0;

  GraphicsSystem::PipelinePrereqSnapshot gs_snapshot;
  bool gs_available = graphics_system_ != nullptr;
  if (graphics_system_) {
    gs_snapshot = graphics_system_->GetPipelinePrereqSnapshot();
  }

  uint32_t system_command_buffer =
      graphics_system_ ? graphics_system_->GetSystemCommandBuffer() : 0;
  bool system_command_buffer_set = system_command_buffer != 0;

  kernel::KernelState::UserModuleLoadStats module_stats{};
  bool module_stats_available = kernel_state_ != nullptr;
  if (kernel_state_) {
    module_stats = kernel_state_->GetUserModuleLoadStats();
  }
    bool executable_ready =
      gs_snapshot.executable_module_present &&
      gs_snapshot.executable_module_entry != 0;
    bool module_load_gate_active =
      gs_snapshot.module_load_inflight || !executable_ready;

  uint64_t now_ms = NowMs();
  uint64_t since_module_begin_ms =
      (gs_snapshot.module_last_begin_ms != 0 &&
       now_ms >= gs_snapshot.module_last_begin_ms)
          ? (now_ms - gs_snapshot.module_last_begin_ms)
          : 0;
  uint64_t since_module_progress_ms =
      (gs_snapshot.module_last_progress_ms != 0 &&
       now_ms >= gs_snapshot.module_last_progress_ms)
          ? (now_ms - gs_snapshot.module_last_progress_ms)
          : 0;
  uint64_t since_module_end_ms =
      (gs_snapshot.module_last_end_ms != 0 &&
       now_ms >= gs_snapshot.module_last_end_ms)
          ? (now_ms - gs_snapshot.module_last_end_ms)
          : 0;
  uint64_t callback_ready_age_ms =
      (gs_snapshot.last_interrupt_callback_set_ms != 0 &&
       now_ms >= gs_snapshot.last_interrupt_callback_set_ms)
          ? (now_ms - gs_snapshot.last_interrupt_callback_set_ms)
          : 0;

    auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
    auto find_runtime_stat = [&](uint16_t ordinal)
        -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
      for (const auto& stat : proc_stats.ordinal_runtime_stats) {
        if (stat.ordinal == ordinal) {
          return &stat;
        }
      }
      return nullptr;
    };
    auto runtime_stat_total =
        [](const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* stat)
            -> uint64_t {
      if (!stat) {
        return 0;
      }
      return stat->dynamic_lookup_calls + stat->static_thunk_calls +
             stat->export_entry_calls;
    };
  const auto* stat_01C2 = find_runtime_stat(0x01C2);
  const auto* stat_01C3 = find_runtime_stat(0x01C3);
  const auto* stat_01D5 = find_runtime_stat(0x01D5);
  const auto* stat_025B = find_runtime_stat(0x025B);
  uint64_t dispatch_bridge_activity_total =
      proc_stats.guest_main_dispatch_bridge_calls_total;
  uint64_t callback_runtime_activity_total = runtime_stat_total(stat_01D5);
  uint64_t bootstrap_runtime_activity_total =
      runtime_stat_total(stat_01C2) + runtime_stat_total(stat_01C3) +
      runtime_stat_total(stat_01D5);
  bool producer_runtime_activity_seen = runtime_stat_total(stat_025B) != 0;
  bool bootstrap_runtime_activity_seen =
      bootstrap_runtime_activity_total != 0 ||
      dispatch_bridge_activity_total != 0 ||
      proc_stats.proc_lookup_calls != 0 ||
      proc_stats.proc_lookup_resolved != 0 ||
      proc_stats.proc_lookup_missing != 0;
  bool bootstrap_activity_seen =
      gs_snapshot.vd_initialize_ring_buffer_calls != 0 ||
      gs_snapshot.vd_set_interrupt_callback_calls != 0 ||
      gs_snapshot.vd_swap_calls != 0 ||
      gs_snapshot.mmio_cp_rb_base_guest_writes != 0 ||
      gs_snapshot.mmio_cp_rb_cntl_guest_writes != 0 ||
      gs_snapshot.mmio_cp_rb_wptr_guest_writes != 0 ||
      bootstrap_runtime_activity_seen;
  bool callback_guest_registration_seen =
      gs_snapshot.vd_set_interrupt_callback_calls != 0 ||
      callback_runtime_activity_total != 0;
  bool callback_effective_ready =
      gs_snapshot.interrupt_callback_set && callback_guest_registration_seen;
  auto is_callback_starvation_dispatch_stall =
      [](const std::string& state) -> bool {
    return state == "gpu_bootstrap_entry_loop_before_callback" ||
           state == "title_local_privilege_gate_before_gpu_dispatch" ||
           state == "title_local_busy_loop_without_import_dispatch" ||
           state == "title_local_busy_loop_before_gpu_dispatch" ||
           state == "busy_loop_before_gpu_dispatch";
  };
  auto callback_starvation_dispatch_stall_blocked_at =
      [](const std::string& state) -> const char* {
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
  auto callback_starvation_dispatch_stall_summary =
      [](const std::string& state) -> const char* {
    if (state == "gpu_bootstrap_entry_loop_before_callback") {
      return "guest main thread remains in the early GPU bootstrap entry loop "
             "before callback registration";
    }
    if (state == "title_local_privilege_gate_before_gpu_dispatch") {
      return "guest main thread remains behind a title-local privilege gate "
             "before any loader or GPU dispatch path reaches callback "
             "registration";
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
    return "interrupt callback not registered after bootstrap";
  };
  bool callback_starvation_dispatch_stall =
      is_callback_starvation_dispatch_stall(
          gs_snapshot.guest_main_dispatch_stall_state);
  constexpr uint64_t kStartupPrologHelperGraceWindowMs = 125;
  constexpr uint32_t kStartupPrologHelperGraceRepeatSamples = 6;
  bool startup_prolog_helper_grace_pending =
      gs_snapshot.guest_main_dispatch_stall_state ==
          "startup_prolog_helper_only" &&
      gs_snapshot.guest_main_dispatch_stall_age_ms <
          kStartupPrologHelperGraceWindowMs &&
      gs_snapshot.guest_main_dispatch_stall_repeat_samples <=
          kStartupPrologHelperGraceRepeatSamples;
  const char* xexcheck_runtime_state =
      (proc_stats.xex_check_executable_privilege_export_entry_calls != 0 ||
       proc_stats.xex_check_executable_privilege_calls != 0)
          ? (proc_stats.xex_check_executable_privilege_static_thunk_calls != 0
                 ? "export_entry_observed"
                 : "export_entry_without_static_thunk_observation")
          : (proc_stats.xex_check_executable_privilege_static_thunk_calls != 0
                 ? "static_thunk_without_export_entry"
                 : "no_static_thunk_observed");
  bool privilege_gate_before_xexcheck_entry =
      gs_snapshot.guest_main_dispatch_stall_state ==
          "title_local_privilege_gate_before_gpu_dispatch" &&
      proc_stats.xex_check_executable_privilege_calls == 0;
  bool bridge_only_callback_starvation =
      !startup_prolog_helper_grace_pending &&
      dispatch_bridge_activity_total != 0 &&
      bootstrap_runtime_activity_total == 0 &&
      !producer_runtime_activity_seen &&
      gs_snapshot.vd_initialize_ring_buffer_calls == 0 &&
      gs_snapshot.vd_set_interrupt_callback_calls == 0 &&
      gs_snapshot.vd_swap_calls == 0 &&
      gs_snapshot.mmio_cp_rb_base_guest_writes == 0 &&
      gs_snapshot.mmio_cp_rb_cntl_guest_writes == 0 &&
      gs_snapshot.mmio_cp_rb_wptr_guest_writes == 0 &&
      proc_stats.loader_module_handle_calls_total == 0 &&
      proc_stats.loader_proc_lookup_calls_total == 0 &&
      proc_stats.proc_lookup_calls == 0 &&
      proc_stats.proc_lookup_resolved == 0 &&
      proc_stats.proc_lookup_missing == 0;

  bool ring_work_expected = false;
  std::string ring_work_expectation_reason =
      "waiting for bootstrap prerequisites";
  uint64_t swap_packet_count =
      swap_packet_count_.load(std::memory_order_relaxed);
  uint64_t debug_host_swap_count =
      debug_host_swap_injection_count_.load(std::memory_order_relaxed);
  bool guest_swap_packet_seen =
      swap_packet_count > debug_host_swap_count;
  bool producer_entry_seen =
      gs_snapshot.vd_swap_calls != 0 || producer_runtime_activity_seen ||
      guest_swap_packet_seen || gs_snapshot.mmio_cp_rb_wptr_guest_writes != 0;
  bool producer_entry_overdue =
      callback_effective_ready && !producer_entry_seen &&
      callback_ready_age_ms >=
          static_cast<uint64_t>(cvars::gpu_producer_entry_grace_ms);
  if (!worker_ready) {
    ring_work_expectation_reason = "worker setup incomplete";
  } else if (!ring_init || !init_ack || !rb_valid) {
    ring_work_expectation_reason = "ring handshake incomplete";
  } else if (!gs_snapshot.executable_module_present) {
    ring_work_expectation_reason = "executable module not visible yet";
  } else if (gs_snapshot.executable_module_entry == 0) {
    ring_work_expectation_reason = "executable entry unresolved (entry=0)";
  } else if (!gs_snapshot.guest_main_thread_ready) {
    ring_work_expectation_reason = "guest main thread not ready";
  } else if (!gs_snapshot.guest_main_thread_running) {
    ring_work_expectation_reason = "guest main thread not running";
  } else if (!bootstrap_activity_seen) {
    ring_work_expectation_reason = "bootstrap activity not observed yet";
  } else if (!gs_snapshot.interrupt_callback_set) {
    if (privilege_gate_before_xexcheck_entry) {
      if (proc_stats.xex_check_executable_privilege_static_thunk_calls != 0) {
        ring_work_expectation_reason = fmt::format(
            "guest main thread remains behind a title-local privilege gate "
            "after hitting the XexCheckExecutablePrivilege static thunk {} "
            "time(s), but before export entry or any loader/GPU dispatch "
            "path (stall_age={}ms repeat_samples={})",
            proc_stats.xex_check_executable_privilege_static_thunk_calls,
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples);
      } else {
        ring_work_expectation_reason = fmt::format(
            "guest main thread remains behind a title-local privilege gate "
            "before entering XexCheckExecutablePrivilege or any loader/GPU "
            "dispatch path (stall_age={}ms repeat_samples={})",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples);
      }
    } else if (callback_starvation_dispatch_stall) {
      ring_work_expectation_reason = fmt::format(
          "{} (stall_age={}ms repeat_samples={})",
          callback_starvation_dispatch_stall_summary(
              gs_snapshot.guest_main_dispatch_stall_state),
          gs_snapshot.guest_main_dispatch_stall_age_ms,
          gs_snapshot.guest_main_dispatch_stall_repeat_samples);
    } else if (startup_prolog_helper_grace_pending) {
      ring_work_expectation_reason = fmt::format(
          "guest main thread has only reached the initial startup prolog "
          "window after processor dispatch entry "
          "(stall_age={}ms repeat_samples={}); callback starvation diagnosis "
          "is deferred until that window persists",
          gs_snapshot.guest_main_dispatch_stall_age_ms,
          gs_snapshot.guest_main_dispatch_stall_repeat_samples);
    } else if (gs_snapshot.guest_main_dispatch_stall_state ==
               "startup_prolog_helper_only") {
      ring_work_expectation_reason = fmt::format(
          "guest main thread remains in same-module startup prolog helper "
          "dispatch after processor dispatch entry "
          "(stall_age={}ms repeat_samples={}); loader/callback/MMIO "
          "diagnosis is blocked until that helper path clears",
          gs_snapshot.guest_main_dispatch_stall_age_ms,
          gs_snapshot.guest_main_dispatch_stall_repeat_samples);
    } else if (bridge_only_callback_starvation) {
      ring_work_expectation_reason = fmt::format(
          "guest dispatch bridge observed without loader/callback/MMIO "
          "follow-through (guest_main_dispatch_bridge_total={} "
          "stall_state={} stall_age={}ms repeat_samples={})",
          dispatch_bridge_activity_total,
          gs_snapshot.guest_main_dispatch_stall_state.empty()
              ? "<none>"
              : gs_snapshot.guest_main_dispatch_stall_state,
          gs_snapshot.guest_main_dispatch_stall_age_ms,
          gs_snapshot.guest_main_dispatch_stall_repeat_samples);
    } else {
      ring_work_expectation_reason =
          "interrupt callback not registered after bootstrap";
    }
  } else if (!callback_guest_registration_seen) {
    ring_work_expectation_reason =
        "interrupt callback is set but guest callback registration "
        "(VdSetGraphicsInterruptCallback/runtime 0x01D5) not observed";
  } else if (!producer_entry_seen && !producer_entry_overdue) {
    ring_work_expectation_reason =
      "producer path not entered yet (no VdSwap/runtime ordinal 0x025B/guest-MMIO-WPTR activity)";
  } else if (!producer_entry_seen && producer_entry_overdue) {
    ring_work_expected = true;
    ring_work_expectation_reason = fmt::format(
      "producer entry overdue (no VdSwap/runtime ordinal 0x025B/guest-MMIO-WPTR "
      "activity after callback-ready age={}ms)",
      callback_ready_age_ms);
  } else {
    ring_work_expected = true;
    ring_work_expectation_reason =
        "guest main running with bootstrap+callback ready";
  }

  const char* blocked_at = "none";
  std::string blocked_detail = "pipeline prerequisites look satisfied";
  if (!worker_ready) {
    blocked_at = "worker_setup";
    blocked_detail = "worker setup not complete";
  } else if (!ring_init || !init_ack || !rb_valid) {
    blocked_at = "ring_handshake";
    blocked_detail = fmt::format(
        "ring_init={} init_ack={} rb_valid={} rb_base={:08X} rb_size={:08X}",
        ring_init ? "YES" : "NO", init_ack ? "YES" : "NO",
        rb_valid ? "YES" : "NO", primary_buffer_ptr_, primary_buffer_size_);
  } else if (!gs_snapshot.executable_module_present) {
    blocked_at = "executable_module_visibility";
    blocked_detail = fmt::format(
        "executable module not visible yet (load_inflight={} load_calls={} "
        "load_progress_count={})",
        gs_snapshot.module_load_inflight ? "YES" : "NO",
        gs_snapshot.module_load_calls, gs_snapshot.module_load_progress_count);
  } else if (gs_snapshot.executable_module_entry == 0) {
    blocked_at = "executable_entry_unresolved";
    blocked_detail = fmt::format(
        "executable module entry is zero (load_inflight={} "
        "loaded_exec_modules={} since_load_end={}ms)",
        gs_snapshot.module_load_inflight ? "YES" : "NO",
        gs_snapshot.loaded_executable_user_modules,
        since_module_end_ms);
  } else if (gs_snapshot.module_load_inflight) {
    blocked_at = "module_load_inflight";
    blocked_detail = fmt::format(
        "load still inflight (load_calls={} load_progress_count={} "
        "since_load_begin={}ms since_load_progress={}ms)",
        gs_snapshot.module_load_calls,
        gs_snapshot.module_load_progress_count, since_module_begin_ms,
        since_module_progress_ms);
  } else if (!gs_snapshot.guest_main_thread_ready) {
    blocked_at = "guest_main_not_ready";
    blocked_detail = "emulator has no guest main thread yet";
  } else if (!gs_snapshot.guest_main_thread_running) {
    blocked_at = "guest_main_not_running";
    blocked_detail = "guest main thread exists but is not running";
  } else if (gs_snapshot.vd_initialize_ring_buffer_calls == 0 &&
             gs_snapshot.vd_set_interrupt_callback_calls == 0 &&
             gs_snapshot.vd_swap_calls == 0 &&
             gs_snapshot.mmio_cp_rb_base_guest_writes == 0 &&
             gs_snapshot.mmio_cp_rb_cntl_guest_writes == 0 &&
             gs_snapshot.mmio_cp_rb_wptr_guest_writes == 0 &&
             !bootstrap_runtime_activity_seen) {
    blocked_at = "bootstrap_calls_missing";
    blocked_detail = fmt::format(
        "no Vd/MMIO/bootstrap-runtime activity seen (no_force_profile={} "
        "bootstrap_nudge_count={} dead_pipeline_recovery_count={} "
        "runtime_bootstrap_total={} guest_main_dispatch_bridge_total={} "
        "proc_lookup_calls={})",
        gs_snapshot.no_force_profile ? "ON" : "OFF",
        gs_snapshot.prelaunch_bootstrap_nudge_count,
        gs_snapshot.dead_pipeline_recovery_count,
        bootstrap_runtime_activity_total, dispatch_bridge_activity_total,
        proc_stats.proc_lookup_calls);
  } else if (!gs_snapshot.interrupt_callback_set &&
             callback_starvation_dispatch_stall) {
    blocked_at = callback_starvation_dispatch_stall_blocked_at(
        gs_snapshot.guest_main_dispatch_stall_state);
    blocked_detail = fmt::format(
        "{} (stall_age={}ms repeat_samples={} guest_main_dispatch_bridge_total={} "
        "loader_module_handle_calls_total={} loader_proc_lookup_calls_total={})",
        callback_starvation_dispatch_stall_summary(
            gs_snapshot.guest_main_dispatch_stall_state),
        gs_snapshot.guest_main_dispatch_stall_age_ms,
        gs_snapshot.guest_main_dispatch_stall_repeat_samples,
        dispatch_bridge_activity_total,
        proc_stats.loader_module_handle_calls_total,
        proc_stats.loader_proc_lookup_calls_total);
  } else if (!gs_snapshot.interrupt_callback_set &&
             gs_snapshot.guest_main_dispatch_stall_state ==
                 "startup_prolog_helper_only") {
    blocked_at = "startup_prolog_helper_only";
    blocked_detail = fmt::format(
        "guest main thread remains in same-module startup prolog helper "
        "dispatch after processor dispatch entry "
        "(stall_age={}ms repeat_samples={} guest_main_dispatch_bridge_total={} "
        "loader_module_handle_calls_total={} loader_proc_lookup_calls_total={})",
        gs_snapshot.guest_main_dispatch_stall_age_ms,
        gs_snapshot.guest_main_dispatch_stall_repeat_samples,
        dispatch_bridge_activity_total,
        proc_stats.loader_module_handle_calls_total,
        proc_stats.loader_proc_lookup_calls_total);
  } else if (!gs_snapshot.interrupt_callback_set &&
             bridge_only_callback_starvation) {
    blocked_at = "guest_dispatch_bridge_callback_starvation";
    blocked_detail = fmt::format(
        "guest dispatch bridge executed, but no loader, callback, MMIO, or "
        "tracked runtime GPU progress followed "
        "(guest_main_dispatch_bridge_total={} stall_state={} stall_age={}ms "
        "repeat_samples={} loader_module_handle_calls_total={} "
        "loader_proc_lookup_calls_total={})",
        dispatch_bridge_activity_total,
        gs_snapshot.guest_main_dispatch_stall_state.empty()
            ? "<none>"
            : gs_snapshot.guest_main_dispatch_stall_state,
        gs_snapshot.guest_main_dispatch_stall_age_ms,
        gs_snapshot.guest_main_dispatch_stall_repeat_samples,
        proc_stats.loader_module_handle_calls_total,
        proc_stats.loader_proc_lookup_calls_total);
  } else if (!gs_snapshot.interrupt_callback_set) {
    blocked_at = "interrupt_callback";
    blocked_detail = fmt::format(
        "callback unset after bootstrap activity (auto_callback_recovery_count={})",
        gs_snapshot.auto_callback_recovery_count);
  } else if (!callback_guest_registration_seen) {
    blocked_at = "interrupt_callback_untrusted";
    blocked_detail = fmt::format(
        "callback is set but guest callback registration is missing "
        "(vd_set_cb_calls={} runtime_01D5_total={})",
        gs_snapshot.vd_set_interrupt_callback_calls,
        callback_runtime_activity_total);
  } else if (!producer_entry_seen) {
    blocked_at = "producer_entry_missing";
    blocked_detail = fmt::format(
        "bootstrap+callback ready but producer call path not entered yet "
      "(vd_swap_calls={} runtime_025B_seen={} mmio_wptr_writes={} "
      "swap_packets={} debug_host_swaps={} guest_swap_packets={})",
        gs_snapshot.vd_swap_calls,
        producer_runtime_activity_seen ? "YES" : "NO",
      gs_snapshot.mmio_cp_rb_wptr_guest_writes,
      swap_packet_count, debug_host_swap_count,
      guest_swap_packet_seen ? "YES" : "NO");
  } else if (!ring_has_work) {
    blocked_at = "producer_progress";
    blocked_detail = fmt::format(
        "read==write=={:08X} idle_streak={} swap_packets={}", read_ptr,
        idle_streak, swap_packet_count_.load(std::memory_order_relaxed));
  }

  XELOGI(
      "DEBUG: GPU PIPELINE CHECK #{}: worker_ready={} ring_init={} init_ack={} "
      "rb_valid={} rb_base={:08X} rb_size={:08X} ring_dwords={} "
      "read={:08X} write={:08X} pending_fns={} has_work={} idle_streak={} "
      "callback_set={} callback_guest_registered={} callback_ready={} "
      "callback={:08X} interrupt_seen={} "
      "sys_cmd_buffer_set={} sys_cmd_buffer={:08X} "
      "vd_init_rb={} vd_set_cb={} vd_swap={} mmio_base_guest={} "
      "mmio_cntl_guest={} mmio_wptr_guest={} no_force_profile={} "
      "force_fallback_gate={} "
      "module_load_gate_active={} module_calls={} module_success={} "
      "module_fail={} module_progress_count={} loaded_user_modules={} "
      "loaded_exec_modules={} executable_module_present={} entry={:08X} "
      "guest_main_ready={} guest_main_running={} guest_main_resume_nudges={} "
      "guest_main_dispatch_bridge_total={} "
      "guest_main_dispatch_stall_state={} "
      "guest_main_dispatch_stall_age_ms={} "
      "guest_main_dispatch_stall_repeat_samples={} "
      "loader_module_handle_calls_total={} "
      "loader_proc_lookup_calls_total={} "
      "xexcheck_static_thunk_calls={} "
      "xexcheck_export_entry_calls={} "
      "xexcheck_runtime_state={} "
      "ring_work_expected={} "
      "ring_work_reason='{}'",
      loop_id, worker_ready ? "YES" : "NO", ring_init ? "YES" : "NO",
      init_ack ? "YES" : "NO", rb_valid ? "YES" : "NO",
      primary_buffer_ptr_, primary_buffer_size_, ring_dwords, read_ptr,
      write_ptr, pending_count, ring_has_work ? "YES" : "NO", idle_streak,
      gs_snapshot.interrupt_callback_set ? "YES" : "NO",
      callback_guest_registration_seen ? "YES" : "NO",
      callback_effective_ready ? "YES" : "NO",
      gs_snapshot.interrupt_callback,
      gs_snapshot.interrupt_dispatch_seen ? "YES" : "NO",
      system_command_buffer_set ? "YES" : "NO", system_command_buffer,
      gs_snapshot.vd_initialize_ring_buffer_calls,
      gs_snapshot.vd_set_interrupt_callback_calls, gs_snapshot.vd_swap_calls,
      gs_snapshot.mmio_cp_rb_base_guest_writes,
      gs_snapshot.mmio_cp_rb_cntl_guest_writes,
      gs_snapshot.mmio_cp_rb_wptr_guest_writes,
      gs_snapshot.no_force_profile ? "ON" : "OFF",
      gs_snapshot.suspend_force_fallback_until_module_ready ? "ON" : "OFF",
        module_load_gate_active ? "YES" : "NO", gs_snapshot.module_load_calls,
        gs_snapshot.module_load_success, gs_snapshot.module_load_fail,
        gs_snapshot.module_load_progress_count, gs_snapshot.loaded_user_modules,
        gs_snapshot.loaded_executable_user_modules,
        gs_snapshot.executable_module_present ? "YES" : "NO",
        gs_snapshot.executable_module_entry,
        gs_snapshot.guest_main_thread_ready ? "YES" : "NO",
        gs_snapshot.guest_main_thread_running ? "YES" : "NO",
        gs_snapshot.guest_main_resume_nudge_count,
        dispatch_bridge_activity_total,
        gs_snapshot.guest_main_dispatch_stall_state.empty()
            ? "<none>"
            : gs_snapshot.guest_main_dispatch_stall_state,
        gs_snapshot.guest_main_dispatch_stall_age_ms,
        gs_snapshot.guest_main_dispatch_stall_repeat_samples,
        proc_stats.loader_module_handle_calls_total,
        proc_stats.loader_proc_lookup_calls_total,
        proc_stats.xex_check_executable_privilege_static_thunk_calls,
        proc_stats.xex_check_executable_privilege_export_entry_calls,
        xexcheck_runtime_state,
        ring_work_expected ? "YES" : "NO", ring_work_expectation_reason);
  XELOGW("DEBUG: PIPELINE_BLOCKED_AT={} ({})", blocked_at, blocked_detail);

  bool allow_loop_forcing =
      cvars::gpu_worker_allow_loop_forcing && !gs_snapshot.no_force_profile;
  bool blocked_on_interrupt_callback =
      worker_ready && ring_init && init_ack && rb_valid &&
      gs_snapshot.guest_main_thread_running && bootstrap_activity_seen &&
      !callback_effective_ready;
  if (blocked_on_interrupt_callback && graphics_system_) {
    if (allow_loop_forcing) {
      static std::atomic<uint64_t> callback_recovery_last_ms{0};
      uint64_t last_recovery_ms =
          callback_recovery_last_ms.load(std::memory_order_relaxed);
      if (last_recovery_ms == 0 || now_ms < last_recovery_ms ||
          now_ms - last_recovery_ms >= 1000) {
        callback_recovery_last_ms.store(now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: PIPELINE callback-recovery nudge: blocked at interrupt_callback "
            "with bootstrap activity observed (idle_streak={} read={:08X} "
            "write={:08X})",
            idle_streak, read_ptr, write_ptr);
        graphics_system_->AttemptFallbackInitialization();
      }

      if (!ring_has_work && read_ptr == 0 && write_ptr == 0 &&
          reader_.buffer() && ring_dwords > 0) {
        static std::atomic<bool> callback_block_seed_done{false};
        bool seed_expected = false;
        if (callback_block_seed_done.compare_exchange_strong(
                seed_expected, true, std::memory_order_relaxed)) {
          constexpr uint32_t kPacketType2Nop = (0x2u << 30);
          auto* dst = reinterpret_cast<uint32_t*>(reader_.buffer());
          xe::store_and_swap<uint32_t>(dst, kPacketType2Nop);
          UpdateWritePointer(1, "pipeline-callback-block-seed");
          XELOGW(
              "DEBUG: PIPELINE ring-seed nudge: wrote one PM4 Type2 no-op at "
              "rb[0] and advanced write_ptr to 1 while callback recovery is "
              "pending");
        }
      }
    } else {
      static std::atomic<uint64_t> callback_suppressed_last_ms{0};
      uint64_t last_log_ms =
          callback_suppressed_last_ms.load(std::memory_order_relaxed);
      if (last_log_ms == 0 || now_ms < last_log_ms ||
          now_ms - last_log_ms >= 1000) {
        callback_suppressed_last_ms.store(now_ms, std::memory_order_relaxed);
        XELOGW(
            "DEBUG: PIPELINE callback recovery suppressed by "
            "loop forcing policy (read={:08X} write={:08X} "
            "idle_streak={} no_force_profile={} allow_loop_forcing={})",
            read_ptr, write_ptr, idle_streak,
            gs_snapshot.no_force_profile ? "YES" : "NO",
            cvars::gpu_worker_allow_loop_forcing ? "YES" : "NO");
      }
    }
  }

  bool blocked_on_producer_progress =
      worker_ready && ring_init && init_ack && rb_valid &&
      gs_snapshot.guest_main_thread_running && bootstrap_activity_seen &&
      gs_snapshot.interrupt_callback_set && producer_entry_seen &&
      !ring_has_work;
  if (blocked_on_producer_progress &&
      swap_packet_count_.load(std::memory_order_relaxed) == 0) {
    if (allow_loop_forcing) {
      static std::atomic<uint64_t> producer_swap_nudge_last_ms{0};
      static std::atomic<uint64_t> producer_swap_nudge_count{0};
      uint64_t last_nudge_ms =
          producer_swap_nudge_last_ms.load(std::memory_order_relaxed);
      bool nudge_due = idle_streak >= 120 &&
                       (last_nudge_ms == 0 || now_ms < last_nudge_ms ||
                        now_ms - last_nudge_ms >= 1000);
      if (nudge_due) {
        producer_swap_nudge_last_ms.store(now_ms, std::memory_order_relaxed);
        uint64_t nudge_id =
            producer_swap_nudge_count.fetch_add(1, std::memory_order_relaxed) +
            1;
        uint32_t nudge_width = last_swap_width_.load(std::memory_order_relaxed);
        uint32_t nudge_height =
            last_swap_height_.load(std::memory_order_relaxed);
        if (nudge_width == 0 || nudge_height == 0) {
          nudge_width = 1280;
          nudge_height = 720;
        }
        XELOGW(
            "DEBUG: PIPELINE producer-progress nudge #{}: blocked at "
            "producer_progress with zero swaps; issuing host swap nudge "
            "{}x{} (read={:08X} write={:08X} idle_streak={})",
            nudge_id, nudge_width, nudge_height, read_ptr, write_ptr,
            idle_streak);
        DebugIssueSwapFromHost(nudge_width, nudge_height);
      }
    } else if (idle_streak >= 120) {
      static std::atomic<uint64_t> producer_progress_suppressed_last_ms{0};
      uint64_t last_log_ms =
          producer_progress_suppressed_last_ms.load(std::memory_order_relaxed);
      if (last_log_ms == 0 || now_ms < last_log_ms ||
          now_ms - last_log_ms >= 1000) {
        producer_progress_suppressed_last_ms.store(now_ms,
                                                   std::memory_order_relaxed);
        XELOGW(
            "DEBUG: PIPELINE producer-progress nudge suppressed by "
            "loop forcing policy (read={:08X} write={:08X} "
            "idle_streak={} swap_packets={} no_force_profile={} "
            "allow_loop_forcing={})",
            read_ptr, write_ptr, idle_streak,
            swap_packet_count_.load(std::memory_order_relaxed),
            gs_snapshot.no_force_profile ? "YES" : "NO",
            cvars::gpu_worker_allow_loop_forcing ? "YES" : "NO");
      }
    }
  }

  bool blocked_on_producer_entry_missing =
      worker_ready && ring_init && init_ack && rb_valid &&
      gs_snapshot.guest_main_thread_running && bootstrap_activity_seen &&
      gs_snapshot.interrupt_callback_set && !producer_entry_seen;
  static std::atomic<uint64_t> producer_entry_block_start_ms{0};
  if (blocked_on_producer_entry_missing) {
    static std::atomic<uint64_t> producer_entry_nudge_last_ms{0};
    static std::atomic<uint64_t> producer_entry_nudge_count{0};
    uint64_t block_start_ms =
      producer_entry_block_start_ms.load(std::memory_order_relaxed);
    if (block_start_ms == 0 || now_ms < block_start_ms) {
      producer_entry_block_start_ms.store(now_ms, std::memory_order_relaxed);
      block_start_ms = now_ms;
    }
    uint64_t blocked_age_ms = now_ms - block_start_ms;
    uint64_t last_nudge_ms =
        producer_entry_nudge_last_ms.load(std::memory_order_relaxed);
    bool nudge_due =
             blocked_age_ms >=
               static_cast<uint64_t>(
                 cvars::gpu_producer_entry_nudge_after_ms) &&
                     (last_nudge_ms == 0 || now_ms < last_nudge_ms ||
              now_ms - last_nudge_ms >=
                static_cast<uint64_t>(
                  cvars::gpu_producer_entry_nudge_interval_ms));
    if (nudge_due) {
      if (allow_loop_forcing) {
        producer_entry_nudge_last_ms.store(now_ms, std::memory_order_relaxed);
        uint64_t nudge_id =
            producer_entry_nudge_count.fetch_add(
                1, std::memory_order_relaxed) +
            1;
        uint32_t nudge_width = last_swap_width_.load(std::memory_order_relaxed);
        uint32_t nudge_height =
            last_swap_height_.load(std::memory_order_relaxed);
        if (nudge_width == 0 || nudge_height == 0) {
          nudge_width = 1280;
          nudge_height = 720;
        }
        XELOGW(
            "DEBUG: PIPELINE producer-entry nudge #{}: bootstrap+callback ready "
            "but no producer path activity yet; issuing host swap nudge "
            "{}x{} (read={:08X} write={:08X} idle_streak={} blocked_age={}ms "
            "callback_ready_age={}ms)",
            nudge_id, nudge_width, nudge_height, read_ptr, write_ptr,
            idle_streak, blocked_age_ms, callback_ready_age_ms);
        DebugIssueSwapFromHost(nudge_width, nudge_height);
      } else {
        static std::atomic<uint64_t> producer_entry_suppressed_last_ms{0};
        uint64_t last_log_ms =
            producer_entry_suppressed_last_ms.load(
                std::memory_order_relaxed);
        if (last_log_ms == 0 || now_ms < last_log_ms ||
            now_ms - last_log_ms >= 1000) {
          producer_entry_suppressed_last_ms.store(
              now_ms, std::memory_order_relaxed);
          XELOGW(
              "DEBUG: PIPELINE producer-entry nudge suppressed by "
              "loop forcing policy (blocked_age={}ms "
              "callback_ready_age={}ms read={:08X} write={:08X} "
              "no_force_profile={} allow_loop_forcing={})",
              blocked_age_ms, callback_ready_age_ms, read_ptr, write_ptr,
              gs_snapshot.no_force_profile ? "YES" : "NO",
              cvars::gpu_worker_allow_loop_forcing ? "YES" : "NO");
        }
      }
    }
  } else {
    producer_entry_block_start_ms.store(0, std::memory_order_relaxed);
  }

  auto log_missing = [&](const char* requirement, const std::string& detail) {
    XELOGW("DEBUG: MISSING: {} ({})", requirement, detail);
  };

  if (!gs_available) {
    log_missing("graphics_system_link", "GraphicsSystem pointer is null");
    return;
  }
  if (!worker_ready) {
    log_missing("worker_setup_complete", "worker setup is not complete");
  }
  if (!ring_init) {
    log_missing("ring_buffer_initialized", "ring init flag is false");
  }
  if (!init_ack) {
    log_missing("ring_init_acknowledged", "worker has not acked init event");
  }
  if (!rb_valid) {
    log_missing("ring_buffer_base_and_size",
                fmt::format("rb_base={:08X} rb_size={:08X}",
                            primary_buffer_ptr_, primary_buffer_size_));
  }
  if (gs_snapshot.mmio_cp_rb_base_guest_writes == 0) {
    log_missing("CP_RB_BASE_write_observed", "no MMIO base writes seen");
  }
  if (gs_snapshot.mmio_cp_rb_cntl_guest_writes == 0) {
    log_missing("CP_RB_CNTL_write_observed", "no MMIO cntl writes seen");
  }
  if (gs_snapshot.mmio_cp_rb_wptr_guest_writes == 0) {
    log_missing("CP_RB_WPTR_write_observed", "no MMIO wptr writes seen");
  }
  if (gs_snapshot.vd_initialize_ring_buffer_calls == 0 &&
      gs_snapshot.mmio_cp_rb_base_guest_writes == 0) {
    if (callback_starvation_dispatch_stall || bridge_only_callback_starvation) {
      log_missing(
          "VdInitializeRingBuffer_call_or_MMIO",
          fmt::format(
              "guest did not call VdInitializeRingBuffer and no base MMIO "
              "writes followed the dispatch bridge (stall_state={} "
              "stall_age={}ms repeat_samples={})",
              gs_snapshot.guest_main_dispatch_stall_state.empty()
                  ? "<none>"
                  : gs_snapshot.guest_main_dispatch_stall_state,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples));
    } else {
      log_missing("VdInitializeRingBuffer_call_or_MMIO",
                  "guest did not call VdInitializeRingBuffer and no base MMIO writes");
    }
  }
  if (gs_snapshot.vd_set_interrupt_callback_calls == 0 && !gs_snapshot.interrupt_callback_set) {
    if (callback_starvation_dispatch_stall || bridge_only_callback_starvation) {
      log_missing(
          "VdSetGraphicsInterruptCallback_call_observed",
          fmt::format(
              "guest did not call VdSetGraphicsInterruptCallback after the "
              "dispatch bridge (stall_state={} stall_age={}ms "
              "repeat_samples={})",
              gs_snapshot.guest_main_dispatch_stall_state.empty()
                  ? "<none>"
                  : gs_snapshot.guest_main_dispatch_stall_state,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples));
    } else {
      log_missing("VdSetGraphicsInterruptCallback_call_observed",
                  "guest did not call VdSetGraphicsInterruptCallback");
    }
  }
  if (privilege_gate_before_xexcheck_entry) {
    if (proc_stats.xex_check_executable_privilege_static_thunk_calls != 0) {
      log_missing(
          "XexCheckExecutablePrivilege_export_entry_observed",
          fmt::format(
              "branch scans show the title-local privilege gate before any "
              "loader or GPU dispatch path; the guest hit the "
              "XexCheckExecutablePrivilege static thunk {} time(s), but the "
              "export body has not been entered yet (stall_age={}ms "
              "repeat_samples={})",
              proc_stats.xex_check_executable_privilege_static_thunk_calls,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples));
    } else {
      log_missing(
          "XexCheckExecutablePrivilege_static_thunk_observed",
          fmt::format(
              "branch scans show the title-local privilege gate before any "
              "loader or GPU dispatch path, but the guest has not reached the "
              "XexCheckExecutablePrivilege static thunk yet (stall_age={}ms "
              "repeat_samples={})",
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples));
    }
  }
  if (gs_snapshot.vd_swap_calls == 0 &&
      swap_packet_count_.load(std::memory_order_relaxed) == 0 &&
      gs_snapshot.mmio_cp_rb_wptr_guest_writes == 0) {
    if (callback_starvation_dispatch_stall || bridge_only_callback_starvation) {
      log_missing(
          "VdSwap_call_or_MMIO_write",
          fmt::format(
              "guest did not call VdSwap and no WPTR writes followed the "
              "dispatch bridge (stall_state={} stall_age={}ms "
              "repeat_samples={})",
              gs_snapshot.guest_main_dispatch_stall_state.empty()
                  ? "<none>"
                  : gs_snapshot.guest_main_dispatch_stall_state,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples));
    } else {
      log_missing("VdSwap_call_or_MMIO_write",
                  "guest did not call VdSwap and no WPTR writes");
    }
  }
  if (!gs_snapshot.interrupt_callback_set) {
    log_missing("interrupt_callback_registered",
                "interrupt callback remains unset");
  }
  if (!gs_snapshot.interrupt_dispatch_seen) {
    log_missing("interrupt_callback_dispatch_observed",
                "no callback dispatch observed yet");
  }
  if (!system_command_buffer_set) {
    log_missing("system_command_buffer_ready",
                "GetSystemCommandBuffer is zero");
  }
  if (!ring_has_work) {
    if (ring_work_expected) {
      log_missing("ring_producer_progress",
                  fmt::format("read==write=={:08X} idle_streak={}", read_ptr,
                              idle_streak));
    } else {
      XELOGI(
          "DEBUG: NOTE: ring has no work yet and this is expected currently "
          "({})",
          ring_work_expectation_reason);
    }
  }
  if (swap_packet_count_.load(std::memory_order_relaxed) == 0) {
    if (ring_work_expected) {
      log_missing("swap_packet_observed",
                  "command processor swap count is zero");
    } else {
      XELOGI(
          "DEBUG: NOTE: zero swap packets currently expected ({})",
          ring_work_expectation_reason);
    }
  }
  if (module_load_gate_active) {
    log_missing("module_load_gate_clear",
                fmt::format(
                    "load_inflight={} executable_ready={} "
                    "loaded_executable_user_modules={} load_success={} "
                    "load_fail={}",
                    gs_snapshot.module_load_inflight ? "YES" : "NO",
                    executable_ready ? "YES" : "NO",
                    module_stats.loaded_executable_user_modules,
                    module_stats.load_success, module_stats.load_fail));
  }
  if (!gs_snapshot.user_module_load_event_present) {
    log_missing("module_load_event_registered",
                "KernelState user_module_load_event is null");
  }
  if (!gs_snapshot.executable_module_present) {
    log_missing("executable_module_visible",
                "KernelState::GetExecutableModule returned null");
  } else if (gs_snapshot.executable_module_entry == 0) {
    log_missing("executable_module_entry_ready",
                "executable module entry point is zero");
  }
  if (gs_snapshot.module_load_calls == 0 &&
      gs_snapshot.loaded_user_modules == 0) {
    log_missing("LoadUserModule_activity_observed",
                "no user module loads observed yet");
  }
  if (gs_snapshot.module_load_inflight &&
      gs_snapshot.module_load_progress_count == 0) {
    log_missing("LoadUserModule_progress_observed",
                fmt::format("inflight with no progress; since_load_begin={}ms",
                            since_module_begin_ms));
  }
  if (gs_snapshot.module_last_end_ms != 0 &&
      gs_snapshot.module_load_success == 0 &&
      gs_snapshot.module_load_fail == 0) {
    log_missing("LoadUserModule_result_accounted",
                fmt::format(
                    "load end observed {}ms ago but success/fail counters are zero",
                    since_module_end_ms));
  }
  if (!gs_snapshot.guest_main_thread_ready) {
    log_missing("guest_main_thread_ready",
                "Emulator::HasGuestMainThread returned false");
  } else if (!gs_snapshot.guest_main_thread_running) {
    log_missing("guest_main_thread_running",
                "guest main thread exists but IsGuestMainThreadRunning is false");
  }
}

void gpu::CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    worker_setup_failed_.store(true, std::memory_order_relaxed);
    if (worker_setup_complete_event_) {
      worker_setup_complete_event_->Set();
    }
    XELOGE("RING BUFFER: worker setup failed (SetupContext returned false)");
    return;
  }

  worker_setup_complete_.store(true, std::memory_order_relaxed);
  if (worker_setup_complete_event_) {
    worker_setup_complete_event_->Set();
  }
  XELOGI(
      "RING BUFFER: worker setup complete (SetupContext succeeded, entering "
      "worker loop)");

  if (cvars::gpu_worker_preloop_bootstrap_gate) {
    uint64_t gate_wait_ms = static_cast<uint64_t>(std::max<uint32_t>(
        1, cvars::gpu_worker_preloop_bootstrap_gate_wait_ms));
    uint64_t gate_log_interval_ms = static_cast<uint64_t>(std::max<uint32_t>(
        1, cvars::gpu_worker_preloop_bootstrap_gate_log_interval_ms));
    uint64_t gate_timeout_ms =
        static_cast<uint64_t>(cvars::gpu_worker_preloop_bootstrap_gate_timeout_ms);
    bool require_producer_entry =
        cvars::gpu_worker_preloop_bootstrap_gate_require_producer_entry;

    uint64_t gate_start_ms = NowMs();
    uint64_t gate_last_log_ms = 0;
    uint64_t gate_iteration = 0;

    XELOGI(
        "RING BUFFER: pre-loop bootstrap gate active "
        "(wait_ms={} log_interval_ms={} timeout_ms={} "
        "require_producer_entry={})",
        gate_wait_ms, gate_log_interval_ms, gate_timeout_ms,
        require_producer_entry ? "YES" : "NO");

    while (worker_running_) {
      ++gate_iteration;
      uint64_t now_ms = NowMs();

      size_t gate_pending_count = 0;
      std::string gate_pending_head_tag;
      uint64_t gate_pending_head_age_ms = 0;
      {
        std::lock_guard<std::mutex> pending_lock(pending_fns_mutex_);
        gate_pending_count = pending_fns_.size();
        if (!pending_fns_.empty()) {
          const auto& pending = pending_fns_.front();
          gate_pending_head_tag = pending.tag;
          gate_pending_head_age_ms =
              (pending.enqueue_ms && now_ms >= pending.enqueue_ms)
                  ? (now_ms - pending.enqueue_ms)
                  : 0;
        }
      }
      if (gate_pending_count != 0) {
        XELOGI(
            "RING BUFFER: pre-loop bootstrap gate executing pending functions "
            "(iteration={} elapsed={}ms pending_fns={} head_tag={} "
            "head_age_ms={})",
            gate_iteration, now_ms - gate_start_ms, gate_pending_count,
            gate_pending_head_tag.empty() ? "<anonymous>"
                                          : gate_pending_head_tag,
            gate_pending_head_age_ms);
        ExecutePendingFunctions();
        if (!worker_running_) {
          break;
        }
        now_ms = NowMs();
      }

      if (kernel_state_) {
        auto* emulator = kernel_state_->emulator();
        if (emulator && emulator->IsModuleLoadGateClosed()) {
          bool log_due = gate_last_log_ms == 0 || now_ms < gate_last_log_ms ||
                         now_ms - gate_last_log_ms >= gate_log_interval_ms;
          if (log_due) {
            gate_last_log_ms = now_ms;
            XELOGW(
                "RING BUFFER: pre-loop bootstrap gate waiting "
                "(blocked_at=module_load_gate_closed iteration={} elapsed={}ms)",
                gate_iteration, now_ms - gate_start_ms);
          }
          auto* module_load_event = kernel_state_->user_module_load_event();
          if (module_load_event) {
            xe::threading::Wait(module_load_event, false,
                                std::chrono::milliseconds(gate_wait_ms));
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(gate_wait_ms));
          }
          continue;
        }
      }

      // Mirror loop-time ack handling so init_ack preconditions can be
      // satisfied before loop #1.
      if (initialization_complete_event_ &&
          initialization_complete_event_->Query().state) {
        bool was_ack = initialization_acknowledged_.exchange(
            true, std::memory_order_relaxed);
        if (!was_ack) {
          init_ack_transition_count_.fetch_add(1, std::memory_order_relaxed);
          last_init_ack_ms_.store(now_ms, std::memory_order_relaxed);
          XELOGI(
              "RING BUFFER: pre-loop gate acknowledged ring initialization");
        }
        initialization_complete_event_->Reset();
      } else if (ring_buffer_initialized_.load(std::memory_order_relaxed) &&
                 !initialization_acknowledged_.load(
                     std::memory_order_relaxed)) {
        initialization_acknowledged_.store(true, std::memory_order_relaxed);
        init_ack_transition_count_.fetch_add(1, std::memory_order_relaxed);
        last_init_ack_ms_.store(now_ms, std::memory_order_relaxed);
        XELOGI("RING BUFFER: pre-loop gate acknowledged init (state check)");
      }

      bool ring_init = ring_buffer_initialized_.load(std::memory_order_relaxed);
      bool init_ack =
          initialization_acknowledged_.load(std::memory_order_relaxed);
      bool rb_valid = primary_buffer_ptr_ != 0 && primary_buffer_size_ != 0;

      GraphicsSystem::PipelinePrereqSnapshot gs_snapshot;
      bool gs_available = graphics_system_ != nullptr;
      if (graphics_system_) {
        gs_snapshot = graphics_system_->GetPipelinePrereqSnapshot();
      }

      uint32_t system_command_buffer =
          graphics_system_ ? graphics_system_->GetSystemCommandBuffer() : 0;
      bool system_command_buffer_set = system_command_buffer != 0;
      bool executable_ready =
          gs_available && gs_snapshot.executable_module_present &&
          gs_snapshot.executable_module_entry != 0 &&
          !gs_snapshot.module_load_inflight;
      bool guest_main_ready = gs_available && gs_snapshot.guest_main_thread_ready &&
                              gs_snapshot.guest_main_thread_running;
      auto proc_stats = kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
      auto find_runtime_stat = [&](uint16_t ordinal)
          -> const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* {
        for (const auto& stat : proc_stats.ordinal_runtime_stats) {
          if (stat.ordinal == ordinal) {
            return &stat;
          }
        }
        return nullptr;
      };
      auto runtime_stat_total =
          [](const kernel::xboxkrnl::GpuBootstrapOrdinalRuntimeStat* stat)
              -> uint64_t {
        if (!stat) {
          return 0;
        }
        return stat->dynamic_lookup_calls + stat->static_thunk_calls +
               stat->export_entry_calls;
      };
      const auto* stat_01C2 = find_runtime_stat(0x01C2);
      const auto* stat_01C3 = find_runtime_stat(0x01C3);
      const auto* stat_01D5 = find_runtime_stat(0x01D5);
      const auto* stat_025B = find_runtime_stat(0x025B);
      uint64_t dispatch_bridge_activity_total =
          proc_stats.guest_main_dispatch_bridge_calls_total;
      uint64_t callback_runtime_activity_total = runtime_stat_total(stat_01D5);
      uint64_t bootstrap_runtime_activity_total =
          runtime_stat_total(stat_01C2) + runtime_stat_total(stat_01C3) +
          runtime_stat_total(stat_01D5);
      bool bootstrap_runtime_activity_seen =
          bootstrap_runtime_activity_total != 0 ||
          dispatch_bridge_activity_total != 0 ||
          proc_stats.proc_lookup_calls != 0 ||
          proc_stats.proc_lookup_resolved != 0 ||
          proc_stats.proc_lookup_missing != 0;
      bool bootstrap_activity_seen =
          gs_available &&
          (gs_snapshot.vd_initialize_ring_buffer_calls != 0 ||
           gs_snapshot.vd_set_interrupt_callback_calls != 0 ||
           gs_snapshot.vd_swap_calls != 0 ||
           gs_snapshot.mmio_cp_rb_base_guest_writes != 0 ||
           gs_snapshot.mmio_cp_rb_cntl_guest_writes != 0 ||
           gs_snapshot.mmio_cp_rb_wptr_guest_writes != 0 ||
           bootstrap_runtime_activity_seen);
      bool callback_set = gs_available && gs_snapshot.interrupt_callback_set;
      bool callback_guest_registration_seen =
          gs_available &&
          (gs_snapshot.vd_set_interrupt_callback_calls != 0 ||
           callback_runtime_activity_total != 0);
      bool callback_ready = callback_set && callback_guest_registration_seen;

      bool producer_runtime_activity_seen =
          runtime_stat_total(stat_025B) != 0;
      uint64_t swap_packet_count =
          swap_packet_count_.load(std::memory_order_relaxed);
      uint64_t debug_host_swap_count =
          debug_host_swap_injection_count_.load(std::memory_order_relaxed);
      bool guest_swap_packet_seen = swap_packet_count > debug_host_swap_count;
      bool producer_entry_seen =
          !require_producer_entry ||
          (gs_available &&
           (gs_snapshot.vd_swap_calls != 0 || producer_runtime_activity_seen ||
            guest_swap_packet_seen ||
            gs_snapshot.mmio_cp_rb_wptr_guest_writes != 0));
      uint64_t gate_elapsed_ms =
          now_ms >= gate_start_ms ? (now_ms - gate_start_ms) : 0;
      uint64_t startup_escape_after_ms = static_cast<uint64_t>(
          cvars::
              gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      constexpr uint64_t kStartupPrologHelperGraceWindowMs = 125;
      constexpr uint32_t kStartupPrologHelperGraceRepeatSamples = 6;
      bool startup_prolog_helper_grace_pending =
          gs_snapshot.guest_main_dispatch_stall_state ==
              "startup_prolog_helper_only" &&
          gs_snapshot.guest_main_dispatch_stall_age_ms <
              kStartupPrologHelperGraceWindowMs &&
          gs_snapshot.guest_main_dispatch_stall_repeat_samples <=
              kStartupPrologHelperGraceRepeatSamples;
      bool dispatch_stall_escape =
          cvars::gpu_worker_preloop_bootstrap_gate_allow_startup_prolog_escape &&
          gs_available && ring_init && init_ack && rb_valid &&
          system_command_buffer_set && executable_ready && guest_main_ready &&
          !bootstrap_activity_seen &&
          (gs_snapshot.guest_main_dispatch_stall_state ==
               "startup_prolog_helper_only" ||
           gs_snapshot.guest_main_dispatch_stall_state ==
               "title_local_privilege_gate_before_gpu_dispatch") &&
          gs_snapshot.guest_main_dispatch_stall_age_ms >=
              startup_escape_after_ms;
      const char* dispatch_stall_escape_reason =
          gs_snapshot.guest_main_dispatch_stall_state ==
                  "title_local_privilege_gate_before_gpu_dispatch"
              ? "title-local pre-GPU privilege-gate stall"
              : "startup prolog-helper stall";
      bool callback_stall_escape =
          cvars::gpu_worker_preloop_bootstrap_gate_allow_startup_prolog_escape &&
          gs_available && ring_init && init_ack && rb_valid &&
          system_command_buffer_set && executable_ready && guest_main_ready &&
          bootstrap_activity_seen && !callback_ready &&
          (gs_snapshot.guest_main_dispatch_stall_state ==
               "gpu_bootstrap_entry_loop_before_callback" ||
           gs_snapshot.guest_main_dispatch_stall_state ==
               "title_local_privilege_gate_before_gpu_dispatch" ||
           gs_snapshot.guest_main_dispatch_stall_state ==
               "title_local_busy_loop_without_import_dispatch" ||
           gs_snapshot.guest_main_dispatch_stall_state ==
               "title_local_busy_loop_before_gpu_dispatch" ||
           gs_snapshot.guest_main_dispatch_stall_state ==
               "busy_loop_before_gpu_dispatch") &&
          gs_snapshot.guest_main_dispatch_stall_age_ms >=
              startup_escape_after_ms;
      const char* callback_stall_escape_reason =
          gs_snapshot.guest_main_dispatch_stall_state ==
                  "gpu_bootstrap_entry_loop_before_callback"
              ? "stable early GPU bootstrap entry loop before callback registration"
          : gs_snapshot.guest_main_dispatch_stall_state ==
                    "title_local_privilege_gate_before_gpu_dispatch"
                ? "title-local privilege gate before loader/GPU dispatch"
          : gs_snapshot.guest_main_dispatch_stall_state ==
                    "title_local_busy_loop_without_import_dispatch"
                ? "title-local busy loop without loader/import dispatch"
          : gs_snapshot.guest_main_dispatch_stall_state ==
                    "title_local_busy_loop_before_gpu_dispatch"
                ? "title-local busy loop before GPU dispatch"
                : "busy loop before GPU dispatch";
      bool bridge_only_callback_starvation_pending =
          cvars::gpu_worker_preloop_bootstrap_gate_allow_startup_prolog_escape &&
          !startup_prolog_helper_grace_pending &&
          gs_available && ring_init && init_ack && rb_valid &&
          system_command_buffer_set && executable_ready && guest_main_ready &&
          bootstrap_activity_seen && !callback_ready &&
          dispatch_bridge_activity_total != 0 &&
          bootstrap_runtime_activity_total == 0 &&
          !producer_runtime_activity_seen &&
          gs_snapshot.vd_initialize_ring_buffer_calls == 0 &&
          gs_snapshot.vd_set_interrupt_callback_calls == 0 &&
          gs_snapshot.vd_swap_calls == 0 &&
          gs_snapshot.mmio_cp_rb_base_guest_writes == 0 &&
          gs_snapshot.mmio_cp_rb_cntl_guest_writes == 0 &&
          gs_snapshot.mmio_cp_rb_wptr_guest_writes == 0 &&
          proc_stats.loader_module_handle_calls_total == 0 &&
          proc_stats.loader_proc_lookup_calls_total == 0 &&
          proc_stats.proc_lookup_calls == 0 &&
          proc_stats.proc_lookup_resolved == 0 &&
          proc_stats.proc_lookup_missing == 0;
      bool bridge_only_callback_stall_escape =
          bridge_only_callback_starvation_pending &&
          std::max(gs_snapshot.guest_main_dispatch_stall_age_ms,
                   gate_elapsed_ms) >= startup_escape_after_ms;
      const char* bridge_only_callback_stall_escape_reason =
          gs_snapshot.guest_main_dispatch_stall_state ==
                  "startup_prolog_helper_only"
              ? "guest main thread remained in same-module startup prolog "
                "helper dispatch after processor dispatch entry"
              : "guest-dispatch bridge without any loader/callback/MMIO "
                "progress";

      bool gate_ready = gs_available && ring_init && init_ack && rb_valid &&
                        system_command_buffer_set && executable_ready &&
                        guest_main_ready &&
                        ((bootstrap_activity_seen && callback_ready &&
                          producer_entry_seen) ||
                         dispatch_stall_escape || callback_stall_escape ||
                         bridge_only_callback_stall_escape);
      if (gate_ready) {
        if (callback_stall_escape) {
          XELOGW(
              "RING BUFFER: pre-loop bootstrap gate opened after {}ms "
              "(iteration={}) due to persistent {} "
              "(stall_age={}ms repeat_samples={}); continuing worker loop "
              "without forcing callback registration",
              now_ms - gate_start_ms, gate_iteration,
              callback_stall_escape_reason,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples);
        } else if (bridge_only_callback_stall_escape) {
          XELOGW(
              "RING BUFFER: pre-loop bootstrap gate opened after {}ms "
              "(iteration={}) due to persistent {} "
              "(gate_elapsed={}ms stall_age={}ms repeat_samples={} "
              "guest_main_dispatch_bridge_total={} proc_lookup_calls={}); "
              "continuing worker loop without forcing callback registration",
              gate_elapsed_ms, gate_iteration,
              bridge_only_callback_stall_escape_reason, gate_elapsed_ms,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples,
              dispatch_bridge_activity_total, proc_stats.proc_lookup_calls);
        } else if (dispatch_stall_escape) {
          XELOGW(
              "RING BUFFER: pre-loop bootstrap gate opened after {}ms "
              "(iteration={}) due to persistent {} "
              "(stall_age={}ms repeat_samples={}); continuing worker loop "
              "without forcing",
              now_ms - gate_start_ms, gate_iteration,
              dispatch_stall_escape_reason,
              gs_snapshot.guest_main_dispatch_stall_age_ms,
              gs_snapshot.guest_main_dispatch_stall_repeat_samples);
        } else {
          XELOGI(
              "RING BUFFER: pre-loop bootstrap gate satisfied after {}ms "
              "(iteration={} ring_init={} init_ack={} rb_base={:08X} "
              "rb_size={:08X} sys_cmd_buffer={:08X} callback={:08X})",
              now_ms - gate_start_ms, gate_iteration, ring_init ? "YES" : "NO",
              init_ack ? "YES" : "NO", primary_buffer_ptr_,
              primary_buffer_size_, system_command_buffer,
              gs_snapshot.interrupt_callback);
        }
        break;
      }

      const char* blocked_at = "unknown";
      std::string blocked_detail = "gate conditions not satisfied";
      if (!gs_available) {
        blocked_at = "graphics_system_link";
        blocked_detail = "graphics_system_ is null";
      } else if (!ring_init) {
        blocked_at = "ring_init";
        blocked_detail = "ring_buffer_initialized is false";
      } else if (!init_ack) {
        blocked_at = "ring_init_ack";
        blocked_detail = "initialization not acknowledged by worker";
      } else if (!rb_valid) {
        blocked_at = "ring_buffer_bounds";
        blocked_detail = fmt::format("rb_base={:08X} rb_size={:08X}",
                                     primary_buffer_ptr_, primary_buffer_size_);
      } else if (!system_command_buffer_set) {
        blocked_at = "system_command_buffer";
        blocked_detail = "GetSystemCommandBuffer is zero";
      } else if (!gs_snapshot.executable_module_present) {
        blocked_at = "executable_module";
        blocked_detail = "executable module not visible yet";
      } else if (gs_snapshot.executable_module_entry == 0) {
        blocked_at = "executable_entry";
        blocked_detail = "executable module entry is zero";
      } else if (gs_snapshot.module_load_inflight) {
        blocked_at = "module_load_inflight";
        blocked_detail = "module load still inflight";
      } else if (!gs_snapshot.guest_main_thread_ready) {
        blocked_at = "guest_main_thread_ready";
        blocked_detail = "guest main thread not available";
      } else if (!gs_snapshot.guest_main_thread_running) {
        blocked_at = "guest_main_thread_running";
        blocked_detail = "guest main thread not running";
      } else if (!bootstrap_activity_seen &&
                 gs_snapshot.guest_main_dispatch_stall_state ==
                     "startup_prolog_helper_only") {
        blocked_at = "startup_prolog_helper_only";
        blocked_detail = fmt::format(
            "guest main thread is repeatedly sampled in same-module startup "
            "prolog helpers before any GPU bootstrap activity "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!bootstrap_activity_seen &&
                 gs_snapshot.guest_main_dispatch_stall_state ==
                     "title_local_privilege_gate_before_gpu_dispatch") {
        blocked_at = "title_local_privilege_gate_before_gpu_dispatch";
        blocked_detail = fmt::format(
            "guest main thread is repeatedly sampled behind a title-local "
            "pre-GPU privilege gate before any GPU bootstrap activity "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!callback_set && startup_prolog_helper_grace_pending) {
        blocked_at = "startup_prolog_helper_grace";
        blocked_detail = fmt::format(
            "guest main thread has only reached the initial startup prolog "
            "window after processor dispatch entry "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!callback_set &&
                 gs_snapshot.guest_main_dispatch_stall_state ==
                     "startup_prolog_helper_only") {
        blocked_at = "startup_prolog_helper_only";
        blocked_detail = fmt::format(
            "guest main thread remains in same-module startup prolog helper "
            "dispatch after processor dispatch entry "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!callback_set &&
                 gs_snapshot.guest_main_dispatch_stall_state ==
                     "gpu_bootstrap_entry_loop_before_callback") {
        blocked_at = "gpu_bootstrap_entry_loop_before_callback";
        blocked_detail = fmt::format(
            "guest main thread remains in the early GPU bootstrap entry loop "
            "before callback registration "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!callback_set &&
                 gs_snapshot.guest_main_dispatch_stall_state ==
                     "title_local_busy_loop_without_import_dispatch") {
        blocked_at = "title_local_busy_loop_without_import_dispatch";
        blocked_detail = fmt::format(
            "guest main thread remains in a title-local busy loop without "
            "loader/import dispatch before callback registration "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!callback_set &&
                 gs_snapshot.guest_main_dispatch_stall_state ==
                     "title_local_busy_loop_before_gpu_dispatch") {
        blocked_at = "title_local_busy_loop_before_gpu_dispatch";
        blocked_detail = fmt::format(
            "guest main thread remains in a title-local busy loop before "
            "callback registration "
            "(stall_age={}ms repeat_samples={} escape_after={}ms)",
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            cvars::
                gpu_worker_preloop_bootstrap_gate_startup_prolog_escape_after_ms);
      } else if (!callback_set && bridge_only_callback_starvation_pending) {
        blocked_at = "guest_dispatch_bridge_callback_starvation";
        blocked_detail = fmt::format(
            "guest main dispatch bridge executed, but no loader, callback, "
            "MMIO, or tracked runtime GPU progress followed "
            "(gate_elapsed={}ms stall_state={} stall_age={}ms "
            "repeat_samples={} guest_main_dispatch_bridge_total={} "
            "loader_module_handle_calls_total={} "
            "loader_proc_lookup_calls_total={} escape_after={}ms)",
            gate_elapsed_ms,
            gs_snapshot.guest_main_dispatch_stall_state.empty()
                ? "<none>"
                : gs_snapshot.guest_main_dispatch_stall_state,
            gs_snapshot.guest_main_dispatch_stall_age_ms,
            gs_snapshot.guest_main_dispatch_stall_repeat_samples,
            dispatch_bridge_activity_total,
            proc_stats.loader_module_handle_calls_total,
            proc_stats.loader_proc_lookup_calls_total,
            startup_escape_after_ms);
      } else if (!bootstrap_activity_seen) {
        blocked_at = "bootstrap_activity";
        blocked_detail = fmt::format(
            "no Vd/MMIO/bootstrap-runtime activity observed yet "
            "(runtime_bootstrap_total={} "
            "guest_main_dispatch_bridge_total={} proc_lookup_calls={})",
            bootstrap_runtime_activity_total,
            dispatch_bridge_activity_total, proc_stats.proc_lookup_calls);
      } else if (!callback_set) {
        blocked_at = "interrupt_callback";
        blocked_detail = "interrupt callback not set";
      } else if (!callback_guest_registration_seen) {
        blocked_at = "interrupt_callback_untrusted";
        blocked_detail = fmt::format(
            "interrupt callback is set but guest callback registration is "
            "missing (vd_set_cb_calls={} runtime_01D5_total={})",
            gs_snapshot.vd_set_interrupt_callback_calls,
            callback_runtime_activity_total);
      } else if (require_producer_entry && !producer_entry_seen) {
        blocked_at = "producer_entry";
        blocked_detail = fmt::format(
            "no VdSwap/runtime ordinal 0x025B/guest-MMIO-WPTR/guest swap packet "
            "activity (vd_swap_calls={} runtime_025B={} mmio_wptr={} "
            "swap_packets={} debug_host_swaps={})",
            gs_snapshot.vd_swap_calls,
            producer_runtime_activity_seen ? "YES" : "NO",
            gs_snapshot.mmio_cp_rb_wptr_guest_writes, swap_packet_count,
            debug_host_swap_count);
      }

      bool log_due = gate_last_log_ms == 0 || now_ms < gate_last_log_ms ||
                     now_ms - gate_last_log_ms >= gate_log_interval_ms;
      if (log_due) {
        gate_last_log_ms = now_ms;
        XELOGW(
            "RING BUFFER: pre-loop bootstrap gate waiting "
            "(iteration={} elapsed={}ms blocked_at={} detail='{}' "
            "ring_init={} init_ack={} rb_valid={} sys_cmd_buffer_set={} "
            "module_inflight={} callback_set={} "
            "callback_guest_registered={} bootstrap_seen={})",
            gate_iteration, now_ms - gate_start_ms, blocked_at, blocked_detail,
            ring_init ? "YES" : "NO", init_ack ? "YES" : "NO",
            rb_valid ? "YES" : "NO",
            system_command_buffer_set ? "YES" : "NO",
            gs_snapshot.module_load_inflight ? "YES" : "NO",
            callback_set ? "YES" : "NO",
            callback_guest_registration_seen ? "YES" : "NO",
            bootstrap_activity_seen ? "YES" : "NO");
      }

      if (gate_timeout_ms != 0 &&
          now_ms - gate_start_ms >= gate_timeout_ms) {
        XELOGW(
            "RING BUFFER: pre-loop bootstrap gate timeout after {}ms "
            "(blocked_at={} detail='{}') - continuing without forcing",
            now_ms - gate_start_ms, blocked_at, blocked_detail);
        break;
      }

      std::vector<xe::threading::WaitHandle*> gate_wait_handles;
      if (write_ptr_index_event_) {
        gate_wait_handles.push_back(write_ptr_index_event_.get());
      }
      if (initialization_complete_event_) {
        gate_wait_handles.push_back(initialization_complete_event_.get());
      }
      if (kernel_state_) {
        auto* module_load_event = kernel_state_->user_module_load_event();
        if (module_load_event) {
          gate_wait_handles.push_back(module_load_event);
        }
      }
      if (!gate_wait_handles.empty()) {
        xe::threading::WaitAny(gate_wait_handles, false,
                               std::chrono::milliseconds(gate_wait_ms));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(gate_wait_ms));
      }
    }

    if (!worker_running_) {
      XELOGI(
          "RING BUFFER: worker stopping before loop #1 due to shutdown during "
          "pre-loop bootstrap gate");
      return;
    }
  }

  uint64_t last_write_ptr_change_ms = 0;
  uint64_t last_idle_log_ms = 0;
  uint32_t last_write_ptr = 0;
  bool logged_first_activity = false;
  uint64_t start_ms = NowMs();
  uint64_t last_no_activity_log_ms = 0;
  uint64_t last_idle_snapshot_ms = 0;
  uint64_t last_rb_dump_ms = 0;
  uint64_t last_rb_scan_ms = 0;
  uint64_t last_rb_full_scan_ms = 0;
  uint64_t last_rb_hash = 0;
  bool rb_hash_valid = false;
  std::vector<uint8_t> rb_scan_snapshot;
  size_t rb_scan_snapshot_bytes = 0;
  bool idle_abort_triggered = false;
  uint64_t ring_buffer_init_ms = 0;
  bool rb_clear_reset_done = false;
  uint64_t loop_count = 0;
  uint64_t wait_count = 0;
  uint64_t consecutive_zero_rw_timeouts = 0;
  uint64_t last_zero_rw_recovery_ms = 0;
  bool pending_watch_rearm_after_processing = false;
  uint64_t rw_equal_since_ms = 0;
  uint64_t last_watch_stuck_log_ms = 0;

  while (worker_running_) {
    if (kernel_state_) {
      auto* emulator = kernel_state_->emulator();
      if (emulator && emulator->IsModuleLoadGateClosed()) {
        auto* module_load_event = kernel_state_->user_module_load_event();
        uint64_t wait_ms = static_cast<uint64_t>(
            std::max<uint32_t>(1,
                               cvars::gpu_worker_suspend_module_gate_wait_ms));
        if (module_load_event) {
          xe::threading::Wait(module_load_event, false,
                              std::chrono::milliseconds(wait_ms));
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }
        continue;
      }
    }

    if (cvars::gpu_worker_suspend_until_module_load_success && kernel_state_) {
      auto load_stats = kernel_state_->GetUserModuleLoadStats();
      auto executable_module = kernel_state_->GetExecutableModule();
      bool executable_fully_ready = executable_module != nullptr &&
                                    executable_module->entry_point() != 0 &&
                                    !load_stats.load_inflight;
      if (!executable_fully_ready) {
        uint64_t now_ms = NowMs();

        auto* module_load_event = kernel_state_->user_module_load_event();
        uint64_t wait_ms = static_cast<uint64_t>(
            std::max<uint32_t>(1,
                               cvars::gpu_worker_suspend_module_gate_wait_ms));
        if (module_load_event) {
          xe::threading::Wait(module_load_event, false,
                              std::chrono::milliseconds(wait_ms));
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }
        continue;
      }
    }

    uint64_t loop_id = ++loop_count;
    bool log_loop = cvars::gpu_debug_worker_verbose &&
                    ShouldLogEvery(loop_id, cvars::gpu_debug_worker_every_n);
    size_t pending_count = 0;
    if (log_loop || cvars::gpu_debug_worker_trace_pending) {
      std::lock_guard<std::mutex> pending_lock(pending_fns_mutex_);
      pending_count = pending_fns_.size();
    }
    if (log_loop) {
      uint32_t ring_dwords =
          primary_buffer_size_ ? (primary_buffer_size_ / sizeof(uint32_t)) : 0;
      bool worker_ready =
          worker_setup_complete_.load(std::memory_order_relaxed);
      bool ring_ready = ring_buffer_initialized_.load(std::memory_order_relaxed);
      XELOGI(
          "DEBUG: GPU WORKER LOOP #{}: read={:08X} write={:08X} "
          "worker_ready={} ring_init={} rb_base={:08X} rb_size={:08X} "
          "ring_dwords={} pending_fns={}",
          loop_id, read_ptr_index_, write_ptr_index_.load(),
          worker_ready ? "YES" : "NO", ring_ready ? "YES" : "NO",
          primary_buffer_ptr_, primary_buffer_size_, ring_dwords, pending_count);
      LogWorkerPipelineChecklist(loop_id, pending_count);
    }

    if (cvars::gpu_debug_watch_ringbuffer_writes &&
        ring_buffer_initialized_.load()) {
      if (ring_watch_restore_verify_pending_.exchange(false,
                              std::memory_order_relaxed)) {
      uint32_t verify_phys =
        ring_watch_restore_verify_phys_.load(std::memory_order_relaxed);
      uint32_t verify_len =
        ring_watch_restore_verify_len_.load(std::memory_order_relaxed);
      XELOGI(
        "RING BUFFER: watch-hit restore-rw verify starting "
        "(hit_phys={:08X} hit_len={:X})",
        verify_phys, verify_len);
      LogRingBufferProtectionState("watch-hit-post-unprotect", verify_phys,
                     verify_len);
      }

      bool rearm_requested = ring_buffer_watch_rearm_requested_.exchange(
          false, std::memory_order_relaxed);
      if (rearm_requested) {
        pending_watch_rearm_after_processing = true;
        XELOGI(
            "RING BUFFER: watch-hit rearm deferred until packet processing "
            "(read={:08X} write={:08X})",
            read_ptr_index_, write_ptr_index_.load(std::memory_order_relaxed));
      }
    }

    // Run pending functions before waiting so queued fallback/bootstrap work
    // can't stall behind an idle wait cycle when no write pointer changes are
    // happening yet.
    {
      std::lock_guard<std::mutex> pending_lock(pending_fns_mutex_);
      pending_count = pending_fns_.size();
      if (pending_count != 0) {
        const auto& pending = pending_fns_.front();
        uint64_t pending_now_ms = NowMs();
        uint64_t pending_age_ms =
            (pending.enqueue_ms && pending_now_ms >= pending.enqueue_ms)
                ? (pending_now_ms - pending.enqueue_ms)
                : 0;
        if (cvars::gpu_debug_worker_trace_pending || log_loop) {
          XELOGI(
              "DEBUG: GPU WORKER: pending function head tag={} age_ms={}",
              pending.tag.empty() ? "<anonymous>" : pending.tag, pending_age_ms);
        }
      }
    }
    if (pending_count != 0) {
      if (cvars::gpu_debug_worker_trace_pending || log_loop) {
        XELOGI(
            "DEBUG: GPU WORKER: executing pending functions before wait "
            "(pending_fns={})",
            pending_count);
      }
      ExecutePendingFunctions();
      if (!worker_running_) {
        XELOGI("DEBUG: WORKER: Shutting down...");
        break;
      }
      continue;
    }

    // CRITICAL FIX 4: Enhanced wait mechanism with multiple event sources
    // Wait for any signal: work event OR initialization complete event
    std::vector<xe::threading::WaitHandle*> wait_handles;
    wait_handles.push_back(write_ptr_index_event_.get());
    size_t write_event_index = 0;
    size_t init_event_index = SIZE_MAX;
    size_t module_load_event_index = SIZE_MAX;
    // Once ring initialization flips to true, stop waiting on module-load
    // heartbeat events by default to avoid wake-thrash; however, keep listening
    // while a module load is still inflight with no executable module yet.
    bool ring_ready_for_module_event =
        ring_buffer_initialized_.load(std::memory_order_relaxed);
    if (initialization_complete_event_) {
      init_event_index = wait_handles.size();
      wait_handles.push_back(initialization_complete_event_.get());
    }
    bool include_module_load_event = false;
    if (cvars::gpu_waitany_include_module_load_event && kernel_state_) {
      bool module_load_inflight_without_executable = false;
      if (ring_ready_for_module_event &&
          !cvars::gpu_waitany_include_module_load_event_after_ring_ready) {
        auto load_stats = kernel_state_->GetUserModuleLoadStats();
        module_load_inflight_without_executable =
            load_stats.load_inflight &&
            load_stats.loaded_executable_user_modules == 0;
      }
      include_module_load_event =
          !ring_ready_for_module_event ||
          cvars::gpu_waitany_include_module_load_event_after_ring_ready ||
          module_load_inflight_without_executable;
    }
    if (include_module_load_event) {
      auto* module_load_event = kernel_state_->user_module_load_event();
      if (module_load_event) {
        module_load_event_index = wait_handles.size();
        wait_handles.push_back(module_load_event);
      }
    }

    // Wait for any event to signal
    uint64_t wait_timeout_ms =
      std::max<uint64_t>(1, cvars::gpu_waitany_timeout_ms);
    if (graphics_system_) {
      auto wait_snapshot = graphics_system_->GetPipelinePrereqSnapshot();
      auto wait_proc_stats =
          kernel::xboxkrnl::GetGpuBootstrapImportLookupStats();
      bool privilege_gate_idle_wait =
          pending_count == 0 &&
          read_ptr_index_ == write_ptr_index_.load(std::memory_order_relaxed) &&
          wait_snapshot.guest_main_dispatch_stall_state ==
              "title_local_privilege_gate_before_gpu_dispatch" &&
          !wait_snapshot.interrupt_callback_set &&
          wait_snapshot.vd_initialize_ring_buffer_calls == 0 &&
          wait_snapshot.vd_set_interrupt_callback_calls == 0 &&
          wait_snapshot.vd_swap_calls == 0 &&
          wait_snapshot.mmio_cp_rb_base_guest_writes == 0 &&
          wait_snapshot.mmio_cp_rb_cntl_guest_writes == 0 &&
          wait_snapshot.mmio_cp_rb_wptr_guest_writes == 0 &&
          wait_proc_stats.loader_module_handle_calls_total == 0 &&
          wait_proc_stats.loader_proc_lookup_calls_total == 0 &&
          wait_proc_stats.xex_check_executable_privilege_calls == 0 &&
          wait_proc_stats.xex_check_executable_privilege_static_thunk_calls ==
              0 &&
          wait_proc_stats.xex_check_executable_privilege_export_entry_calls ==
              0;
      if (privilege_gate_idle_wait) {
        wait_timeout_ms = std::max<uint64_t>(
            wait_timeout_ms, cvars::gpu_waitany_privilege_gate_timeout_ms);
      }
    }
    auto wait_result = xe::threading::WaitAny(
      wait_handles, false, std::chrono::milliseconds(wait_timeout_ms));
    bool wait_timed_out =
        wait_result.first == xe::threading::WaitResult::kTimeout;
    uint64_t wait_id = ++wait_count;
    if (cvars::gpu_debug_wait_results &&
        ShouldLogEvery(wait_id, cvars::gpu_debug_wait_every_n)) {
      bool worker_ready =
          worker_setup_complete_.load(std::memory_order_relaxed);
      bool ring_ready = ring_buffer_initialized_.load(std::memory_order_relaxed);
      XELOGI(
          "DEBUG: GPU WORKER WAIT #{} result={} index={} timed_out={} "
          "handles={} timeout_ms={} read={:08X} write={:08X} worker_ready={} "
          "ring_init={} pending_fns={}",
          wait_id, WaitResultToString(wait_result.first),
          static_cast<uint32_t>(wait_result.second),
          wait_timed_out ? "YES" : "NO",
          static_cast<uint32_t>(wait_handles.size()), wait_timeout_ms,
          read_ptr_index_,
          write_ptr_index_.load(), worker_ready ? "YES" : "NO",
          ring_ready ? "YES" : "NO", pending_count);
    }

    // Process any pending functions first thing after waking up
    ExecutePendingFunctions();

    if (!worker_running_) {
      XELOGI("DEBUG: WORKER: Shutting down...");
      break;
    }

    if (wait_result.first == xe::threading::WaitResult::kSuccess &&
        wait_result.second == write_event_index) {
      // Log only occasionally or on first hit to avoid flooding
      static uint64_t wake_count = 0;
      if (wake_count++ % 100 == 0) {
        XELOGI(
            "DEBUG: WORKER: Woke up due to write_ptr_index_event_ (total wakes: "
            "{})",
            wake_count);
      }
    } else if (wait_result.first == xe::threading::WaitResult::kSuccess &&
               module_load_event_index != SIZE_MAX &&
               wait_result.second == module_load_event_index) {
      if (cvars::gpu_debug_wait_results &&
          ShouldLogEvery(wait_id, cvars::gpu_debug_wait_every_n)) {
        XELOGI("DEBUG: WORKER: Woke up due to user-module load progress event");
      }
    }
    if (wait_timed_out) {
      static uint64_t timeout_count = 0;
      if (timeout_count++ % 50 == 0) {
        XELOGI("DEBUG: WORKER: WaitAny timed out (no event) - polling state");
      }
      if (!ring_buffer_initialized_.load(std::memory_order_relaxed) &&
          (wait_id % 120) == 0) {
        XELOGW(
            "RING BUFFER: worker still waiting for ring initialization "
            "(wait_id={}, init_request_count={}, ack_transitions={}, "
            "last_init_request_ms={}, last_init_signal_ms={}, "
            "last_init_ack_ms={}, has_init_event={}, has_write_event={})",
            wait_id, init_request_count_.load(std::memory_order_relaxed),
            init_ack_transition_count_.load(std::memory_order_relaxed),
            last_init_request_ms_.load(std::memory_order_relaxed),
            last_init_signal_ms_.load(std::memory_order_relaxed),
            last_init_ack_ms_.load(std::memory_order_relaxed),
            initialization_complete_event_ ? "YES" : "NO",
            write_ptr_index_event_ ? "YES" : "NO");
      }

      uint32_t write_ptr_now = write_ptr_index_.load(std::memory_order_relaxed);
      bool zero_rw_idle = ring_buffer_initialized_.load(std::memory_order_relaxed) &&
                          initialization_acknowledged_.load(std::memory_order_relaxed) &&
                          read_ptr_index_ == 0 && write_ptr_now == 0;
      if (zero_rw_idle) {
        ++consecutive_zero_rw_timeouts;
        uint32_t recover_after = cvars::gpu_waitany_zero_rw_recover_after_timeouts;
        if (recover_after != 0 &&
            consecutive_zero_rw_timeouts >=
                static_cast<uint64_t>(recover_after)) {
          uint64_t now_ms = NowMs();
          if (last_zero_rw_recovery_ms == 0 ||
              now_ms - last_zero_rw_recovery_ms >= 1000) {
            last_zero_rw_recovery_ms = now_ms;
            XELOGW(
                "RING BUFFER: WaitAny zero-RW timeout recovery nudge "
                "(timeouts={}, read={:08X}, write={:08X}, init_ack={})",
                consecutive_zero_rw_timeouts, read_ptr_index_, write_ptr_now,
                initialization_acknowledged_.load(std::memory_order_relaxed)
                    ? "YES"
                    : "NO");
            if (write_ptr_index_event_) {
              write_ptr_index_event_->Set();
            }
            if (initialization_complete_event_) {
              initialization_complete_event_->Set();
            }
          }
          consecutive_zero_rw_timeouts = 0;
        }
      } else {
        consecutive_zero_rw_timeouts = 0;
      }
    }

    // CRITICAL FIX 5: Acknowledge initialization if needed
    // The event is manual-reset and may remain signaled if ack was set from
    // UpdateWritePointer before the worker observed the event. Always clear
    // stale signals to avoid hot wait loops on index=1.
    if (initialization_complete_event_ &&
        initialization_complete_event_->Query().state) {
      bool was_ack = initialization_acknowledged_.exchange(
          true, std::memory_order_relaxed);
      if (!was_ack) {
        init_ack_transition_count_.fetch_add(1, std::memory_order_relaxed);
        last_init_ack_ms_.store(NowMs(), std::memory_order_relaxed);
        XELOGI("DEBUG: WORKER: Acknowledged ring buffer initialization");
      } else if (wait_result.first == xe::threading::WaitResult::kSuccess &&
                 init_event_index != SIZE_MAX &&
                 wait_result.second == init_event_index &&
                 cvars::gpu_debug_wait_results &&
                 ShouldLogEvery(wait_id, cvars::gpu_debug_wait_every_n)) {
        XELOGI(
            "DEBUG: WORKER: clearing stale init-complete event "
            "(already acknowledged)");
      }
      initialization_complete_event_->Reset();
    } else if (ring_buffer_initialized_.load() &&
               !initialization_acknowledged_.load()) {
      // Fallback path: if the worker observes an initialized ring buffer but
      // missed the completion event edge, still acknowledge initialization so
      // debug state reflects real readiness.
      initialization_acknowledged_.store(true, std::memory_order_relaxed);
      init_ack_transition_count_.fetch_add(1, std::memory_order_relaxed);
      last_init_ack_ms_.store(NowMs(), std::memory_order_relaxed);
      XELOGI(
          "DEBUG: WORKER: Acknowledged ring buffer initialization (state check)");
    }

    // Track when the ring buffer becomes initialized so we can measure
    // post-init timeouts accurately.
    if (ring_buffer_initialized_.load() && ring_buffer_init_ms == 0) {
      ring_buffer_init_ms = NowMs();
      if (cvars::gpu_debug_worker_verbose) {
        uint32_t ring_dwords = primary_buffer_size_
                                   ? (primary_buffer_size_ / sizeof(uint32_t))
                                   : 0;
        DumpRingBufferSummary("ring buffer init", reader_.buffer(), ring_dwords,
                              read_ptr_index_, write_ptr_index_.load(),
                              primary_buffer_ptr_, primary_buffer_size_,
                              read_ptr_writeback_ptr_);
      }
    }

    // Fast-out checks with enhanced logic
    uint32_t write_ptr_index = write_ptr_index_.load();
    uint64_t now_ms_for_watch = NowMs();
    if (read_ptr_index_ == write_ptr_index) {
      if (rw_equal_since_ms == 0) {
        rw_equal_since_ms = now_ms_for_watch;
      }
    } else {
      rw_equal_since_ms = 0;
    }

    uint64_t watch_force_rw_idle_ms =
      std::max<uint64_t>(1,
                 cvars::gpu_debug_watch_ringbuffer_force_rw_after_idle_ms);
    if (cvars::gpu_debug_watch_ringbuffer_writes && rw_equal_since_ms != 0 &&
      now_ms_for_watch - rw_equal_since_ms >= watch_force_rw_idle_ms) {
      bool guest_write_enabled = ring_watch_last_guest_write_enabled_.load(
          std::memory_order_relaxed);
      if (!guest_write_enabled &&
          (last_watch_stuck_log_ms == 0 ||
           now_ms_for_watch - last_watch_stuck_log_ms >= 1000)) {
        uint64_t transition_ms =
            ring_watch_last_transition_ms_.load(std::memory_order_relaxed);
        uint64_t transition_age =
            (transition_ms && now_ms_for_watch >= transition_ms)
                ? (now_ms_for_watch - transition_ms)
                : 0;
        uint32_t phase_id =
            ring_watch_last_phase_id_.load(std::memory_order_relaxed);
        uint32_t range_start =
            ring_watch_last_range_start_.load(std::memory_order_relaxed);
        uint32_t range_size =
            ring_watch_last_range_size_.load(std::memory_order_relaxed);
        uint32_t range_end =
            range_start + (range_size ? range_size : primary_buffer_size_);
        XELOGW(
            "WATCH STUCK: RB left protected idle_ms={} read={:08X} "
            "write={:08X} phase={}({}) transition_age_ms={} "
            "range=[{:08X}..{:08X}) size={:08X} arms={} hits={} "
            "force_rw_mode={}",
            now_ms_for_watch - rw_equal_since_ms, read_ptr_index_,
            write_ptr_index, phase_id, RingWatchPhaseNameFromId(phase_id),
            transition_age, range_start, range_end, range_size,
            ring_buffer_watch_arm_count_.load(std::memory_order_relaxed),
            ring_buffer_watch_hits_.load(std::memory_order_relaxed),
            ring_watch_force_rw_mode_.load(std::memory_order_relaxed) ? "YES"
                                                                       : "NO");
        LogRingBufferProtectionState("watch-stuck-check", primary_buffer_ptr_,
                                     primary_buffer_size_);
        if (cvars::gpu_debug_watch_ringbuffer_force_rw_fallback &&
            !ring_watch_force_rw_mode_.load(std::memory_order_relaxed) &&
            kernel_state_) {
          auto* memory = kernel_state_->memory();
          auto* heap = memory ? memory->LookupHeap(primary_buffer_ptr_) : nullptr;
          if (heap) {
            uint32_t fallback_protect = kMemoryProtectRead | kMemoryProtectWrite |
                                        kMemoryProtectWriteCombine;
            bool protect_ok = heap->Protect(primary_buffer_ptr_,
                                            primary_buffer_size_,
                                            fallback_protect, nullptr);
            ring_watch_force_rw_mode_.store(true, std::memory_order_relaxed);
            XELOGW(
                "WATCH STUCK: forcing RB RW fallback protect_ok={} "
                "rb=[{:08X}..{:08X}) size={:08X}",
                protect_ok ? "YES" : "NO", primary_buffer_ptr_,
                primary_buffer_ptr_ + primary_buffer_size_, primary_buffer_size_);
            LogRingBufferProtectionState("watch-stuck-force-rw", primary_buffer_ptr_,
                                         primary_buffer_size_);
          }
        }
        last_watch_stuck_log_ms = now_ms_for_watch;
      }
    }

    if (write_ptr_index != last_write_ptr) {
      uint64_t now_ms = NowMs();
      XELOGI("DEBUG: GPU activity: write_ptr advanced {:08X} -> {:08X}",
             last_write_ptr, write_ptr_index);
      last_write_ptr = write_ptr_index;
      last_write_ptr_change_ms = now_ms;
      rb_clear_reset_done = false;
      logged_first_activity = true;
    } else if (logged_first_activity) {
      uint64_t now_ms = NowMs();
      if (now_ms - last_idle_log_ms > 5000) {
      uint64_t idle_ms = now_ms - last_write_ptr_change_ms;
        XELOGI(
            "DEBUG: GPU idle: no write_ptr change for {} ms (read={:08X}, "
            "write={:08X})",
        static_cast<unsigned long long>(idle_ms),
            read_ptr_index_, write_ptr_index);
        uint64_t host_seeded_swaps =
            debug_host_swap_ring_injection_count_.load(std::memory_order_relaxed);
        uint64_t swap_packets =
            swap_packet_count_.load(std::memory_order_relaxed);
        uint64_t rb_watch_hits =
            ring_buffer_watch_hits_.load(std::memory_order_relaxed);
        uint64_t rb_watch_arms =
            ring_buffer_watch_arm_count_.load(std::memory_order_relaxed);
        uint64_t rb_watch_last_arm_ms =
            ring_buffer_watch_last_arm_ms_.load(std::memory_order_relaxed);
        uint64_t rb_watch_last_arm_age_ms =
            (rb_watch_last_arm_ms && now_ms >= rb_watch_last_arm_ms)
                ? (now_ms - rb_watch_last_arm_ms)
                : 0;
        bool rb_watch_rearm_pending =
            ring_buffer_watch_rearm_requested_.load(std::memory_order_relaxed);
        if (swap_packets > 0 && host_seeded_swaps > 0 && rb_watch_hits == 0) {
          XELOGW(
              "RING BUFFER: post-swap idle classifier "
              "swap_packets={} host_debug_ring_swaps={} ring_watch_hits={} "
              "ring_watch_arms={} last_watch_arm_age_ms={} "
              "watch_rearm_pending={} exact_unwatch={} rearm_on_hit={} "
              "-> no observed guest ring writes after host-seeded swap",
              swap_packets, host_seeded_swaps, rb_watch_hits, rb_watch_arms,
              rb_watch_last_arm_age_ms,
              rb_watch_rearm_pending ? "YES" : "NO",
              cvars::gpu_debug_watch_ringbuffer_exact_unwatch ? "YES" : "NO",
              cvars::gpu_debug_watch_ringbuffer_rearm_on_hit ? "YES" : "NO");
        }

        if (graphics_system_ && idle_ms >= 5000 &&
            read_ptr_index_ == write_ptr_index && swap_packets == 0 &&
            graphics_system_->GetInterruptCallback() != 0) {
          bool no_force_profile =
              graphics_system_->GetPipelinePrereqSnapshot().no_force_profile;
          bool allow_loop_forcing =
              cvars::gpu_worker_allow_loop_forcing && !no_force_profile;
          if (allow_loop_forcing) {
            static std::atomic<uint64_t> producer_idle_nudge_last_ms{0};
            uint64_t last_nudge_ms =
                producer_idle_nudge_last_ms.load(std::memory_order_relaxed);
            if (last_nudge_ms == 0 || now_ms < last_nudge_ms ||
                now_ms - last_nudge_ms >= 5000) {
              producer_idle_nudge_last_ms.store(now_ms,
                                                std::memory_order_relaxed);
              uint32_t nudge_width =
                  last_swap_width_.load(std::memory_order_relaxed);
              uint32_t nudge_height =
                  last_swap_height_.load(std::memory_order_relaxed);
              if (nudge_width == 0 || nudge_height == 0) {
                nudge_width = 1280;
                nudge_height = 720;
              }
              XELOGW(
                  "DEBUG: GPU idle producer nudge: idle_ms={} read==write=={:08X} "
                  "swap_packets=0 callback_set=YES -> DebugIssueSwapFromHost "
                  "{}x{}",
                  idle_ms, read_ptr_index_, nudge_width, nudge_height);
              DebugIssueSwapFromHost(nudge_width, nudge_height);
            }
          } else {
            static std::atomic<uint64_t> producer_idle_suppressed_last_ms{0};
            uint64_t last_log_ms =
                producer_idle_suppressed_last_ms.load(std::memory_order_relaxed);
            if (last_log_ms == 0 || now_ms < last_log_ms ||
                now_ms - last_log_ms >= 5000) {
              producer_idle_suppressed_last_ms.store(
                  now_ms, std::memory_order_relaxed);
              XELOGW(
                  "DEBUG: GPU idle producer nudge suppressed by "
                  "loop forcing policy (idle_ms={} read={:08X} "
                  "write={:08X} swap_packets=0 no_force_profile={} "
                  "allow_loop_forcing={})",
                  idle_ms, read_ptr_index_, write_ptr_index,
                  no_force_profile ? "YES" : "NO",
                  cvars::gpu_worker_allow_loop_forcing ? "YES" : "NO");
            }
          }
        }

        last_idle_log_ms = now_ms;
      }
    } else if (ring_buffer_initialized_.load()) {
      uint64_t now_ms = NowMs();
      if (now_ms - start_ms > 5000 && now_ms - last_no_activity_log_ms > 5000) {
        XELOGW(
            "DEBUG: GPU idle: no write_ptr updates since init (read={:08X}, "
            "write={:08X})",
            read_ptr_index_, write_ptr_index);

        // Requested stagnation logging
        if (write_ptr_index == 8 && read_ptr_index_ == 8) {
          XELOGW("DEBUG: RING BUFFER STAGNATION DETECTED:");
          XELOGW(
              "   The GPU command ring buffer was initialized with 8 NOP "
              "dwords at address {:08X}",
              primary_buffer_ptr_);
          XELOGW(
              "   Write pointer (write_ptr) is stuck at 00000008 (never "
              "advances)");
          XELOGW(
              "   Read pointer (read_ptr) is stuck at 00000008 (never "
              "advances)");
          XELOGW(
              "   The GPU worker thread has done {} wait cycles with no new "
              "commands",
              wait_id);
          XELOGW(
              "   The frame limiter thread reports \"NO SWAP PACKETS\" after "
              "{} ms since init",
              static_cast<unsigned long long>(now_ms - ring_buffer_init_ms));
        }

        if (write_ptr_index == 8 && read_ptr_index_ == 8) {
          XELOGW("DEBUG: RING BUFFER STAGNATION DETECTED:");
          XELOGW(
              "   Ring buffer was initialized with 8 NOP dwords at address "
              "{:08X}",
              primary_buffer_ptr_);
          XELOGW(
              "   Write pointer (write_ptr) is stuck at 00000008 (never "
              "advances)");
          XELOGW(
              "   Read pointer (read_ptr) is stuck at 00000008 (never "
              "advances)");
          XELOGW(
              "   The frame limiter thread reports \"NO SWAP PACKETS\" after "
              "15+ seconds");
        }

        last_no_activity_log_ms = now_ms;
      }
      // Check if guest has configured GPU registers before aborting.
      // If rb_base_reg is 0, the guest hasn't set up the ring buffer yet -
      // this is normal during early game initialization and should not abort.
      RegisterFile* guest_rf_check =
          graphics_system_ ? graphics_system_->register_file() : nullptr;
      uint32_t rb_base_reg =
          guest_rf_check ? guest_rf_check->values[0x01C0] : 0;
      uint32_t rb_cntl_reg =
          guest_rf_check ? guest_rf_check->values[0x01C1] : 0;
      bool guest_gpu_configured = (rb_base_reg != 0) || (rb_cntl_reg != 0);

      // Throttle "guest not configured" warning to once per 30s to avoid spam.
      static uint64_t last_guest_not_configured_log_ms = 0;
      constexpr uint64_t kGuestNotConfiguredLogIntervalMs = 30000;
      bool should_log_guest_not_configured =
          (now_ms - last_guest_not_configured_log_ms >=
           kGuestNotConfiguredLogIntervalMs);

      if (!idle_abort_triggered && cvars::gpu_zero_rwptr_abort_ms > 0 &&
          ring_buffer_init_ms != 0 &&
          now_ms - ring_buffer_init_ms >= cvars::gpu_zero_rwptr_abort_ms &&
          write_ptr_index == 0 && read_ptr_index_ == 0) {
        // Only abort if guest has configured GPU registers - otherwise it's
        // just still in initialization and hasn't started rendering yet
        if (!guest_gpu_configured) {
          if (should_log_guest_not_configured) {
            last_guest_not_configured_log_ms = now_ms;
            XELOGW(
                "DEBUG: GPU zero R/W ptr: Guest has not configured GPU registers "
                "yet (rb_base_reg={:08X}, rb_cntl_reg={:08X}). Skipping abort "
                "- "
                "guest may still be initializing. (Log throttled to every {}s)",
                rb_base_reg, rb_cntl_reg,
                static_cast<unsigned>(kGuestNotConfiguredLogIntervalMs / 1000));
          }
          // Don't abort - continue waiting for guest to configure GPU
        } else {
          idle_abort_triggered = true;
          XELOGE(
              "DEBUG: GPU zero R/W ptr abort: read_ptr_index_ and write_ptr_index_ "
              "are still zero {} ms after ring buffer init.",
              static_cast<unsigned long long>(now_ms - ring_buffer_init_ms));
          // Log guest-visible CP_RB_* (GraphicsSystem register file); if all
          // zero, the guest has not written ring buffer registers yet.
          XELOGE(
              "DEBUG: GPU zero R/W ptr abort: rb_base_reg={:08X} "
              "rb_cntl_reg={:08X} "
              "rb_rptr_reg={:08X} rb_wptr_reg={:08X} (guest CP_RB_* writes)",
              guest_rf_check ? guest_rf_check->values[0x01C0] : 0,
              guest_rf_check ? guest_rf_check->values[0x01C1] : 0,
              guest_rf_check ? guest_rf_check->values[0x01C4] : 0,
              guest_rf_check ? guest_rf_check->values[0x01C5] : 0);
          if (cvars::gpu_zero_rwptr_abort_terminate_title && kernel_state_) {
            XELOGE(
                "DEBUG: GPU zero R/W ptr abort: terminating title via "
                "KernelState.");
            kernel_state_->TerminateTitle();
          }
          worker_running_.store(false);
          if (write_ptr_index_event_) {
            write_ptr_index_event_->Set();
          }
          break;
        }
      }
      if (!idle_abort_triggered && cvars::gpu_idle_abort_ms > 0 &&
          now_ms - start_ms >= cvars::gpu_idle_abort_ms) {
        // Only abort if guest has configured GPU registers - otherwise it's
        // just still in initialization
        if (!guest_gpu_configured) {
          if (should_log_guest_not_configured) {
            last_guest_not_configured_log_ms = now_ms;
            XELOGW(
                "DEBUG: GPU idle abort: Guest has not configured GPU registers "
                "yet (rb_base_reg={:08X}, rb_cntl_reg={:08X}). Skipping abort "
                "- "
                "guest may still be initializing. (Log throttled to every {}s)",
                rb_base_reg, rb_cntl_reg,
                static_cast<unsigned>(kGuestNotConfiguredLogIntervalMs / 1000));
          }
        } else {
          idle_abort_triggered = true;
          XELOGE(
              "DEBUG: GPU idle abort: no write_ptr updates for {} ms after init "
              "(read={:08X}, write={:08X}).",
              static_cast<unsigned long long>(now_ms - start_ms),
              read_ptr_index_, write_ptr_index);
          RegisterFile* guest_rf_idle =
              graphics_system_ ? graphics_system_->register_file() : nullptr;
          XELOGE(
              "DEBUG: GPU idle abort: rb_base_reg={:08X} rb_cntl_reg={:08X} "
              "rb_rptr_reg={:08X} rb_wptr_reg={:08X} (guest CP_RB_* writes)",
              guest_rf_idle ? guest_rf_idle->values[0x01C0] : 0,
              guest_rf_idle ? guest_rf_idle->values[0x01C1] : 0,
              guest_rf_idle ? guest_rf_idle->values[0x01C4] : 0,
              guest_rf_idle ? guest_rf_idle->values[0x01C5] : 0);
          if (cvars::gpu_idle_abort_terminate_title && kernel_state_) {
            XELOGE("DEBUG: GPU idle abort: terminating title via KernelState.");
            kernel_state_->TerminateTitle();
          }
          worker_running_.store(false);
          if (write_ptr_index_event_) {
            write_ptr_index_event_->Set();
          }
          break;
        }
      }
      uint64_t snapshot_interval = cvars::gpu_debug_rb_snapshot_interval_ms;
      if (snapshot_interval == 0) {
        snapshot_interval = 5000;
      }
      if (now_ms - last_idle_snapshot_ms > snapshot_interval) {
        RegisterFile* guest_rf_snap =
            graphics_system_ ? graphics_system_->register_file() : nullptr;
        uint32_t rb_base_reg =
            guest_rf_snap ? guest_rf_snap->values[0x01C0] : 0;
        uint32_t rb_cntl_reg =
            guest_rf_snap ? guest_rf_snap->values[0x01C1] : 0;
        uint32_t rb_rptr_reg =
            guest_rf_snap ? guest_rf_snap->values[0x01C4] : 0;
        uint32_t rb_wptr_reg =
            guest_rf_snap ? guest_rf_snap->values[0x01C5] : 0;
        XELOGI(
            "DEBUG: GPU idle snapshot: rb_base_reg={:08X} rb_cntl_reg={:08X} "
            "rb_rptr_reg={:08X} rb_wptr_reg={:08X} (guest CP_RB_*)",
            rb_base_reg, rb_cntl_reg, rb_rptr_reg, rb_wptr_reg);
        XELOGI(
            "DEBUG: GPU idle snapshot: primary_ptr={:08X} size={:08X} "
            "read_ptr_index_={:08X} write_ptr_index_={:08X}",
            primary_buffer_ptr_, primary_buffer_size_, read_ptr_index_,
            write_ptr_index);
        XELOGI(
            "DEBUG: GPU idle snapshot: rptr_writeback_ptr={:08X} "
            "rptr_update_freq={:08X}",
            read_ptr_writeback_ptr_, read_ptr_update_freq_);
        if (cvars::gpu_debug_watch_ringbuffer_writes) {
          XELOGI(
              "DEBUG: GPU idle snapshot: rb_watch_hits={} last_rb_write_ms={} "
              "last_rb_write_phys={:08X}",
              ring_buffer_watch_hits_.load(std::memory_order_relaxed),
              ring_buffer_last_write_ms_.load(std::memory_order_relaxed),
              ring_buffer_last_write_phys_.load(std::memory_order_relaxed));
        }
        if (guest_rf_snap) {
          auto fetch0 = guest_rf_snap->GetTextureFetch(0);
          XELOGI(
              "DEBUG: GPU idle snapshot: fetch0 dwords={:08X} {:08X} {:08X} "
              "{:08X} {:08X} {:08X}",
              fetch0.dword_0, fetch0.dword_1, fetch0.dword_2, fetch0.dword_3,
              fetch0.dword_4, fetch0.dword_5);
          XELOGI(
              "DEBUG: GPU idle snapshot: fetch0 type={} base_page=0x{:05X} "
              "mip_page=0x{:05X} format=0x{:X} endian=0x{:X} dimension=0x{:X}",
              static_cast<uint32_t>(fetch0.type), fetch0.base_address,
              fetch0.mip_address, static_cast<uint32_t>(fetch0.format),
              static_cast<uint32_t>(fetch0.endianness),
              static_cast<uint32_t>(fetch0.dimension));
          XELOGI(
              "DEBUG: GPU idle snapshot: fetch0 size_2d width={} height={} "
              "stack_depth={} pitch=0x{:X} tiled={} packed_mips={}",
              fetch0.size_2d.width, fetch0.size_2d.height,
              fetch0.size_2d.stack_depth, fetch0.pitch,
              fetch0.tiled ? "YES" : "NO", fetch0.packed_mips ? "YES" : "NO");
        }
        bool rb_host_reads_safe =
          (!cvars::gpu_debug_watch_ringbuffer_writes ||
           ring_watch_force_rw_mode_.load(std::memory_order_relaxed) ||
           ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed)) &&
          IsRingDebugReadLikelySafe(kernel_state_, primary_buffer_ptr_,
                                    primary_buffer_size_);
        if (cvars::gpu_debug_rb_dump_on_idle && reader_.buffer() &&
          rb_host_reads_safe) {
          uint32_t ring_dwords = primary_buffer_size_
                                     ? (primary_buffer_size_ / sizeof(uint32_t))
                                     : 0;
          DumpRingBufferSummary("idle snapshot", reader_.buffer(), ring_dwords,
                                read_ptr_index_, write_ptr_index,
                                primary_buffer_ptr_, primary_buffer_size_,
                                read_ptr_writeback_ptr_);
          uint32_t window = std::min<uint32_t>(
              cvars::gpu_debug_rb_dump_window_dwords,
              ring_dwords ? ring_dwords
                          : cvars::gpu_debug_rb_dump_window_dwords);
          uint32_t start = read_ptr_index_ > (window / 2)
                               ? (read_ptr_index_ - window / 2)
                               : 0;
          DumpRingBufferWindow("idle rb window (around read_ptr)",
                               reader_.buffer(), ring_dwords, start, window);
        } else if (cvars::gpu_debug_rb_dump_on_idle && reader_.buffer() &&
                   !rb_host_reads_safe) {
          XELOGW(
              "RING BUFFER: skipping idle snapshot ring read because watch "
              "protection is active (guest_write=NO, force_rw_mode=NO)");
        }
        last_idle_snapshot_ms = now_ms;
      }
    }

    // Lightweight ring buffer memory scan to detect writes without WPTR
    // updates (debug signal for missing CP_RB_WPTR writes).
    if (ring_buffer_initialized_.load() && cvars::gpu_debug_rb_scan_on_idle) {
      uint64_t now_ms = NowMs();
      if (now_ms - last_rb_scan_ms > 5000) {
        uint8_t* buffer = reader_.buffer();
        size_t capacity = static_cast<size_t>(primary_buffer_size_);
          bool rb_host_reads_safe =
            (!cvars::gpu_debug_watch_ringbuffer_writes ||
             ring_watch_force_rw_mode_.load(std::memory_order_relaxed) ||
             ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed)) &&
            IsRingDebugReadLikelySafe(kernel_state_, primary_buffer_ptr_,
                        primary_buffer_size_);
        if (buffer && capacity && rb_host_reads_safe) {
          size_t scan_bytes = std::min<size_t>(capacity, 4096);
          if (rb_scan_snapshot_bytes != scan_bytes) {
            rb_scan_snapshot.assign(scan_bytes, 0);
            rb_scan_snapshot_bytes = scan_bytes;
          }
          uint64_t hash = 1469598103934665603ull;
          for (size_t i = 0; i < scan_bytes; ++i) {
            hash ^= static_cast<uint64_t>(buffer[i]);
            hash *= 1099511628211ull;
          }
          if (!rb_hash_valid) {
            rb_hash_valid = true;
            last_rb_hash = hash;
            if (!rb_scan_snapshot.empty()) {
              std::copy(buffer, buffer + scan_bytes, rb_scan_snapshot.begin());
            }
            XELOGI("DEBUG: RB scan: initial hash=0x{:016X} (bytes_scanned={})",
                   hash, scan_bytes);
          } else if (hash != last_rb_hash) {
            XELOGW(
                "DEBUG: RB scan: memory changed without CP_RB_WPTR update "
                "(old=0x{:016X} new=0x{:016X})",
                last_rb_hash, hash);
            last_rb_hash = hash;
            size_t last_changed = SIZE_MAX;
            if (!rb_scan_snapshot.empty()) {
              for (size_t i = 0; i < scan_bytes; ++i) {
                if (rb_scan_snapshot[i] != buffer[i]) {
                  last_changed = i;
                }
              }
              std::copy(buffer, buffer + scan_bytes, rb_scan_snapshot.begin());
            }
            uint32_t last_nonzero = 0xFFFFFFFFu;
            size_t scan_dwords = scan_bytes / 4;
            for (size_t i = 0; i < scan_dwords; ++i) {
              uint32_t v = xe::load_and_swap<uint32_t>(buffer + i * 4);
              if (v != 0) {
                last_nonzero = static_cast<uint32_t>(i);
              }
            }
            XELOGI(
                "DEBUG: RB scan change details: last_changed_byte={} "
                "last_nonzero_dword={:08X} (scan_bytes={})",
                last_changed == SIZE_MAX ? -1 : static_cast<int>(last_changed),
                last_nonzero, scan_bytes);
            if (last_changed != SIZE_MAX) {
              size_t changed_dword = last_changed / 4;
              size_t window_start =
                  (changed_dword > 8) ? (changed_dword - 8) : 0;
              size_t window_end =
                  std::min<size_t>(scan_dwords, changed_dword + 9);
              XELOGI("DEBUG: RB dump near change (dword {}..{}):", window_start,
                     window_end - 1);
              for (size_t i = window_start; i < window_end; ++i) {
                uint32_t v = xe::load_and_swap<uint32_t>(buffer + i * 4);
                XELOGI("   rb[{:04X}] = {:08X}", static_cast<uint32_t>(i), v);
              }
            }
            bool allow_wptr_infer =
              cvars::gpu_force_wptr_on_rb_change ||
              cvars::gpu_auto_force_wptr_on_rb_change_if_idle;
            if (allow_wptr_infer && primary_buffer_size_ != 0) {
              bool rw_zero = (read_ptr_index_ == 0 && write_ptr_index_ == 0);
              bool rw_equal = (read_ptr_index_ == write_ptr_index_);
              bool wptr_stalled = (now_ms - last_write_ptr_change_ms) > 100;
              if (rw_zero || (rw_equal && wptr_stalled)) {
                uint32_t inferred_wptr = 0;
                bool inferred = false;
                if (last_changed != SIZE_MAX) {
                  inferred_wptr = static_cast<uint32_t>(last_changed / 4) + 1;
                  inferred = true;
                }
                if (last_nonzero != 0xFFFFFFFFu) {
                  uint32_t nonzero_wptr = last_nonzero + 1;
                  if (!inferred || nonzero_wptr > inferred_wptr) {
                    inferred_wptr = nonzero_wptr;
                    inferred = true;
                  }
                }
                if (inferred && inferred_wptr != write_ptr_index_) {
                  XELOGW(
                      "DEBUG: RB change w/o WPTR: inferring "
                      "write_ptr_index_={:08X} "
                      "(last_changed={} last_nonzero={:08X} read={:08X} "
                      "write={:08X}) and kicking worker",
                      inferred_wptr,
                      last_changed == SIZE_MAX ? -1
                                               : static_cast<int>(last_changed),
                      last_nonzero, read_ptr_index_, write_ptr_index_);
                  UpdateWritePointer(inferred_wptr, "RB_SCAN_INFERRED");
                } else if (!inferred) {
                  XELOGW(
                      "DEBUG: RB change w/o WPTR: no non-zero or changed dwords "
                      "found in scan window; leaving write_ptr_index_ "
                      "unchanged (read={:08X} write={:08X})",
                      read_ptr_index_, write_ptr_index_);
                } else {
                  XELOGW(
                      "DEBUG: RB change w/o WPTR: inferred write_ptr_index_={:08X} "
                      "matches current; leaving unchanged",
                      inferred_wptr);
                }
              }
            }
            if (last_nonzero == 0xFFFFFFFFu) {
              // If the scan window appears empty, do a deeper scan across the
              // full ring buffer (throttled) to find any non-zero packets.
              const uint64_t kFullScanMinMs = 10000;
              if (now_ms - last_rb_full_scan_ms > kFullScanMinMs) {
                last_rb_full_scan_ms = now_ms;
                size_t ring_dwords = primary_buffer_size_ / 4;
                uint32_t first_nz = 0xFFFFFFFFu;
                uint32_t last_nz = 0xFFFFFFFFu;
                for (size_t i = 0; i < ring_dwords; ++i) {
                  uint32_t v = xe::load_and_swap<uint32_t>(buffer + i * 4);
                  if (v != 0) {
                    if (first_nz == 0xFFFFFFFFu) {
                      first_nz = static_cast<uint32_t>(i);
                    }
                    last_nz = static_cast<uint32_t>(i);
                  }
                }
                if (last_nz != 0xFFFFFFFFu) {
                  XELOGW(
                      "DEBUG: RB full scan: found non-zero data (first={:08X} "
                      "last={:08X}, ring_dwords={})",
                      first_nz, last_nz, ring_dwords);
                  size_t dump_start = (last_nz > 8) ? (last_nz - 8) : 0;
                  size_t dump_end = std::min<size_t>(ring_dwords, last_nz + 9);
                  XELOGI("DEBUG: RB full scan dump near last_nz ({}..{}):",
                         dump_start, dump_end - 1);
                  for (size_t i = dump_start; i < dump_end; ++i) {
                    uint32_t v = xe::load_and_swap<uint32_t>(buffer + i * 4);
                    XELOGI("   rb[{:04X}] = {:08X}", static_cast<uint32_t>(i),
                           v);
                  }
                    if (allow_wptr_infer &&
                      read_ptr_index_ == write_ptr_index_ &&
                      last_nz + 1 != write_ptr_index_) {
                    uint32_t inferred_wptr = last_nz + 1;
                    XELOGW(
                        "DEBUG: RB full scan: inferring write_ptr_index_={:08X} "
                        "from last_nz and kicking worker",
                        inferred_wptr);
                    UpdateWritePointer(inferred_wptr,
                                       "RB_FULL_SCAN_INFERRED");
                  }
                } else {
                  XELOGW(
                      "DEBUG: RB full scan: ring buffer appears empty "
                      "(all zero)");
                  if (cvars::gpu_reset_wptr_on_rb_clear &&
                      read_ptr_index_ == write_ptr_index_ &&
                      write_ptr_index_ != 0 && !rb_clear_reset_done) {
                    XELOGW(
                        "DEBUG: RB clear detected: resetting read/write pointers "
                        "from {:08X} to 00000000 to match cleared ring",
                        write_ptr_index_);
                    read_ptr_index_ = 0;
                    UpdateWritePointer(0, "RB_CLEAR_RESET");
                    rb_clear_reset_done = true;
                  }
                }
              }
            }
            // Dump a small window of dwords at the start of the ring for
            // context.
            size_t dump_dwords = std::min<size_t>(
                static_cast<size_t>(primary_buffer_size_ / 4), 64);
            if (dump_dwords > 0) {
              uint32_t nonzero = 0;
              XELOGI(
                  "DEBUG: RB dump: first {} dwords (host={:p}, "
                  "guest_phys={:08X})",
                  dump_dwords, static_cast<void*>(buffer), primary_buffer_ptr_);
              for (size_t i = 0; i < dump_dwords; ++i) {
                uint32_t v = xe::load_and_swap<uint32_t>(buffer + i * 4);
                if (v != 0) {
                  if (nonzero < 16) {
                    XELOGI("   rb[{:04X}] = {:08X}", static_cast<uint32_t>(i),
                           v);
                  }
                  nonzero++;
                }
              }
              if (nonzero == 0) {
                XELOGI("   (all zero in first {} dwords)", dump_dwords);
              } else if (nonzero > 16) {
                XELOGI("   ... ({} additional non-zero dwords)", nonzero - 16);
              }
            }
          } else {
            XELOGI("DEBUG: RB scan: no change (hash=0x{:016X}, bytes_scanned={})",
                   hash, scan_bytes);
          }
        } else if (!rb_host_reads_safe) {
          XELOGW(
              "DEBUG: RB scan: skipped while watch protection is active "
              "(guest_write=NO, force_rw_mode=NO)");
        } else {
          XELOGW("DEBUG: RB scan: buffer not available (buffer={}, size={})",
                 static_cast<void*>(buffer), capacity);
        }
        last_rb_scan_ms = now_ms;
      }
    }

    // Periodic ring buffer dump while idle for deeper visibility.
    if (ring_buffer_initialized_.load()) {
      uint64_t now_ms = NowMs();
      if (now_ms - last_rb_dump_ms > 10000) {
        uint8_t* buffer = reader_.buffer();
        size_t capacity = static_cast<size_t>(primary_buffer_size_);
        bool rb_host_reads_safe =
          (!cvars::gpu_debug_watch_ringbuffer_writes ||
           ring_watch_force_rw_mode_.load(std::memory_order_relaxed) ||
           ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed)) &&
          IsRingDebugReadLikelySafe(kernel_state_, primary_buffer_ptr_,
                                    primary_buffer_size_);
        if (buffer && capacity && rb_host_reads_safe) {
          size_t dump_dwords = std::min<size_t>(
              static_cast<size_t>(primary_buffer_size_ / 4), 64);
          uint32_t nonzero = 0;
          XELOGI(
              "DEBUG: RB idle dump: first {} dwords (host={:p}, "
              "guest_phys={:08X})",
              dump_dwords, static_cast<void*>(buffer), primary_buffer_ptr_);
          for (size_t i = 0; i < dump_dwords; ++i) {
            uint32_t v = xe::load_and_swap<uint32_t>(buffer + i * 4);
            if (v != 0) {
              if (nonzero < 16) {
                XELOGI("   rb[{:04X}] = {:08X}", static_cast<uint32_t>(i), v);
              }
              nonzero++;
            }
          }
          if (nonzero == 0) {
            XELOGI("   (all zero in first {} dwords)", dump_dwords);
          } else if (nonzero > 16) {
            XELOGI("   ... ({} additional non-zero dwords)", nonzero - 16);
          }
        } else if (!rb_host_reads_safe) {
          XELOGW(
              "DEBUG: RB idle dump: skipped while watch protection is active "
              "(guest_write=NO, force_rw_mode=NO)");
        } else {
          XELOGW("DEBUG: RB idle dump: buffer unavailable (buffer={}, size={})",
                 static_cast<void*>(buffer), capacity);
        }
        last_rb_dump_ms = now_ms;
      }
    }

    // If the ring buffer isn't initialized yet, wait for initialization.
    if (!ring_buffer_initialized_.load()) {
      PrepareForWait();
      continue;
    }
    // If no work is available yet, wait for a real write pointer update.
    if (read_ptr_index_ == write_ptr_index) {
      if (cvars::gpu_debug_watch_ringbuffer_writes &&
          pending_watch_rearm_after_processing) {
        XELOGW(
            "RING BUFFER: watch rearm remains deferred while idle "
            "(read==write=={:08X}); waiting for WPTR advance before re-arm",
            write_ptr_index);
      }
      if (cvars::gpu_debug_watch_ringbuffer_writes &&
          !ring_watch_force_rw_mode_.load(std::memory_order_relaxed) &&
          !ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed)) {
        XELOGW(
            "RING BUFFER: idle wait with non-writable watch state "
            "(guest_write=NO, force_rw_mode=NO) read={:08X} write={:08X}",
            read_ptr_index_, write_ptr_index);
        LogRingBufferProtectionState("idle-wait-nonwritable",
                                     primary_buffer_ptr_, primary_buffer_size_);
      }
        bool rb_host_reads_safe =
          !cvars::gpu_debug_watch_ringbuffer_writes ||
          ring_watch_force_rw_mode_.load(std::memory_order_relaxed) ||
          ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed);
        bool prelaunch_module_load_inflight = false;
        if (kernel_state_) {
          auto load_stats = kernel_state_->GetUserModuleLoadStats();
          prelaunch_module_load_inflight =
              load_stats.load_inflight &&
              load_stats.loaded_executable_user_modules == 0;
        }
        if (cvars::gpu_debug_rb_dump_on_wait && reader_.buffer() &&
            rb_host_reads_safe && !prelaunch_module_load_inflight) {
        uint32_t ring_dwords = primary_buffer_size_
                                   ? (primary_buffer_size_ / sizeof(uint32_t))
                                   : 0;
        DumpRingBufferSummary("pre-wait", reader_.buffer(), ring_dwords,
                              read_ptr_index_, write_ptr_index,
                              primary_buffer_ptr_, primary_buffer_size_,
                              read_ptr_writeback_ptr_);
        uint32_t window = std::min<uint32_t>(
            cvars::gpu_debug_rb_dump_window_dwords,
            ring_dwords ? ring_dwords : cvars::gpu_debug_rb_dump_window_dwords);
        uint32_t start =
            read_ptr_index_ > (window / 2) ? (read_ptr_index_ - window / 2) : 0;
        DumpRingBufferWindow("pre-wait rb window (around read_ptr)",
                             reader_.buffer(), ring_dwords, start, window);
        } else if (cvars::gpu_debug_rb_dump_on_wait && reader_.buffer() &&
                   !rb_host_reads_safe) {
        XELOGW(
          "RING BUFFER: skipping pre-wait ring read because watch "
          "protection is active (guest_write=NO, force_rw_mode=NO)");
      } else if (cvars::gpu_debug_rb_dump_on_wait && reader_.buffer() &&
                 prelaunch_module_load_inflight) {
        static std::atomic<uint64_t> prelaunch_dump_skip_count{0};
        uint64_t skip_id = prelaunch_dump_skip_count.fetch_add(1) + 1;
        if (skip_id <= 5 || (skip_id % 120) == 0) {
          XELOGI(
              "RING BUFFER: skipping pre-wait ring dump while module load is "
              "inflight and no executable module is ready (count={})",
              skip_id);
        }
      }
      PrepareForWait();
      continue;
    }

    ReturnFromWait();
    ExecutePendingFunctions();
    // WaitAny above already performed the wake synchronization. Don't wait a
    // second time here - that can require an extra signal and stall single
    // WPTR updates.
    write_ptr_index = write_ptr_index_.load(std::memory_order_relaxed);

    // Handle race condition where ring buffer is empty after wait
    if (read_ptr_index_ == write_ptr_index) {
      // Check if primary buffer is valid - if not, we're in an invalid state
      bool primary_buffer_valid =
          primary_buffer_ptr_ != 0 && primary_buffer_size_ > 0;

      if (!primary_buffer_valid) {
        XELOGE(
            "DEBUG: CRITICAL: Primary buffer invalid (ptr={:08X}, size={}) - "
            "aborting worker loop",
            primary_buffer_ptr_, primary_buffer_size_);

        // Force a sleep to prevent CPU spinning and break the infinite loop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Skip back to the wait condition instead of continuing
        continue;
      }

      // Count consecutive race conditions to detect loops
      static int consecutive_races = 0;
      consecutive_races++;

      // Log at decreasing frequency to avoid log spam
      if (consecutive_races == 1 || consecutive_races % 1000 == 0) {
        XELOGW(
            "DEBUG: Ring buffer race #{}: read={:#x}, write={:#x} - primary "
            "buffer "
            "valid: {}",
            consecutive_races, read_ptr_index_, write_ptr_index,
            primary_buffer_valid);
      }

      // Add throttling to prevent CPU spinning
      if (consecutive_races > 10) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }

      continue;
    }

    // Reset race counter when we have work to do
    if (read_ptr_index_ != write_ptr_index) {
      static int consecutive_races = 0;
      consecutive_races = 0;
    }

    // Execute. Note that we handle wraparound transparently.
    uint32_t old_read_ptr = read_ptr_index_;
    if (cvars::gpu_debug_rb_dump_on_activity && reader_.buffer() &&
      IsRingDebugReadLikelySafe(kernel_state_, primary_buffer_ptr_,
                    primary_buffer_size_)) {
      uint32_t ring_dwords =
          primary_buffer_size_ ? (primary_buffer_size_ / sizeof(uint32_t)) : 0;
      DumpRingBufferSummary("activity pre-exec", reader_.buffer(), ring_dwords,
                            read_ptr_index_, write_ptr_index,
                            primary_buffer_ptr_, primary_buffer_size_,
                            read_ptr_writeback_ptr_);
      uint32_t window = std::min<uint32_t>(
          cvars::gpu_debug_rb_dump_window_dwords,
          ring_dwords ? ring_dwords : cvars::gpu_debug_rb_dump_window_dwords);
      uint32_t start_read =
          read_ptr_index_ > (window / 2) ? (read_ptr_index_ - window / 2) : 0;
      uint32_t start_write =
          write_ptr_index > (window / 2) ? (write_ptr_index - window / 2) : 0;
      DumpRingBufferWindow("activity rb window (around read_ptr)",
                           reader_.buffer(), ring_dwords, start_read, window);
      DumpRingBufferWindow("activity rb window (around write_ptr)",
                           reader_.buffer(), ring_dwords, start_write, window);
    }
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);
    uint32_t ring_dwords = primary_buffer_size_ / sizeof(uint32_t);
    uint32_t processed_dwords = 0;
    if (ring_dwords > 0) {
      if (write_ptr_index >= old_read_ptr) {
        processed_dwords = write_ptr_index - old_read_ptr;
      } else {
        processed_dwords = (ring_dwords - old_read_ptr) + write_ptr_index;
      }
    }
    XELOGI(
        "DEBUG: GPU activity: processed {} dwords (read {:08X} -> {:08X}, write "
        "{:08X})",
        processed_dwords, old_read_ptr, read_ptr_index_, write_ptr_index);
    if (processed_dwords > 0 && graphics_system_ &&
        graphics_system_->GetInterruptCallback() != 0) {
      graphics_system_->DispatchInterruptCallback(0, 2);
    }
    if (cvars::gpu_debug_watch_ringbuffer_writes &&
        pending_watch_rearm_after_processing && processed_dwords > 0) {
      EnableRingBufferWriteWatch("post-process-rearm");
      pending_watch_rearm_after_processing = false;
    }
    if (cvars::gpu_debug_rb_dump_on_activity && reader_.buffer()) {
      uint32_t window = std::min<uint32_t>(
          cvars::gpu_debug_rb_dump_window_dwords,
          ring_dwords ? ring_dwords : cvars::gpu_debug_rb_dump_window_dwords);
      uint32_t start =
          read_ptr_index_ > (window / 2) ? (read_ptr_index_ - window / 2) : 0;
      DumpRingBufferWindow("activity rb window (post-exec read_ptr)",
                           reader_.buffer(), ring_dwords, start, window);
    }

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after
    // moving
    //     that many indices.
    // Keep in mind that the gpu also updates the cpu-side copy if the write
    // pointer and read pointer would be equal
    if (read_ptr_writeback_ptr_) {
      xe::store_and_swap<uint32_t>(
          memory_->TranslatePhysical(read_ptr_writeback_ptr_), read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void gpu::CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  threading::Fence fence;
  CallInThread(
      [&fence, this]() {
        fence.Signal();
        worker_thread_->thread()->Suspend();
      },
      "pause-suspend-worker");

  fence.Wait();
}

void gpu::CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool gpu::CommandProcessor::Save(ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool gpu::CommandProcessor::Restore(ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool gpu::CommandProcessor::SetupContext() { return true; }

void gpu::CommandProcessor::ShutdownContext() {}

void gpu::CommandProcessor::InitializeRingBuffer(uint32_t ptr,
                                                 uint32_t size_log2) {
  uint64_t init_request_id =
    init_request_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t init_request_ms = NowMs();
  last_init_request_ms_.store(init_request_ms, std::memory_order_relaxed);
  XELOGI("RING BUFFER: InitializeRingBuffer ENTRY ptr={:08X} size_log2={}", ptr,
         size_log2);
  XELOGI(
    "RING BUFFER: InitializeRingBuffer request_id={} request_ms={} "
    "previous_ack_transitions={} last_ack_ms={}",
    init_request_id, init_request_ms,
    init_ack_transition_count_.load(std::memory_order_relaxed),
    last_init_ack_ms_.load(std::memory_order_relaxed));

#ifdef __APPLE__
  // macOS debugging: Critical for GPU Commands thread (01000010) hang
  // investigation
  if (worker_thread_ && worker_thread_->handle() == 0x01000010) {
    XELOGI("DEBUG: CRITICAL: InitializeRingBuffer called for GPU Commands thread");
    XELOGI("   ptr = {:08X}", ptr);
    XELOGI("   size_log2 = {}", size_log2);
    XELOGI("   primary_buffer_ptr_ = {:08X}", ptr);
    XELOGI("   primary_buffer_size_ = {:08X}", uint32_t(1) << (size_log2 + 3));
  }
#endif

  // Calculate buffer size
  uint32_t buffer_size = uint32_t(1) << (size_log2 + 3);

  // Validate buffer parameters FIRST before any state changes
  if (ptr == 0 || buffer_size == 0) {
    XELOGE(
        "RING BUFFER: InitializeRingBuffer invalid parameters ptr={:08X} "
        "size={:08X}",
        ptr, buffer_size);
    return;
  }

  // Validate that the requested ring range is actually backed by committed
  // guest memory before mutating worker-visible ring state.
  auto* memory = kernel_state_ ? kernel_state_->memory() : nullptr;
  if (!memory) {
    XELOGE("RING BUFFER: InitializeRingBuffer aborted: memory system unavailable");
    return;
  }
  auto* heap = memory->LookupHeap(ptr);
  if (!heap) {
    XELOGE(
        "RING BUFFER: InitializeRingBuffer aborted: no heap for rb_base={:08X} "
        "rb_size={:08X}",
        ptr, buffer_size);
    return;
  }
  auto has_committed_access = [&](uint32_t addr) -> bool {
    uint32_t guest_protect = 0;
    if (!heap->QueryProtect(addr, &guest_protect)) {
      return false;
    }
    return guest_protect != 0;
  };
  uint32_t rb_end = ptr + buffer_size - 1;
  uint32_t rb_mid = ptr + (buffer_size / 2);
  if (!has_committed_access(ptr) || !has_committed_access(rb_mid) ||
      !has_committed_access(rb_end)) {
    uint32_t p0 = 0, p1 = 0, p2 = 0;
    heap->QueryProtect(ptr, &p0);
    heap->QueryProtect(rb_mid, &p1);
    heap->QueryProtect(rb_end, &p2);
    XELOGE(
        "RING BUFFER: InitializeRingBuffer aborted: rb range appears "
        "uncommitted/inaccessible rb=[{:08X}..{:08X}] prot_start={:08X} "
        "prot_mid={:08X} prot_end={:08X}",
        ptr, rb_end, p0, p1, p2);
    return;
  }

  // Keep state constant: if guest re-writes the same CP_RB_BASE/CP_RB_CNTL,
  // do not re-initialize (would reset read_ptr_index_/write_ptr_index_ and
  // lose progress). Only re-init when base or size actually changes.
  if (ring_buffer_initialized_.load() && primary_buffer_ptr_ == ptr &&
      primary_buffer_size_ == buffer_size) {
    XELOGI(
        "DEBUG: InitializeRingBuffer: same config (ptr={:08X} size={:08X}), "
        "keeping ring buffer state constant",
        ptr, buffer_size);
    return;
  }

  XELOGI(
      "DEBUG: InitializeRingBuffer: Validation passed, proceeding with "
      "initialization");

  // CRITICAL FIX 1: Reset acknowledgment flag before initialization
  initialization_acknowledged_.store(false);
  ring_buffer_watch_rearm_requested_.store(false, std::memory_order_relaxed);
  ring_buffer_watch_arm_count_.store(0, std::memory_order_relaxed);
  ring_buffer_watch_last_arm_ms_.store(0, std::memory_order_relaxed);
  ring_watch_last_transition_ms_.store(0, std::memory_order_relaxed);
  ring_watch_last_phase_id_.store(0, std::memory_order_relaxed);
  ring_watch_last_range_start_.store(0, std::memory_order_relaxed);
  ring_watch_last_range_size_.store(0, std::memory_order_relaxed);
  ring_watch_last_guest_write_enabled_.store(false,
                                             std::memory_order_relaxed);
  ring_watch_restore_verify_pending_.store(false, std::memory_order_relaxed);
  ring_watch_restore_verify_phys_.store(0, std::memory_order_relaxed);
  ring_watch_restore_verify_len_.store(0, std::memory_order_relaxed);
  ring_watch_force_rw_mode_.store(false, std::memory_order_relaxed);

  // Set initialization flag to false FIRST to prevent worker from using
  // buffer
  ring_buffer_initialized_.store(false);
  std::atomic_thread_fence(std::memory_order_seq_cst);  // Ensure visibility

  XELOGI("DEBUG: InitializeRingBuffer: Set ring_buffer_initialized_ = false");

  // Now set buffer parameters
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = buffer_size;
  read_ptr_index_ = 0;

  // Initialize write pointer to 0 - do not inject synthetic work into the
  // ring. With initialization_complete_event_ signaling, the worker can wake
  // without consuming any fake packets.
  write_ptr_index_.store(0);

  XELOGI("DEBUG: InitializeRingBuffer: Buffer parameters set");
  XELOGI("   primary_buffer_ptr_ = {:08X}", primary_buffer_ptr_);
  XELOGI("   primary_buffer_size_ = {:08X}", primary_buffer_size_);
  XELOGI("   write_ptr_index_ = {:08X}", write_ptr_index_.load());
  XELOGI("   read_ptr_index_ = {:08X}", read_ptr_index_);

  // Clear buffer memory - this is where the hang might occur
  XELOGI("DEBUG: InitializeRingBuffer: About to translate physical address {:08X}",
         primary_buffer_ptr_);

  // Ensure the ring memory is writable before touching it. Previous watch-arm
  // cycles or stale protection state may leave pages non-writable and hang
  // during memset.
  uint32_t rw_protect = kMemoryProtectRead | kMemoryProtectWrite |
                        kMemoryProtectWriteCombine;
  bool ring_protect_ok =
      heap->Protect(primary_buffer_ptr_, primary_buffer_size_, rw_protect,
                    nullptr);
  if (!ring_protect_ok) {
    XELOGE(
        "RING BUFFER: InitializeRingBuffer failed to restore RW protection "
        "rb=[{:08X}..{:08X}) size={:08X}",
        primary_buffer_ptr_, primary_buffer_ptr_ + primary_buffer_size_,
        primary_buffer_size_);
    primary_buffer_ptr_ = 0;
    primary_buffer_size_ = 0;
    read_ptr_index_ = 0;
    write_ptr_index_.store(0);
    return;
  }
  if (cvars::gpu_debug_watch_ringbuffer_writes) {
    XELOGI(
        "RING BUFFER: InitializeRingBuffer restored RW protection before "
        "clear rb=[{:08X}..{:08X}) size={:08X}",
        primary_buffer_ptr_, primary_buffer_ptr_ + primary_buffer_size_,
        primary_buffer_size_);
  }

  auto* buffer_ptr =
      kernel_state_->memory()->TranslatePhysical(primary_buffer_ptr_);

  if (!buffer_ptr) {
    XELOGE(
        "RING BUFFER: InitializeRingBuffer failed to translate rb_base={:08X}",
        primary_buffer_ptr_);
    // Reset state to prevent worker from using invalid buffer
    primary_buffer_ptr_ = 0;
    primary_buffer_size_ = 0;
    return;
  }

  XELOGI(
      "DEBUG: InitializeRingBuffer: Physical address translated successfully to "
      "host address {:p}",
      static_cast<void*>(buffer_ptr));
  XELOGI("DEBUG: InitializeRingBuffer: About to clear {} bytes of buffer memory",
         primary_buffer_size_);

  std::memset(buffer_ptr, 0, primary_buffer_size_);

  XELOGI("DEBUG: InitializeRingBuffer: Buffer memory cleared successfully");

  // Seed the ring buffer with PM4 NOPs only in the explicit zero-R/W abort
  // debugging mode. Keep normal behavior fully guest-driven.
  uint32_t seed_nop_dwords = cvars::gpu_seed_ringbuffer_nop_dwords;
  if (seed_nop_dwords > 0 && !cvars::gpu_allow_pm4_nop_ring_seed) {
    XELOGW(
        "DEBUG: Ignoring gpu_seed_ringbuffer_nop_dwords={} because "
        "gpu_allow_pm4_nop_ring_seed=false",
        seed_nop_dwords);
    seed_nop_dwords = 0;
  }
  if (seed_nop_dwords > 0 && cvars::gpu_zero_rwptr_abort_ms == 0) {
    XELOGW(
        "DEBUG: Ignoring gpu_seed_ringbuffer_nop_dwords={} because "
        "gpu_zero_rwptr_abort_ms is 0 (guest-driven mode)",
        seed_nop_dwords);
    seed_nop_dwords = 0;
  }
  if (seed_nop_dwords > 0 && write_ptr_index_.load() == 0 &&
      read_ptr_index_ == 0) {
    uint32_t executable_entry = 0;
    if (kernel_state_) {
      auto executable = kernel_state_->GetExecutableModule();
      if (executable) {
        executable_entry = executable->entry_point();
      }
    }
    XELOGW(
        "RING BUFFER: PM4 NOP seed enabled for debug (rb_base={:08X} "
        "rb_size={:08X} executable_entry={:08X}) - seed affects ring-buffer "
        "memory only, not guest code pages.",
        primary_buffer_ptr_, primary_buffer_size_, executable_entry);
    uint32_t ring_dwords = primary_buffer_size_ / 4;
    uint32_t seed_dwords = std::min<uint32_t>(seed_nop_dwords, ring_dwords);
    if (seed_dwords > 0) {
      constexpr uint32_t kPacketType2Nop = (0x2u << 30);
      for (uint32_t i = 0; i < seed_dwords; ++i) {
        auto* dst =
            reinterpret_cast<uint32_t*>(buffer_ptr + i * sizeof(uint32_t));
        xe::store_and_swap<uint32_t>(dst, kPacketType2Nop);
      }
      write_ptr_index_.store(seed_dwords);
      XELOGI(
          "DEBUG: Seeded ring buffer with {} PM4 Type2 no-op dwords; write_ptr_index_ now "
          "{:08X}",
          seed_dwords, write_ptr_index_.load());
    }
  }

  // Reinitialize the RingBuffer reader with the translated buffer
  // We need to reconstruct it because RingBuffer doesn't have a setter for
  // buffer/capacity
  XELOGI(
      "DEBUG: InitializeRingBuffer: Reconstructing RingBuffer reader with buffer "
      "{:p}, capacity {}",
      static_cast<void*>(buffer_ptr), primary_buffer_size_);

  reader_.~RingBuffer();  // Destruct old instance

  new (&reader_) RingBuffer(
      buffer_ptr, primary_buffer_size_);  // Placement new to reconstruct

  // CRITICAL FIX 3: Enhanced signaling mechanism for macOS robustness
  // Create and initialize the complete event if not already created
  if (!initialization_complete_event_) {
    initialization_complete_event_ =
        xe::threading::Event::CreateManualResetEvent(false);
  }

  // Mark as properly initialized LAST, after all state is valid
  {
    std::lock_guard<std::mutex> lock(ring_buffer_ready_mutex_);
    ring_buffer_initialized_.store(true);
    if (cvars::gpu_debug_watch_ringbuffer_writes && kernel_state_) {
      ring_buffer_watch_rearm_requested_.store(false,
                                               std::memory_order_relaxed);
      EnableRingBufferWriteWatch("init-complete");
    }
    initialization_complete_event_
        ->Set();  // Manual reset - stays signaled until acknowledged
    last_init_signal_ms_.store(NowMs(), std::memory_order_relaxed);
    XELOGI(
        "DEBUG: InitializeRingBuffer: Set ring_buffer_initialized_ = true, "
        "signaling completion event");
    ring_buffer_ready_cv_.notify_all();
  }

  if (!worker_running_.load(std::memory_order_relaxed)) {
    initialization_acknowledged_.store(true, std::memory_order_relaxed);
    init_ack_transition_count_.fetch_add(1, std::memory_order_relaxed);
    last_init_ack_ms_.store(NowMs(), std::memory_order_relaxed);
  }

  // Additional safety: Signal write_ptr_index_event to wake worker from wait
  write_ptr_index_event_->Set();

  XELOGI(
      "DEBUG: InitializeRingBuffer: Signaled both write_ptr_index_event and "
      "initialization_complete_event");

#ifdef __APPLE__
  if (cvars::gpu_compat_mac_ringbuffer_double_signal) {
    // Optional compatibility path for investigating missed signals on macOS.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    write_ptr_index_event_->Set();
    XELOGI(
        "DEBUG: InitializeRingBuffer: Second signal sent for macOS compatibility");

    if (!worker_thread_) {
      XELOGI(
          "DEBUG: InitializeRingBuffer: Worker thread not started yet - skipping "
          "acknowledgment wait");
      initialization_acknowledged_.store(true);
    } else {
      auto start_time = std::chrono::steady_clock::now();
      const auto soft_timeout = std::chrono::milliseconds(100);
      const auto hard_timeout = std::chrono::milliseconds(1000);
      bool nudged = false;

      while (!initialization_acknowledged_.load()) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (!nudged && elapsed > soft_timeout) {
          XELOGI(
              "DEBUG: InitializeRingBuffer: No acknowledgment after {}ms - "
              "nudging worker thread",
              soft_timeout.count());
          write_ptr_index_event_->Set();
          initialization_complete_event_->Set();
          ring_buffer_ready_cv_.notify_all();
          nudged = true;
        }
        if (elapsed > hard_timeout) {
          XELOGW(
              "DEBUG: InitializeRingBuffer: Worker thread acknowledgment timeout "
              "after {}ms (worker_running={}, thread_handle={})",
              hard_timeout.count(), worker_running_.load(),
              worker_thread_ ? worker_thread_->handle() : 0);
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      if (initialization_acknowledged_.load()) {
        XELOGI(
            "DEBUG: InitializeRingBuffer: Worker thread successfully acknowledged "
            "initialization");
      }
    }

    if (worker_thread_ && worker_thread_->handle() == 0x01000010) {
      XELOGI(
          "DEBUG: ENHANCED: InitializeRingBuffer completed with full "
          "synchronization");
      XELOGI("   write_ptr_index: {:08X}", write_ptr_index_.load());
      XELOGI("   read_ptr_index_: {:08X}", read_ptr_index_);
      XELOGI("   ring_buffer_initialized_: TRUE");
      XELOGI("   Worker thread notified via condition_variable AND event");
    }
  }
#endif

  XELOGI(
      "RING BUFFER: InitializeRingBuffer COMPLETE rb_base={:08X} rb_size={:08X} "
      "read_ptr={:08X} write_ptr={:08X} init={}",
      primary_buffer_ptr_, primary_buffer_size_, read_ptr_index_,
      write_ptr_index_.load(), ring_buffer_initialized_.load() ? "YES" : "NO");
}

bool gpu::CommandProcessor::IsRingBufferInitialized() const {
  return ring_buffer_initialized_.load();
}

void gpu::CommandProcessor::ForceFallbackInitialization() {
  XELOGI("DEBUG: Forcing fallback GPU initialization");

  // Try to initialize with default parameters if nothing is set
  if (primary_buffer_ptr_ == 0 || primary_buffer_size_ == 0) {
    XELOGI("DEBUG: No ring buffer configured, trying default initialization");
    const uint32_t default_size_log2 = 18;
    const uint32_t default_size = uint32_t(1) << (default_size_log2 + 3);
    uint32_t rb_ptr = kernel_state_ && kernel_state_->memory()
                          ? kernel_state_->memory()->SystemHeapAlloc(
                                default_size, 4096, kSystemHeapPhysical)
                          : 0;
    if (rb_ptr == 0) {
      XELOGE(
          "RING BUFFER: fallback init failed: unable to allocate {} bytes "
          "for ring buffer",
          default_size);
      return;
    }
    XELOGI("RING BUFFER: fallback allocated rb at {:08X} size={:08X}", rb_ptr,
           default_size);
    // If the worker isn't running yet (startup path), initialize immediately
    // so worker loop #1 can observe an initialized ring.
    if (!worker_running_.load(std::memory_order_relaxed) || !worker_thread_) {
      XELOGI(
          "RING BUFFER: fallback init executing immediately (pre-worker) "
          "(rb_base={:08X} size_log2={})",
          rb_ptr, default_size_log2);
      InitializeRingBuffer(rb_ptr, default_size_log2);
    } else {
      // Schedule fallback ring initialization in the GPU worker thread to
      // avoid cross-thread re-entrancy / lock ordering issues when fallback is
      // triggered from non-worker threads (such as the frame limiter).
      CallInThread(
          [this, rb_ptr, default_size_log2]() {
            InitializeRingBuffer(rb_ptr, default_size_log2);
          },
          "force-fallback-initialize-ring-buffer");
      XELOGI(
          "RING BUFFER: fallback init scheduled on GPU worker thread "
          "(rb_base={:08X} size_log2={})",
          rb_ptr, default_size_log2);
    }
  }

  // Ensure interrupt callback is set
  if (!interrupt_callback_) {
    XELOGW(
        "DEBUG: Fallback initialization left interrupt callback unset "
        "(waiting for guest VdSetGraphicsInterruptCallback)");
  }

  XELOGI("DEBUG: Fallback initialization forced");
}

void gpu::CommandProcessor::SetInterruptCallback(uint32_t callback,
                                                 uint32_t user_data) {
  static std::atomic<uint64_t> set_count{0};
  uint64_t call_id = set_count.fetch_add(1) + 1;
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

  interrupt_callback_ = callback;
  interrupt_user_data_ = user_data;

  // Notify graphics system of callback registration
  auto graphics_sys = kernel_state_->emulator()->graphics_system();
  if (graphics_sys) {
    graphics_sys->NotifyVdSetGraphicsInterruptCallbackCall(0, 0);
  }
}

#if 0
// [Antigravity]: The following block appears to be a duplicate of InitializeRingBuffer logic
// and was causing compilation errors because it was at global scope.
// It is preserved here as requested by the user.

// Calculate buffer size
uint32_t buffer_size = uint32_t(1) << (size_log2 + 3);
...
}
#endif

void gpu::CommandProcessor::LogRingBufferProtectionState(const char* phase,
                                                         uint32_t hit_phys,
                                                         uint32_t hit_len) {
  if (!cvars::gpu_debug_watch_ringbuffer_log_protection) {
    return;
  }
  if (!kernel_state_) {
    return;
  }
  auto* memory = kernel_state_->memory();
  if (!memory || primary_buffer_ptr_ == 0 || primary_buffer_size_ == 0) {
    return;
  }
  auto* heap = memory->LookupHeap(primary_buffer_ptr_);
  if (!heap) {
    XELOGW(
        "RING BUFFER: watch-protect phase={} heap lookup failed for rb_base="
        "{:08X} rb_size={:08X}",
        phase ? phase : "unknown", primary_buffer_ptr_, primary_buffer_size_);
    return;
  }

  uint32_t page_size = std::max<uint32_t>(heap->page_size(), 1u);
  uint32_t page_mask = ~(page_size - 1);
  uint32_t rb_start = primary_buffer_ptr_;
  uint32_t rb_end = rb_start + primary_buffer_size_ - 1;
  uint32_t aligned_start = rb_start & page_mask;
  uint64_t aligned_end_exclusive_u64 =
      (static_cast<uint64_t>(rb_end) + page_size) & page_mask;
  uint32_t aligned_end_exclusive = static_cast<uint32_t>(aligned_end_exclusive_u64);
  uint32_t aligned_size = aligned_end_exclusive - aligned_start;

  bool alignment_ok = aligned_start <= rb_start &&
                      aligned_end_exclusive >= (rb_end + 1) &&
                      (aligned_start % page_size) == 0 &&
                      (aligned_end_exclusive % page_size) == 0;
  if (!alignment_ok) {
    XELOGW(
        "RING BUFFER: watch-protect alignment mismatch phase={} "
        "rb=[{:08X}..{:08X}] aligned=[{:08X}..{:08X}) page_size={}",
        phase ? phase : "unknown", rb_start, rb_end, aligned_start,
        aligned_end_exclusive, page_size);
  }

  auto host_access_name = [](xe::memory::PageAccess access) {
    switch (access) {
      case xe::memory::PageAccess::kNoAccess:
        return "NO_ACCESS";
      case xe::memory::PageAccess::kReadOnly:
        return "READ_ONLY";
      case xe::memory::PageAccess::kReadWrite:
        return "READ_WRITE";
      case xe::memory::PageAccess::kExecuteReadOnly:
        return "EXECUTE_READ_ONLY";
      case xe::memory::PageAccess::kExecuteReadWrite:
        return "EXECUTE_READ_WRITE";
      default:
        return "UNKNOWN";
    }
  };

  bool any_guest_write = false;
  bool any_host_query = false;
  bool any_host_writable = false;

  auto log_point = [&](const char* label, uint32_t addr) {
    uint32_t guest_protect = 0;
    bool guest_queried = heap->QueryProtect(addr, &guest_protect);
    void* host_ptr = memory->TranslatePhysical(addr);
    size_t host_query_len = xe::memory::page_size();
    xe::memory::PageAccess host_access = xe::memory::PageAccess::kNoAccess;
    bool host_queried = false;
    if (host_ptr) {
      host_queried =
          xe::memory::QueryProtect(host_ptr, host_query_len, host_access);
    }
#if XE_PLATFORM_MAC
    if (host_ptr && !host_queried) {
      static std::atomic<bool> queryprotect_warned{false};
      bool expected = false;
      if (queryprotect_warned.compare_exchange_strong(
              expected, true, std::memory_order_relaxed)) {
        XELOGW(
            "RING BUFFER: host protection query unavailable on this macOS "
            "path (QueryProtect returned false). Use MEMORY WATCH arm/"
            "unprotect logs for definitive mprotect transitions.");
      }
    }
#endif
    XELOGI(
        "RING BUFFER: watch-protect phase={} point={} addr={:08X} "
        "guest_query={} guest_protect={:08X} guest_read={} guest_write={} "
        "guest_wc={} host_ptr={:p} host_query={} host_access={} "
        "host_len={}",
        phase ? phase : "unknown", label ? label : "?", addr,
        guest_queried ? "YES" : "NO", guest_protect,
        (guest_protect & kMemoryProtectRead) ? "YES" : "NO",
        (guest_protect & kMemoryProtectWrite) ? "YES" : "NO",
        (guest_protect & kMemoryProtectWriteCombine) ? "YES" : "NO",
        host_ptr, host_queried ? "YES" : "NO", host_access_name(host_access),
        host_query_len);
      any_guest_write =
        any_guest_write || (guest_protect & kMemoryProtectWrite) ||
        (guest_protect & kMemoryProtectWriteCombine);
      any_host_query = any_host_query || host_queried;
      any_host_writable =
        any_host_writable ||
        (host_queried && host_access == xe::memory::PageAccess::kReadWrite);
  };

  XELOGI(
      "RING BUFFER: watch-protect phase={} rb=[{:08X}..{:08X}] "
      "aligned=[{:08X}..{:08X}) aligned_size={:08X} page_size={} "
      "hit_phys={:08X} hit_len={:X}",
      phase ? phase : "unknown", rb_start, rb_end, aligned_start,
      aligned_end_exclusive, aligned_size, page_size, hit_phys, hit_len);

  log_point("start", rb_start);
  log_point("middle", rb_start + primary_buffer_size_ / 2);
  log_point("end", rb_end);

  uint64_t now_ms = NowMs();
  uint32_t phase_id = RingWatchPhaseIdFromName(phase);
  ring_watch_last_transition_ms_.store(now_ms, std::memory_order_relaxed);
  ring_watch_last_phase_id_.store(phase_id, std::memory_order_relaxed);
  ring_watch_last_range_start_.store(aligned_start, std::memory_order_relaxed);
  ring_watch_last_range_size_.store(aligned_size, std::memory_order_relaxed);
  ring_watch_last_guest_write_enabled_.store(any_guest_write,
                                             std::memory_order_relaxed);

  XELOGI(
      "RING BUFFER: watch-protect summary phase={} phase_id={} "
      "guest_write_any={} host_query_any={} host_writable_any={} "
      "tracked_range=[{:08X}..{:08X}) tracked_size={:08X} now_ms={}",
      phase ? phase : "unknown", phase_id, any_guest_write ? "YES" : "NO",
      any_host_query ? "YES" : "NO", any_host_writable ? "YES" : "NO",
      aligned_start, aligned_end_exclusive, aligned_size, now_ms);
  if (phase &&
      (std::string_view(phase) == "watch-arm-after" ||
       std::string_view(phase) == "watch-hit-post-unprotect") &&
      !any_guest_write) {
    XELOGW(
        "RING BUFFER: watch-protect post-phase not writable phase={} "
        "(guest_write_any=NO) range=[{:08X}..{:08X})",
        phase, aligned_start, aligned_end_exclusive);
  }
}

void gpu::CommandProcessor::HandleRingBufferWriteWatch(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (!ring_buffer_initialized_.load() || primary_buffer_ptr_ == 0 ||
      primary_buffer_size_ == 0 || !reader_.buffer()) {
    return;
  }
  uint32_t rb_start = primary_buffer_ptr_;
  uint32_t rb_end = rb_start + primary_buffer_size_;
  uint32_t write_end = physical_address_start + length;
  if (write_end <= rb_start || physical_address_start >= rb_end) {
    return;
  }

  uint64_t now_ms = NowMs();
  ring_buffer_last_write_ms_.store(now_ms, std::memory_order_relaxed);
  ring_buffer_last_write_phys_.store(physical_address_start,
                                     std::memory_order_relaxed);

    uint32_t restore_start =
      ring_watch_last_range_start_.load(std::memory_order_relaxed);
    uint32_t restore_size =
      ring_watch_last_range_size_.load(std::memory_order_relaxed);
    uint32_t restore_end =
      restore_start + (restore_size ? restore_size : primary_buffer_size_);
    XELOGI(
      "RING BUFFER: watch-hit restore-rw pending range=[{:08X}..{:08X}) "
      "size={:08X} hit_phys={:08X} hit_len={:X} exact={}",
      restore_start, restore_end, restore_size, physical_address_start, length,
      exact_range ? "YES" : "NO");

  LogRingBufferProtectionState("watch-hit-callback", physical_address_start,
                               length);
    ring_watch_restore_verify_phys_.store(physical_address_start,
                      std::memory_order_relaxed);
    ring_watch_restore_verify_len_.store(length, std::memory_order_relaxed);
    ring_watch_restore_verify_pending_.store(true, std::memory_order_relaxed);
  ring_buffer_watch_rearm_requested_.store(true, std::memory_order_relaxed);
  bool signaled_worker = write_ptr_index_event_ != nullptr;
  if (signaled_worker) {
    // Always wake the worker on watched writes so packet processing can run
    // before watch re-arming.
    write_ptr_index_event_->SetBoostPriority();
  }
  if (cvars::gpu_debug_watch_ringbuffer_rearm_on_hit && signaled_worker) {
    XELOGI(
        "RING BUFFER: watch-hit signaled worker for pending rearm "
        "(hit_phys={:08X} hit_len={:X})",
        physical_address_start, length);
  }
  uint64_t hit = ring_buffer_watch_hits_.fetch_add(1) + 1;
  if (!ShouldLogEvery(hit, cvars::gpu_debug_watch_ringbuffer_log_every_n)) {
    return;
  }

  auto* thread = kernel::XThread::GetCurrentThread();
  uint32_t read_ptr = read_ptr_index_;
  uint32_t write_ptr = write_ptr_index_.load();
  uint32_t ring_dwords = primary_buffer_size_ / sizeof(uint32_t);

  XELOGW(
      "DEBUG: RB WRITE WATCH hit#{} phys=0x{:08X} len=0x{:X} exact={} "
      "rb=[0x{:08X}..0x{:08X}) read={:08X} write={:08X} thread={:08X} "
      "tid={} main={} now_ms={} rearm_on_hit={} signaled_worker={}",
      hit, physical_address_start, length, exact_range ? "YES" : "NO", rb_start,
      rb_end, read_ptr, write_ptr, thread ? thread->handle() : 0,
      thread ? thread->thread_id() : 0,
      thread && thread->main_thread() ? "YES" : "NO", now_ms,
      cvars::gpu_debug_watch_ringbuffer_rearm_on_hit ? "YES" : "NO",
      signaled_worker ? "YES" : "NO");

  DumpRingBufferSummary("rb watch", reader_.buffer(), ring_dwords, read_ptr,
                        write_ptr, primary_buffer_ptr_, primary_buffer_size_,
                        read_ptr_writeback_ptr_);
  uint32_t window = std::min<uint32_t>(
      cvars::gpu_debug_watch_ringbuffer_dump_dwords,
      ring_dwords ? ring_dwords
                  : cvars::gpu_debug_watch_ringbuffer_dump_dwords);
  if (window > 0) {
    uint32_t start = read_ptr > (window / 2) ? (read_ptr - window / 2) : 0;
    DumpRingBufferWindow("rb watch window (around read_ptr)", reader_.buffer(),
                         ring_dwords, start, window);
    start = write_ptr > (window / 2) ? (write_ptr - window / 2) : 0;
    DumpRingBufferWindow("rb watch window (around write_ptr)", reader_.buffer(),
                         ring_dwords, start, window);
  }
}

void gpu::CommandProcessor::EnableRingBufferWriteWatch(const char* reason) {
  if (!cvars::gpu_debug_watch_ringbuffer_writes || !kernel_state_) {
    return;
  }
  if (!ring_buffer_initialized_.load() || primary_buffer_ptr_ == 0 ||
      primary_buffer_size_ == 0) {
    return;
  }
  auto* memory = kernel_state_->memory();
  if (!memory) {
    return;
  }
  if (ring_watch_force_rw_mode_.load(std::memory_order_relaxed)) {
    if (cvars::gpu_debug_watch_ringbuffer_log_protection) {
      XELOGW(
          "RING BUFFER: watch-arm skipped (force_rw_mode=YES) reason={} "
          "rb_base={:08X} rb_size={:08X}",
          reason ? reason : "unknown", primary_buffer_ptr_,
          primary_buffer_size_);
      LogRingBufferProtectionState("watch-arm-skipped-force-rw",
                                   primary_buffer_ptr_, primary_buffer_size_);
    }
    return;
  }
  if (!ring_buffer_watch_handle_) {
    ring_buffer_watch_handle_ =
        memory->RegisterPhysicalMemoryInvalidationCallback(
            &RingBufferWriteWatchCallback, this);
  }
  uint32_t page_size = 0;
  if (auto* heap = memory->LookupHeap(primary_buffer_ptr_)) {
    page_size = heap->page_size();
  }
  uint64_t arm_id =
      ring_buffer_watch_arm_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t now_ms = NowMs();
  ring_buffer_watch_last_arm_ms_.store(now_ms, std::memory_order_relaxed);
  XELOGI(
      "RING BUFFER: watch-arm request reason={} rb_base={:08X} rb_size={:08X} "
      "page_size={} callback_handle={:p} arm_id={} now_ms={}",
      reason ? reason : "unknown", primary_buffer_ptr_, primary_buffer_size_,
      page_size, ring_buffer_watch_handle_, arm_id, now_ms);
  LogRingBufferProtectionState("watch-arm-before", primary_buffer_ptr_,
                               primary_buffer_size_);
  memory->EnablePhysicalMemoryAccessCallbacks(
      primary_buffer_ptr_, primary_buffer_size_, true, false);
  LogRingBufferProtectionState("watch-arm-after", primary_buffer_ptr_,
                               primary_buffer_size_);

  if (cvars::gpu_debug_watch_ringbuffer_force_rw_fallback &&
      !ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed)) {
    auto* heap = memory->LookupHeap(primary_buffer_ptr_);
    if (heap) {
      uint32_t fallback_protect =
          kMemoryProtectRead | kMemoryProtectWrite | kMemoryProtectWriteCombine;
      bool protect_ok = heap->Protect(primary_buffer_ptr_, primary_buffer_size_,
                                      fallback_protect, nullptr);
      ring_watch_force_rw_mode_.store(true, std::memory_order_relaxed);
      XELOGW(
          "RING BUFFER: watch-arm RW fallback applied protect_ok={} "
          "rb=[{:08X}..{:08X}) size={:08X} protect={:08X}",
          protect_ok ? "YES" : "NO", primary_buffer_ptr_,
          primary_buffer_ptr_ + primary_buffer_size_, primary_buffer_size_,
          fallback_protect);
      LogRingBufferProtectionState("watch-arm-rw-fallback", primary_buffer_ptr_,
                                   primary_buffer_size_);
    }
  }
  XELOGI(
      "DEBUG: Ring buffer write watch armed (reason={}, rb_base={:08X} "
      "size={:08X}, arm_id={})",
      reason ? reason : "unknown", primary_buffer_ptr_, primary_buffer_size_,
      arm_id);
}

void gpu::CommandProcessor::EnableReadPointerWriteBack(
    uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates
  // of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
  void* host_ptr = nullptr;
  if (kernel_state_ && read_ptr_writeback_ptr_) {
    host_ptr = kernel_state_->memory()->TranslatePhysical(read_ptr_writeback_ptr_);
  }
  XELOGI(
      "RING BUFFER: RPTR writeback configured ptr={:08X} block_size_log2={} "
      "update_freq={} host_ptr={:p} mapped={}",
      read_ptr_writeback_ptr_, block_size_log2, read_ptr_update_freq_, host_ptr,
      host_ptr ? "YES" : "NO");
}

XE_NOINLINE XE_COLD void gpu::CommandProcessor::LogKickoffInitator(
    uint32_t value) {
  cpu::backend::GuestPseudoStackTrace st;

  if (logging::ShouldLog(LogLevel::Debug) &&
      kernel_state_->processor()->backend()->PopulatePseudoStacktrace(&st)) {
    logging::LoggerBatch<LogLevel::Debug> log_initiator{};

    log_initiator("Updating read ptr to {}, initiator stacktrace below\n",
                  value);

    for (uint32_t i = 0; i < st.count; ++i) {
      log_initiator("\t{:08X}\n", st.return_addrs[i]);
    }

    if (st.truncated_flag) {
      log_initiator("\t(Truncated stacktrace to {} entries)\n",
                    cpu::backend::MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES);
    }
    log_initiator.submit('d');
  }
}

void gpu::CommandProcessor::UpdateWritePointer(uint32_t value,
                                               const char* source) {
  static std::atomic<bool> logged_first_update{false};
  const char* source_name =
      (source && source[0] != '\0') ? source : "unspecified";
  if (!logged_first_update.exchange(true)) {
    XELOGI("DEBUG: First UpdateWritePointer call: value={:08X} source={}", value,
           source_name);
  }

  // macOS debugging: Critical for GPU Commands thread (01000010) hang
  // investigation
  uint32_t prev_value = write_ptr_index_.load();
  if (value != prev_value) {
    XELOGI("DEBUG: UpdateWritePointer: {:08X} -> {:08X} source={}", prev_value,
           value, source_name);
  } else {
    // Log even if same value, as it might be a re-trigger
    static uint32_t same_value_count = 0;
    if (same_value_count++ % 100 == 0) {
      XELOGI(
          "DEBUG: UpdateWritePointer re-triggered with same value {:08X} "
          "source={}",
          value, source_name);
    }
  }

  XE_UNLIKELY_IF(cvars::log_ringbuffer_kickoff_initiator_bts) {
    LogKickoffInitator(value);
  }
  write_ptr_index_ = value;
  if (ring_buffer_initialized_.load() &&
      !initialization_acknowledged_.load(std::memory_order_relaxed)) {
    initialization_acknowledged_.store(true, std::memory_order_relaxed);
    if (initialization_complete_event_ &&
        initialization_complete_event_->Query().state) {
      initialization_complete_event_->Reset();
      XELOGI(
          "DEBUG: UpdateWritePointer: cleared init-complete event after early ack");
    }
    XELOGI(
        "DEBUG: UpdateWritePointer: marking initialization acknowledged on first "
        "WPTR activity");
  }
  if (cvars::gpu_debug_rb_dump_on_activity && reader_.buffer()) {
    uint32_t ring_dwords =
        primary_buffer_size_ ? (primary_buffer_size_ / sizeof(uint32_t)) : 0;
    DumpRingBufferSummary("UpdateWritePointer", reader_.buffer(), ring_dwords,
                          read_ptr_index_, write_ptr_index_.load(),
                          primary_buffer_ptr_, primary_buffer_size_,
                          read_ptr_writeback_ptr_);
  }
  bool signaled_worker = write_ptr_index_event_ != nullptr;
  if (signaled_worker) {
    write_ptr_index_event_->SetBoostPriority();
  }
  if (value != prev_value || cvars::gpu_debug_wait_results) {
    XELOGI(
        "RING BUFFER: WPTR advanced old={:08X} new={:08X} source={} "
        "signaled_worker={} ring_init={} read_ptr={:08X}",
        prev_value, value, source_name, signaled_worker ? "YES" : "NO",
        ring_buffer_initialized_.load(std::memory_order_relaxed) ? "YES" : "NO",
        read_ptr_index_);
  }
  if (value != prev_value && graphics_system_ &&
      graphics_system_->GetInterruptCallback() != 0) {
    graphics_system_->DispatchInterruptCallback(0, 2);
  }
}

void gpu::CommandProcessor::NotifySwapPacket(uint32_t frontbuffer_ptr,
                                             uint32_t width, uint32_t height) {
  static std::atomic<uint64_t> notify_count{0};
  uint64_t notify_id = notify_count.fetch_add(1) + 1;
  uint64_t now_ms = NowMs();
  swap_packet_count_.fetch_add(1, std::memory_order_relaxed);
  last_swap_packet_ms_.store(now_ms, std::memory_order_relaxed);
  last_swap_frontbuffer_ptr_.store(frontbuffer_ptr, std::memory_order_relaxed);
  last_swap_width_.store(width, std::memory_order_relaxed);
  last_swap_height_.store(height, std::memory_order_relaxed);
  if (notify_id <= 5 || (notify_id % 120) == 0) {
    XELOGI(
        "DEBUG: NotifySwapPacket #{} frontbuffer=0x{:08X} size={}x{} "
        "ring_init={} init_ack={} read_ptr={:08X} write_ptr={:08X}",
        notify_id, frontbuffer_ptr, width, height,
        ring_buffer_initialized_.load(std::memory_order_relaxed) ? "YES"
                                                                  : "NO",
        initialization_acknowledged_.load(std::memory_order_relaxed) ? "YES"
                                                                      : "NO",
        read_ptr_index_, write_ptr_index_.load(std::memory_order_relaxed));
  }
}

void gpu::CommandProcessor::RecordDebugHostSwapInjection(
    uint32_t frontbuffer_ptr, uint32_t width, uint32_t height,
    bool ring_injected) {
  uint64_t now_ms = NowMs();
  debug_host_swap_injection_count_.fetch_add(1, std::memory_order_relaxed);
  if (ring_injected) {
    debug_host_swap_ring_injection_count_.fetch_add(1,
                                                    std::memory_order_relaxed);
  } else {
    debug_host_swap_direct_issue_count_.fetch_add(1,
                                                  std::memory_order_relaxed);
  }
  debug_host_swap_last_ms_.store(now_ms, std::memory_order_relaxed);
  debug_host_swap_last_frontbuffer_ptr_.store(frontbuffer_ptr,
                                              std::memory_order_relaxed);
  debug_host_swap_last_width_.store(width, std::memory_order_relaxed);
  debug_host_swap_last_height_.store(height, std::memory_order_relaxed);
}

void gpu::CommandProcessor::LogRegisterSet(uint32_t register_index,
                                           uint32_t value) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    const RegisterInfo* reginfo = RegisterFile::GetRegisterInfo(register_index);

    if (!reginfo) {
      XELOGD("Unknown_Reg{:04X} <- {:08X}\n", register_index, value);
    } else {
      XELOGD("{} <- {:08X}\n", reginfo->name, value);
    }
  }
#endif
}

void gpu::CommandProcessor::LogRegisterSets(uint32_t base_register_index,
                                            const uint32_t* values,
                                            uint32_t n_values) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    auto target = logging::internal::GetThreadBuffer();

    auto target_ptr = target.first;

    size_t total_size = 0;

    size_t rem_size = target.second;

    for (uint32_t i = 0; i < n_values; ++i) {
      uint32_t register_index = base_register_index + i;

      uint32_t value = xe::load_and_swap<uint32_t>(&values[i]);

      const RegisterInfo* reginfo =
          RegisterFile::GetRegisterInfo(register_index);

      if (!reginfo) {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size,
                                       "Unknown_Reg{:04X} <- {:08X}\n",
                                       register_index, value);
        target_ptr = tmpres.out;
        rem_size -= tmpres.size;
        total_size += tmpres.size;

      } else {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size, "{} <- {:08X}\n",
                                       reginfo->name, value);
        rem_size -= tmpres.size;
        target_ptr = tmpres.out;
        total_size += tmpres.size;
      }
    }
    logging::internal::AppendLogLine(LogLevel::Debug, 'd', total_size);
  }
#endif
}

void gpu::CommandProcessor::HandleSpecialRegisterWrite(uint32_t index,
                                                       uint32_t value) {
  RegisterFile& regs = *register_file_;
  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_MEM_REGs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        regs.values[index] |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color =
              regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl]
                                 [gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the
          // last PWL entry. However, the DC_LUT_RW_INDEX documentation says
          // that for PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index =
              (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
              ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask =
            regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If
        // this increase is assumed to behave like a full DC_LUT_RW_INDEX
        // write, it probably should. Currently this also calls WriteRegister
        // for DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(
            XE_GPU_REG_DC_LUT_RW_INDEX,
            xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}
void gpu::CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // chrispy: rearrange check order, place set after checks

  if (XE_LIKELY(index < RegisterFile::kRegisterCount)) {
    if (index == 0x01C5 || index == 0x01C0 || index == 0x01C1 ||
        index == 0x000F || index == 0x0010) {
      XELOGI("DEBUG: BREADCRUMB: WriteRegister(index={:04X}, value={:08X})", index,
             value);
    }
    register_file_->values[index] = value;

    // Keep CP_RB_* side effects consistent even when writes come from packet
    // execution rather than MMIO.
    if (index == 0x01C5) {  // CP_RB_WPTR
      uint32_t prev_wptr = write_ptr_index_.load(std::memory_order_relaxed);
      if (value != prev_wptr || cvars::trace_gpu_bootstrap) {
        XELOGI(
            "RING BUFFER: CP packet wrote CP_RB_WPTR={:08X} (prev={:08X})",
            value, prev_wptr);
      }
      UpdateWritePointer(value, "CP_PACKET_CP_RB_WPTR");
      if (graphics_system_) {
        graphics_system_->RecordWritePointerUpdate(value, "CP packet CP_RB_WPTR");
      }
    } else if (index == 0x01C3) {  // CP_RB_RPTR_ADDR
      uint32_t rb_cntl = register_file_->values[0x01C1];
      uint32_t block_size_log2 = (rb_cntl >> 8) & 0x3F;
      EnableReadPointerWriteBack(value, block_size_log2);
      if (cvars::trace_gpu_bootstrap) {
        XELOGI(
            "RING BUFFER: CP packet wrote CP_RB_RPTR_ADDR={:08X} "
            "(block_size_log2={})",
            value, block_size_log2);
      }
    }

    // quick pre-test
    // todo: figure out just how unlikely this is. if very (it ought to be,
    // theres a ton of registers other than these) make this predicate
    // branchless and mark with unlikely, then make HandleSpecialRegisterWrite
    // noinline yep, its very unlikely. these ORS here are meant to be bitwise
    // ors, so that we do not do branching evaluation of the conditions (we
    // will almost always take all of the branches)

    unsigned expr = (index - XE_GPU_REG_SCRATCH_REG0 < 8) |
                    (index == XE_GPU_REG_COHER_STATUS_HOST) |
                    ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
                     (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));
    // chrispy: reordered for msvc branch probability (assumes if is taken and
    // else is not)
    if (XE_LIKELY(expr == 0)) {
      XE_MSVC_REORDER_BARRIER();

    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  } else {
    XELOGW("CommandProcessor::WriteRegister index out of bounds: {}", index);
    return;
  }
}
void gpu::CommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = xe::load_and_swap<uint32_t>(base + i);
    this->WriteRegister(start_index + i, data);
  }
}

void gpu::CommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                       uint32_t base,
                                                       uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = ring->ReadAndSwap<uint32_t>();
    WriteRegister(base + i, data);
  }
}

void gpu::CommandProcessor::WriteALURangeFromRing(xe::RingBuffer* ring,
                                                  uint32_t base,
                                                  uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_times);
}

void gpu::CommandProcessor::WriteFetchRangeFromRing(xe::RingBuffer* ring,
                                                    uint32_t base,
                                                    uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_times);
}

void gpu::CommandProcessor::WriteBoolRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_times);
}

void gpu::CommandProcessor::WriteLoopRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_times);
}

void gpu::CommandProcessor::WriteREGISTERSRangeFromRing(xe::RingBuffer* ring,
                                                        uint32_t base,
                                                        uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_times);
}

void gpu::CommandProcessor::WriteALURangeFromMem(uint32_t start_index,
                                                 uint32_t* base,
                                                 uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void gpu::CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index,
                                                   uint32_t* base,
                                                   uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void gpu::CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void gpu::CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void gpu::CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index,
                                                       uint32_t* base,
                                                       uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}
XE_NOINLINE
void gpu::CommandProcessor::WriteOneRegisterFromRing(uint32_t base,
                                                     uint32_t num_times) {
  for (uint32_t m = 0; m < num_times; m++) {
    uint32_t reg_data = reader_.ReadAndSwap<uint32_t>();
    uint32_t target_index = base;
    WriteRegister(target_index, reg_data);
  }
}
void gpu::CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = xe::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  XELOGGPU("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
           base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void gpu::CommandProcessor::PrepareForWait() { trace_writer_.Flush(); }

void gpu::CommandProcessor::ReturnFromWait() {}

gpu::CommandProcessor::RingBufferDebugState
gpu::CommandProcessor::GetRingBufferDebugState() const {
  RingBufferDebugState state;
  state.worker_running = worker_running_.load(std::memory_order_relaxed);
  state.worker_setup_complete =
      worker_setup_complete_.load(std::memory_order_relaxed);
  state.worker_setup_failed =
      worker_setup_failed_.load(std::memory_order_relaxed);
  state.ring_buffer_initialized =
      ring_buffer_initialized_.load(std::memory_order_relaxed);
  state.initialization_acknowledged =
      initialization_acknowledged_.load(std::memory_order_relaxed);
  state.has_write_ptr_event = (write_ptr_index_event_ != nullptr);
  state.has_init_complete_event = (initialization_complete_event_ != nullptr);
    state.init_request_count =
      init_request_count_.load(std::memory_order_relaxed);
    state.init_ack_transition_count =
      init_ack_transition_count_.load(std::memory_order_relaxed);
    state.last_init_request_ms =
      last_init_request_ms_.load(std::memory_order_relaxed);
    state.last_init_signal_ms =
      last_init_signal_ms_.load(std::memory_order_relaxed);
    state.last_init_ack_ms =
      last_init_ack_ms_.load(std::memory_order_relaxed);
  state.primary_buffer_ptr = primary_buffer_ptr_;
  state.primary_buffer_size = primary_buffer_size_;
  state.read_ptr_index = read_ptr_index_;
  state.write_ptr_index = write_ptr_index_.load(std::memory_order_relaxed);
  state.read_ptr_writeback_ptr = read_ptr_writeback_ptr_;
  state.read_ptr_update_freq = read_ptr_update_freq_;
  state.swap_packet_count = swap_packet_count_.load(std::memory_order_relaxed);
  state.last_swap_packet_ms =
      last_swap_packet_ms_.load(std::memory_order_relaxed);
  state.last_swap_frontbuffer_ptr =
      last_swap_frontbuffer_ptr_.load(std::memory_order_relaxed);
  state.last_swap_width = last_swap_width_.load(std::memory_order_relaxed);
  state.last_swap_height = last_swap_height_.load(std::memory_order_relaxed);
  state.debug_host_swap_injection_count =
      debug_host_swap_injection_count_.load(std::memory_order_relaxed);
  state.debug_host_swap_ring_injection_count =
      debug_host_swap_ring_injection_count_.load(std::memory_order_relaxed);
  state.debug_host_swap_direct_issue_count =
      debug_host_swap_direct_issue_count_.load(std::memory_order_relaxed);
  state.debug_host_swap_last_ms =
      debug_host_swap_last_ms_.load(std::memory_order_relaxed);
  state.debug_host_swap_last_frontbuffer_ptr =
      debug_host_swap_last_frontbuffer_ptr_.load(std::memory_order_relaxed);
  state.debug_host_swap_last_width =
      debug_host_swap_last_width_.load(std::memory_order_relaxed);
  state.debug_host_swap_last_height =
      debug_host_swap_last_height_.load(std::memory_order_relaxed);
  state.ring_buffer_watch_hits =
      ring_buffer_watch_hits_.load(std::memory_order_relaxed);
  state.ring_buffer_watch_arm_count =
      ring_buffer_watch_arm_count_.load(std::memory_order_relaxed);
  state.ring_buffer_watch_last_arm_ms =
      ring_buffer_watch_last_arm_ms_.load(std::memory_order_relaxed);
  state.ring_buffer_last_write_ms =
      ring_buffer_last_write_ms_.load(std::memory_order_relaxed);
  state.ring_buffer_last_write_phys =
      ring_buffer_last_write_phys_.load(std::memory_order_relaxed);
  state.ring_buffer_watch_rearm_requested =
      ring_buffer_watch_rearm_requested_.load(std::memory_order_relaxed);
    state.ring_watch_last_transition_ms =
      ring_watch_last_transition_ms_.load(std::memory_order_relaxed);
    state.ring_watch_last_phase_id =
      ring_watch_last_phase_id_.load(std::memory_order_relaxed);
    state.ring_watch_last_range_start =
      ring_watch_last_range_start_.load(std::memory_order_relaxed);
    state.ring_watch_last_range_size =
      ring_watch_last_range_size_.load(std::memory_order_relaxed);
    state.ring_watch_last_guest_write_enabled =
      ring_watch_last_guest_write_enabled_.load(std::memory_order_relaxed);
    state.ring_watch_restore_verify_pending =
      ring_watch_restore_verify_pending_.load(std::memory_order_relaxed);
    state.ring_watch_force_rw_mode =
        ring_watch_force_rw_mode_.load(std::memory_order_relaxed);
  return state;
}

void gpu::CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(
      0, reinterpret_cast<const uint32_t*>(register_file_->values),
      RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(),
                               gamma_ramp_pwl_rgb(), gamma_ramp_rw_component_);
}

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   kernel::KernelState* kernel_state)
    : reader_(nullptr, 0),
      trace_writer_(nullptr),
      graphics_system_(graphics_system),
      kernel_state_(kernel_state),
      memory_(kernel_state->memory()),
      register_file_(nullptr),
      write_ptr_index_(0),
      worker_running_(false),
      initialization_acknowledged_(false) {
  write_ptr_index_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  worker_setup_complete_event_ =
      xe::threading::Event::CreateManualResetEvent(false);
  // initialization_complete_event_ will be created on-demand in
  // InitializeRingBuffer
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  fflush(stderr);
  if (!memory_) {
    return false;
  }

  fflush(stderr);
  register_file_ = new RegisterFile();
  fflush(stderr);

  worker_setup_complete_.store(false, std::memory_order_relaxed);
  worker_setup_failed_.store(false, std::memory_order_relaxed);
  if (worker_setup_complete_event_) {
    worker_setup_complete_event_->Reset();
  }

  worker_running_ = true;

  auto host_thread_created = std::make_shared<std::atomic<bool>>(false);
  auto thread_started_execution = std::make_shared<std::atomic<bool>>(false);

  // Watchdog 1: Ensure XHostThread object creation completes
  std::thread([host_thread_created]() {
    constexpr int kTimeoutMs = 5000;
    constexpr int kPollMs = 100;
    for (int elapsed = 0; elapsed < kTimeoutMs; elapsed += kPollMs) {
      if (host_thread_created->load(std::memory_order_acquire)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
    fflush(stderr);
    std::abort();
  }).detach();

  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0, [this, thread_started_execution]() {
            thread_started_execution->store(true, std::memory_order_release);
            WorkerThreadMain();
            return 0;
          }));
  host_thread_created->store(true, std::memory_order_release);

  worker_thread_->set_name("GPU Command Processor");

  // Validate that the GPU Command Processor is a HOST thread
  if (worker_thread_->is_guest_thread()) {
    XELOGE("DEBUG: FATAL: GPU Command Processor misclassified as GUEST thread!");
    XELOGE("   This violates Xenia's threading architecture.");
    XELOGE("   GPU infrastructure MUST be HOST threads.");
    fflush(stderr);
    std::abort();
  }
  if (!worker_thread_->is_host_object()) {
    XELOGE("DEBUG: FATAL: GPU Command Processor is not a host object!");
    XELOGE("   This violates Xenia's threading architecture.");
    fflush(stderr);
    std::abort();
  }
  XELOGI(
      "DEBUG: GPU Command Processor thread role validation PASSED (HOST thread)");

  worker_thread_->Create();

  // Watchdog 2: Ensure thread starts execution within timeout
  std::thread([thread_started_execution, this]() {
    constexpr int kTimeoutMs = 5000;
    constexpr int kPollMs = 100;
    for (int elapsed = 0; elapsed < kTimeoutMs; elapsed += kPollMs) {
      if (thread_started_execution->load(std::memory_order_acquire)) {
        XELOGI("DEBUG: GPU Command Processor thread started execution after {} ms",
               elapsed);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
    }
    XELOGE(
        "DEBUG: FATAL: GPU Command Processor thread failed to start execution "
        "after {} ms",
        kTimeoutMs);
    XELOGE("   Thread handle: {:08X}",
           worker_thread_ ? worker_thread_->handle() : 0);
    XELOGE("   This indicates a critical deadlock or scheduler starvation.");
    XELOGE("   Terminating to prevent permanent hang.");
    fflush(stderr);
    std::abort();
  }).detach();

  if (!worker_thread_) {
    return false;
  }

  if (worker_setup_complete_event_) {
    auto setup_wait =
        xe::threading::Wait(worker_setup_complete_event_.get(), false,
                            std::chrono::milliseconds(5000));
    if (setup_wait != xe::threading::WaitResult::kSuccess) {
      XELOGE("RING BUFFER: worker setup handshake timeout (result={})",
             static_cast<int>(setup_wait));
      worker_running_ = false;
      write_ptr_index_event_->Set();
      if (worker_thread_) {
        xe::threading::Wait(worker_thread_->thread(), false,
                            std::chrono::milliseconds(100));
      }
      return false;
    }
  }

  if (worker_setup_failed_.load(std::memory_order_relaxed) ||
      !worker_setup_complete_.load(std::memory_order_relaxed)) {
    XELOGE(
        "RING BUFFER: worker setup handshake failed "
        "(setup_complete={} setup_failed={})",
        worker_setup_complete_.load(std::memory_order_relaxed) ? "YES" :
                                                                 "NO",
        worker_setup_failed_.load(std::memory_order_relaxed) ? "YES" : "NO");
    worker_running_ = false;
    write_ptr_index_event_->Set();
    if (worker_thread_) {
      xe::threading::Wait(worker_thread_->thread(), false,
                          std::chrono::milliseconds(100));
    }
    return false;
  }

  XELOGI("RING BUFFER: worker startup handshake complete");

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  write_ptr_index_event_->Set();
  if (worker_setup_complete_event_) {
    worker_setup_complete_event_->Set();
  }

  // Signal and reset initialization complete event
  if (initialization_complete_event_) {
    initialization_complete_event_->Set();
  }

  if (worker_thread_) {
    xe::threading::Wait(worker_thread_->thread(), false);
    worker_thread_.reset();
  }
  if (ring_buffer_watch_handle_ && kernel_state_ && kernel_state_->memory()) {
    kernel_state_->memory()->UnregisterPhysicalMemoryInvalidationCallback(
        ring_buffer_watch_handle_);
    ring_buffer_watch_handle_ = nullptr;
  }
  ring_buffer_watch_rearm_requested_.store(false, std::memory_order_relaxed);
  ring_buffer_watch_arm_count_.store(0, std::memory_order_relaxed);
  ring_buffer_watch_last_arm_ms_.store(0, std::memory_order_relaxed);

  delete register_file_;
  register_file_ = nullptr;

  // Clean up initialization event
  initialization_complete_event_.reset();
}

void CommandProcessor::CallInThread(std::function<void()> fn,
                                    const char* tag) {
  std::lock_guard<std::mutex> lock(pending_fns_mutex_);
  PendingFnEntry entry;
  entry.fn = std::move(fn);
  entry.tag = (tag && tag[0]) ? tag : "<anonymous>";
  entry.enqueue_ms = NowMs();
  pending_fns_.push(std::move(entry));
  write_ptr_index_event_->Set();
}

void CommandProcessor::ExecutePendingFunctions() {
  std::unique_lock<std::mutex> lock(pending_fns_mutex_);
  if (pending_fns_.empty()) {
    return;
  }
  size_t count = pending_fns_.size();
  uint64_t now_ms = NowMs();
  const auto& head = pending_fns_.front();
  uint64_t head_age_ms =
      (head.enqueue_ms && now_ms >= head.enqueue_ms) ? (now_ms - head.enqueue_ms)
                                                     : 0;
  XELOGI(
      "DEBUG: COMMAND_PROCESSOR: Executing {} pending functions "
      "(head_tag={} head_age_ms={})...",
      count, head.tag.empty() ? "<anonymous>" : head.tag, head_age_ms);
  while (!pending_fns_.empty()) {
    auto pending = std::move(pending_fns_.front());
    pending_fns_.pop();
    size_t remaining = pending_fns_.size();
    std::string pending_tag =
        pending.tag.empty() ? "<anonymous>" : pending.tag;
    uint64_t pending_age_ms =
        (pending.enqueue_ms && now_ms >= pending.enqueue_ms)
            ? (now_ms - pending.enqueue_ms)
            : 0;
    auto fn = std::move(pending.fn);
    lock.unlock();
    XELOGI(
        "DEBUG: COMMAND_PROCESSOR: running pending function tag={} age_ms={} "
        "remaining_after_pop={}",
        pending_tag, pending_age_ms, remaining);
    fn();
    now_ms = NowMs();
    lock.lock();
  }
  XELOGI("DEBUG: COMMAND_PROCESSOR: Executed {} pending functions successfully.",
         count);
}

void CommandProcessor::ClearCaches() {
  // Default implementation - subclasses should override
}

void CommandProcessor::RequestFrameTrace(
    const std::filesystem::path& root_path) {
  // Default implementation - subclasses should override
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  // Default implementation - subclasses should override
}

void CommandProcessor::EndTracing() {
  // Default implementation - subclasses should override
}

void CommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking) {
  // Default implementation - subclasses should override
}

void CommandProcessor::SetDesiredSwapPostEffect(
    SwapPostEffect swap_post_effect) {
  swap_post_effect_desired_ = swap_post_effect;
}

void CommandProcessor::RestoreRegisters(uint32_t first_register,
                                        const uint32_t* register_values,
                                        uint32_t register_count,
                                        bool execute_callbacks) {
  for (uint32_t i = 0; i < register_count; ++i) {
    WriteRegister(first_register + i, register_values[i]);
  }
}

void CommandProcessor::RestoreGammaRamp(
    const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
    const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
    uint32_t new_gamma_ramp_rw_component) {
  if (new_gamma_ramp_256_entry_table) {
    std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
                sizeof(gamma_ramp_256_entry_table_));
  }
  if (new_gamma_ramp_pwl_rgb) {
    std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb,
                sizeof(gamma_ramp_pwl_rgb_));
  }
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
}

#define COMMAND_PROCESSOR gpu::CommandProcessor
#include "pm4_command_processor_implement_mac.h"
}  // namespace gpu
}  // namespace xe
