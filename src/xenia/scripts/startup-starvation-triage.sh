#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [[ $# -lt 1 || -z "${1:-}" ]]; then
  echo "error: log file path is required." >&2
  echo "usage: $0 <path-to-log-file>" >&2
  exit 1
fi

LOG_FILE="$1"

if [[ ! -f "$LOG_FILE" ]]; then
  echo "error: log file not found: $LOG_FILE" >&2
  echo "usage: $0 <path-to-log-file>" >&2
  exit 1
fi

if ! command -v rg >/dev/null 2>&1; then
  echo "error: rg (ripgrep) is required for this script." >&2
  exit 1
fi

count_matches() {
  local pattern="$1"
  local out
  out="$(rg -c -- "$pattern" "$LOG_FILE" 2>/dev/null || true)"
  if [[ -z "$out" ]]; then
    echo "0"
  else
    echo "$out"
  fi
}

last_match() {
  local pattern="$1"
  rg -n -- "$pattern" "$LOG_FILE" 2>/dev/null | tail -n 1 || true
}

gpu_startup_watch_count="$(count_matches "(GPU STARTUP WATCH:|gpu_startup_watch:)")"
gpu_startup_starvation_count="$(count_matches "(GPU STARTUP STARVATION:|gpu_startup_starvation:)")"
gpu_callback_missing_count="$(count_matches "GPU interrupt callback not set")"
worker_wait_timeout_count="$(count_matches "GPU WORKER WAIT #[0-9]+ result=timeout")"
worker_loop_init_no_count="$(count_matches "GPU WORKER LOOP #[0-9]+: .*init=NO .*rb_base=00000000 rb_size=00000000")"
main_loop_pc_82096008_count="$(count_matches "MainThread PC sample .*pc=82096008")"
dispatch_bridge_ready_count="$(count_matches "dispatch path gate READY .*guest_main_dispatch_bridge_total=[1-9]")"
dispatch_bridge_before_loader_count="$(count_matches "guest_dispatch_bridge_before_loader|guest_bootstrap_entry_loop_before_loader|guest_busy_loop_before_loader")"
bootstrap_entry_loop_count="$(count_matches "gpu_bootstrap_entry_loop_before_callback")"
title_privilege_gate_count="$(count_matches "title_local_privilege_gate_before_gpu_dispatch")"
privilege_branch_scan_count="$(count_matches "main-thread dispatch stall (target summary|classification).*xex_privilege_check=YES|xex_check_branch_scan_seen=YES")"
privilege_execution_probe_count="$(count_matches "title pre-gpu privilege execution probe")"
privilege_call_count="$(count_matches "RING BUFFER: XexCheckExecutablePrivilege call|xex_check_privilege_calls=[1-9]")"
privilege_branch_only_count="$(count_matches "xex_priv_branch_only=YES|xex_check_branch_only=YES")"
xexcheck_static_thunk_count="$(count_matches "title pre-gpu runtime name=XexCheckExecutablePrivilege .*static_thunk_calls=[1-9]|xex_check_privilege_static_thunk_calls=[1-9]")"
xexcheck_export_entry_count="$(count_matches "title pre-gpu runtime name=XexCheckExecutablePrivilege .*export_entry_calls=[1-9]|xex_check_privilege_export_entry_calls=[1-9]")"
xexcheck_import_probe_valid_count="$(count_matches "RING BUFFER: title pre-gpu import name=XexCheckExecutablePrivilege .*probe_valid=YES|static_xexcheck_import=YES .*xexcheck_import_probe_valid=YES")"
xexcheck_import_probe_invalid_count="$(count_matches "RING BUFFER: title pre-gpu import name=XexCheckExecutablePrivilege .*probe_valid=NO|static_xexcheck_import=NO|xexcheck_import_probe_valid=NO")"
foundation_summary_count="$(count_matches "RING BUFFER: critical ordinal foundation summary")"
foundation_xexcheck_entry_count="$(count_matches "next_missing_transition=bridge_to_xexcheck_entry")"
foundation_xexcheck_thunk_to_entry_count="$(count_matches "next_missing_transition=xexcheck_thunk_to_export_entry")"
foundation_xexcheck_to_loader_count="$(count_matches "next_missing_transition=xexcheck_to_loader_or_static_dispatch")"
foundation_bridge_to_loader_count="$(count_matches "next_missing_transition=bridge_to_loader_or_static_dispatch")"
foundation_runtime_dispatch_count="$(count_matches "next_missing_transition=ordinal_runtime_dispatch")"
foundation_runtime_entry_count="$(count_matches "next_missing_transition=runtime_entry_after_lookup|next_missing_transition=export_entry_after_thunk|next_missing_transition=guest_side_effect_after_export")"

no_swap_vd_snapshot_count="$(count_matches "NO SWAP: Vd\\* call state snapshot")"
vd_init_never_count="$(count_matches "vd VdInitializeRingBuffer: <never>")"
vd_setcb_never_count="$(count_matches "vd VdSetGraphicsInterruptCallback: <never>")"
vd_swap_never_count="$(count_matches "vd VdSwap: <never>")"
vd_syscb_never_count="$(count_matches "vd VdGetSystemCommandBuffer: <never>")"

forced_init_flag_count="$(count_matches "gpu_force_init_if_missing = true")"
forced_cb_flag_count="$(count_matches "gpu_debug_force_interrupt_callback_if_missing = true")"
gate_watch_flag_count="$(count_matches "gpu_debug_gate_watch = true")"

launch_line="$(last_match "ABOUT TO CALL RunTitle! Path:")"
launch_path="$(echo "$launch_line" | sed -E 's/.*Path: //')"

classification="UNDETERMINED"
risk="MEDIUM"

if (( title_privilege_gate_count > 0 &&
      dispatch_bridge_ready_count > 0 &&
      vd_setcb_never_count > 0 &&
      gpu_callback_missing_count >= 5 )); then
  classification="DISPATCH_BRIDGE_STALLED_AT_TITLE_PRIVILEGE_GATE"
  risk="HIGH"
elif (( bootstrap_entry_loop_count > 0 &&
        vd_setcb_never_count > 0 &&
        gpu_callback_missing_count >= 5 )); then
  classification="DISPATCH_BRIDGE_STALLED_BEFORE_CALLBACK"
  risk="HIGH"
elif (( dispatch_bridge_ready_count > 0 &&
        dispatch_bridge_before_loader_count > 0 &&
        vd_init_never_count > 0 &&
        vd_setcb_never_count > 0 &&
        vd_swap_never_count > 0 &&
        gpu_callback_missing_count >= 5 )); then
  classification="DISPATCH_BRIDGE_WITHOUT_GPU_BOOTSTRAP"
  risk="HIGH"
elif (( vd_init_never_count > 0 &&
        vd_setcb_never_count > 0 &&
        vd_swap_never_count > 0 &&
        worker_wait_timeout_count >= 10 &&
        gpu_callback_missing_count >= 5 &&
        dispatch_bridge_ready_count == 0 &&
        dispatch_bridge_before_loader_count == 0 &&
        bootstrap_entry_loop_count == 0 )); then
  classification="BOOTSTRAP_NEVER_ENTERED"
  risk="CRITICAL"
elif (( vd_init_never_count == 0 &&
        vd_setcb_never_count > 0 &&
        gpu_callback_missing_count >= 5 )); then
  classification="RING_INIT_WITHOUT_INTERRUPT_CALLBACK"
  risk="HIGH"
elif (( vd_init_never_count == 0 &&
        vd_setcb_never_count == 0 &&
        vd_swap_never_count > 0 )); then
  classification="BOOTSTRAP_DONE_NO_SWAP_PROGRESS"
  risk="HIGH"
fi

echo ""
echo "Xenia Startup Starvation Triage"
echo ""
echo "Log file: $LOG_FILE"
echo "Launch path: ${launch_path:-<not found>}"
echo
echo "Classification: $classification"
echo "Severity: $risk"
echo
echo "Observed signals:"
echo "  gpu_startup_watch logs:                  $gpu_startup_watch_count"
echo "  gpu_startup_starvation logs:             $gpu_startup_starvation_count"
echo "  gpu callback missing warnings:           $gpu_callback_missing_count"
echo "  gpu worker wait timeouts:                $worker_wait_timeout_count"
echo "  gpu worker loops with init=NO+rb=0:      $worker_loop_init_no_count"
echo "  dispatch-path READY via bridge:          $dispatch_bridge_ready_count"
echo "  dispatch bridge before loader states:    $dispatch_bridge_before_loader_count"
echo "  bootstrap-entry-loop callback stalls:    $bootstrap_entry_loop_count"
echo "  title privilege-gate stalls:             $title_privilege_gate_count"
echo "  privilege branch-scan sightings:         $privilege_branch_scan_count"
echo "  privilege execution probes:              $privilege_execution_probe_count"
echo "  privilege export-entry/call sightings:   $privilege_call_count"
echo "  privilege branch-only states:            $privilege_branch_only_count"
echo "  xexcheck static thunk sightings:         $xexcheck_static_thunk_count"
echo "  xexcheck export-entry sightings:         $xexcheck_export_entry_count"
echo "  xexcheck import probes valid:            $xexcheck_import_probe_valid_count"
echo "  xexcheck import probes invalid/missing:  $xexcheck_import_probe_invalid_count"
echo "  ordinal foundation summaries:            $foundation_summary_count"
echo "  next=bridge->xexcheck entry:             $foundation_xexcheck_entry_count"
echo "  next=xexcheck thunk->export entry:       $foundation_xexcheck_thunk_to_entry_count"
echo "  next=xexcheck->loader/static dispatch:   $foundation_xexcheck_to_loader_count"
echo "  next=bridge->loader/static dispatch:     $foundation_bridge_to_loader_count"
echo "  next=ordinal runtime dispatch:           $foundation_runtime_dispatch_count"
echo "  next=runtime entry/effect:               $foundation_runtime_entry_count"
echo "  no-swap Vd snapshot dumps:               $no_swap_vd_snapshot_count"
echo "  VdInitializeRingBuffer '<never>':        $vd_init_never_count"
echo "  VdSetGraphicsInterruptCallback '<never>':$vd_setcb_never_count"
echo "  VdSwap '<never>':                        $vd_swap_never_count"
echo "  VdGetSystemCommandBuffer '<never>':      $vd_syscb_never_count"
echo "  main loop pc=82096008 samples:           $main_loop_pc_82096008_count"
echo
echo "Run flags seen in log:"
echo "  gpu_force_init_if_missing=true count:    $forced_init_flag_count"
echo "  gpu_debug_force_interrupt...=true count: $forced_cb_flag_count"
echo "  gpu_debug_gate_watch=true count:         $gate_watch_flag_count"
echo
echo "Latest key lines:"
echo "  $(last_match "(GPU STARTUP WATCH:|gpu_startup_watch:)")"
echo "  $(last_match "(GPU STARTUP STARVATION:|gpu_startup_starvation:)")"
echo "  $(last_match "GPU interrupt callback not set")"
echo "  $(last_match "GPU WORKER WAIT #[0-9]+ result=timeout")"
echo "  $(last_match "RING BUFFER: critical ordinal foundation summary")"
echo "  $(last_match "RING BUFFER: title pre-gpu import name=XexCheckExecutablePrivilege|RING BUFFER: title pre-gpu kernel health")"
echo "  $(last_match "RING BUFFER: title pre-gpu runtime name=XexCheckExecutablePrivilege")"
echo "  $(last_match "RING BUFFER: title pre-gpu privilege execution probe")"
echo "  $(last_match "main-thread dispatch stall classification state=title_local_privilege_gate_before_gpu_dispatch|RING BUFFER: XexCheckExecutablePrivilege call")"
echo
echo "Project anchors for debugging:"
echo "  $REPO_ROOT/src/xenia/gpu/graphics_system.cc:6354"
echo "  $REPO_ROOT/src/xenia/gpu/graphics_system.cc:6423"
echo "  $REPO_ROOT/src/xenia/gpu/graphics_system.cc:11268"
echo "  $REPO_ROOT/src/xenia/gpu/graphics_system.cc:11294"
echo "  $REPO_ROOT/src/xenia/emulator.cc:3139"
echo "  $REPO_ROOT/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc:386"
echo
if [[ "$classification" == "BOOTSTRAP_NEVER_ENTERED" ]]; then
  echo "Suggested next checks:"
  echo "  1. Verify the title ever reaches Vd* exports in xboxkrnl_video."
  echo "  2. Correlate main-loop gate behavior with the branch/call targets in your PC loop."
  echo "  3. Keep forced GPU init/callback flags OFF during root-cause runs."
elif [[ "$classification" == "DISPATCH_BRIDGE_STALLED_AT_TITLE_PRIVILEGE_GATE" ]]; then
  echo "Suggested next checks:"
  echo "  1. Focus on the title-local privilege gate before any loader or GPU path executes."
  if (( xexcheck_import_probe_invalid_count > 0 )); then
    echo "  2. Fix the XexCheckExecutablePrivilege static import/probe surface before chasing loader or GPU runtime follow-through."
  elif (( foundation_xexcheck_thunk_to_entry_count > 0 || (xexcheck_static_thunk_count > 0 && xexcheck_export_entry_count == 0) )); then
    echo "  2. The guest is reaching the XexCheckExecutablePrivilege static thunk, but export entry is still missing; focus on the thunk-to-export handoff."
  elif (( foundation_xexcheck_entry_count > 0 )); then
    echo "  2. The critical ordinals are still stuck before entering XexCheckExecutablePrivilege itself; confirm the stall resolves at that export entry before loader or thunk/export follow-through."
  elif (( foundation_xexcheck_to_loader_count > 0 )); then
    echo "  2. XexCheckExecutablePrivilege is being entered, but the run is still not clearing into loader or tracked GPU dispatch afterward."
  elif (( foundation_bridge_to_loader_count > 0 )); then
    echo "  2. The critical ordinals are still stuck at bridge->loader/static-dispatch; confirm the stall resolves before any loader or thunk/export entry."
  elif (( privilege_branch_scan_count > 0 && privilege_call_count == 0 )); then
    echo "  2. Correlate repeated XexCheckExecutablePrivilege branch scans with the stable PC/LR chain; the export itself has not been entered yet."
  else
    echo "  2. Correlate repeated XexCheckExecutablePrivilege scans with the stable PC/LR branch chain."
  fi
  echo "  3. Keep callback recovery and other forcing paths OFF while tightening privilege-gate diagnostics."
elif [[ "$classification" == "DISPATCH_BRIDGE_WITHOUT_GPU_BOOTSTRAP" ]]; then
  echo "Suggested next checks:"
  echo "  1. Correlate guest-main dispatch-bridge entry with the stable PC loop before callback registration."
  echo "  2. Verify the worker is no longer over-gated once the dispatch bridge is observed."
  echo "  3. Keep forced GPU init/callback flags OFF and focus on real loader/Vd/MMIO follow-through."
elif [[ "$classification" == "DISPATCH_BRIDGE_STALLED_BEFORE_CALLBACK" ]]; then
  echo "Suggested next checks:"
  echo "  1. Focus on the stable bootstrap-entry loop or call chain immediately before VdSetGraphicsInterruptCallback."
  echo "  2. Check whether the guest repeatedly branches through the same early bootstrap helper without reaching loader/Vd paths."
  echo "  3. Keep callback recovery and other forcing paths OFF while tightening dispatch-stall diagnostics."
elif [[ "$classification" == "RING_INIT_WITHOUT_INTERRUPT_CALLBACK" ]]; then
  echo "Suggested next checks:"
  echo "  1. Confirm callback registration path in VdSetGraphicsInterruptCallback."
  echo "  2. Track callback value transitions in GraphicsSystem::SetInterruptCallback."
  echo "  3. Validate callback thunk address is executable and stable."
elif [[ "$classification" == "BOOTSTRAP_DONE_NO_SWAP_PROGRESS" ]]; then
  echo "Suggested next checks:"
  echo "  1. Trace ring write pointer changes and CP packet decode progress."
  echo "  2. Check for swap packet suppression due to command stream content."
  echo "  3. Compare with a known-good title's early VdSwap sequence."
else
  echo "Suggested next checks:"
  echo "  1. Re-run with gpu_startup_watchdog and no-swap diagnostics enabled."
  echo "  2. Re-check whether this log path matches the title actually launched."
fi
