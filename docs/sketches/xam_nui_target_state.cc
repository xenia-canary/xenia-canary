/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// =============================================================================
// xam_nui.cc — Kinectix rewrite of upstream Xenia's NUI stub.
//
// IMPORTANT: this is a Stage-1 sketch.
//
// Upstream xenia-canary's xam_nui.cc currently contains stubs that return
// X_E_FAIL or 0 for every NUI export. The list of XAM NUI functions
// reference titles actually call is unknown until Stage 0 telemetry is run
// (see ROADMAP.md). The exports below are a best-guess based on:
//
//   * Microsoft Kinect for Windows SDK 1.8 NUI.h naming conventions
//   * Reverse-engineered xex import lists from a handful of Kinect titles
//   * Existing upstream xam_nui.cc declared exports (preserved verbatim)
//
// Function bodies marked TODO(stage-0) require empirical confirmation.
// Until Stage 0 logs are processed, these implementations route through
// NuiManager but assume guest struct layouts that may need adjustment.
//
// Once Stage 0 is complete, the contract here will be:
//   1. Every observed XAM NUI export listed in
//      docs/XAM_NUI_API_OBSERVED.md is implemented in this file.
//   2. Each implementation pulls data from NuiManager::Instance()->backend()
//      via INuiBackend::Poll*().
//   3. Each implementation translates host-side struct into the guest-side
//      layout expected by the title (per Stage 0 observation).
//
// =============================================================================

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/hid/nui/nui_constants.h"
#include "xenia/hid/nui/nui_manager.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

using xe::hid::nui::NuiManager;
using xe::hid::nui::SkeletonFrame;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

NuiManager* Nui() { return NuiManager::Instance(); }

// Returns true if Setup() has run and a backend is reporting connected.
bool NuiAttached() {
  auto* m = Nui();
  return m && m->backend() && m->backend()->IsConnected();
}

// Capability check helper.
bool NuiHasCapability(uint32_t cap_bit) {
  auto* m = Nui();
  return m && m->backend() && (m->backend()->Capabilities() & cap_bit) != 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// XamNuiGetSkeletonCapabilities
//
// Observed argument shape (TODO confirm Stage 0): single dword out-pointer
// receiving capability bitfield.
// Upstream stub returned X_E_FAIL.
// ---------------------------------------------------------------------------

dword_result_t XamNuiGetSkeletonCapabilities_entry(
    pointer_t<uint32_t> out_caps) {
  if (!Nui()) {
    return xe::hid::nui::X_E_NUI_NOT_INITIALIZED;
  }
  if (out_caps) {
    *out_caps = Nui()->backend()->Capabilities();
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiGetSkeletonCapabilities, kNui, kImplemented);

// ---------------------------------------------------------------------------
// XamNuiGetDeviceStatus
//
// Returns whether a sensor is attached and ready.
// Upstream stub returned X_E_FAIL.
// ---------------------------------------------------------------------------

dword_result_t XamNuiGetDeviceStatus_entry(pointer_t<uint32_t> out_status) {
  if (out_status) {
    *out_status = NuiAttached() ? 1u : 0u;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNui, kImplemented);

// ---------------------------------------------------------------------------
// XamNuiSkeletonGetNextFrame
//
// Polls the next skeleton frame for a given skeleton slot.
// Guest-side struct layout: TODO(stage-0). Tentative layout:
//   uint32_t skeleton_index
//   uint32_t state
//   { float x, y, z; uint32_t state; } joints[20]
//   uint64_t timestamp_us
// = 4 + 4 + 20*16 + 8 = 336 bytes.
// ---------------------------------------------------------------------------

dword_result_t XamNuiSkeletonGetNextFrame_entry(dword_t skeleton_index,
                                                pointer_t<uint8_t> out_frame) {
  if (!NuiAttached()) {
    return xe::hid::nui::X_E_NUI_DEVICE_NOT_CONNECTED;
  }
  if (!NuiHasCapability(xe::hid::nui::kCapabilitySkeleton)) {
    return xe::hid::nui::X_E_NUI_NOT_SUPPORTED;
  }

  auto frame_opt = Nui()->backend()->PollSkeleton(skeleton_index);
  if (!frame_opt.has_value()) {
    return xe::hid::nui::X_E_NUI_FRAME_NOT_READY;
  }

  // TODO(stage-0): Replace this struct write with the guest-observed layout.
  // The current code writes our host struct verbatim, which is almost
  // certainly wrong byte-order on the guest (PowerPC big-endian).
  if (out_frame) {
    const SkeletonFrame& f = *frame_opt;
    auto* dst = out_frame.host_address();
    std::memset(dst, 0, sizeof(SkeletonFrame));  // SAFE size; guest may diverge
    std::memcpy(dst, &f, sizeof(SkeletonFrame));
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetNextFrame, kNui, kStub);

// ---------------------------------------------------------------------------
// XamNuiEnableSkeletonTracking
//
// Game tells us "yes, we want skeleton data". Upstream stub no-op'd.
// We treat as a hint — we always poll on demand anyway — but log it so
// Stage 0 telemetry can observe whether a title calls this before
// expecting skeleton data.
// ---------------------------------------------------------------------------

dword_result_t XamNuiEnableSkeletonTracking_entry(dword_t enable) {
  XELOGI("XamNuiEnableSkeletonTracking(%d)", static_cast<int>(enable));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiEnableSkeletonTracking, kNui, kImplemented);

// ---------------------------------------------------------------------------
// Functions still TODO — listed here so the diff against upstream is
// visible and Stage 0 has a checklist:
//
//   XamNuiInit                               (TODO: lifecycle)
//   XamNuiShutdown                           (TODO: lifecycle)
//   XamNuiSetDeviceStatusChangedCallback     (TODO: hot-plug)
//   XamNuiImageGetNextFrame                  (TODO: depth/color stream)
//   XamNuiCameraSetMode                      (TODO: capture config)
//   XamNuiSetSkeletonTrackingFlags           (TODO: tracking modes)
//   XamNuiTransformSmooth                    (TODO: skeleton smoothing)
//
// Tracking issue: <REPLACE WITH KINECTIX ISSUE NUMBER>
// ---------------------------------------------------------------------------

}  // namespace xam
}  // namespace kernel
}  // namespace xe

// Module-registration helper invoked by xam_module.cc. Upstream uses this
// pattern so all xam_*.cc files self-register their exports.
DECLARE_XAM_REGISTER_EXPORTS(Nui);
