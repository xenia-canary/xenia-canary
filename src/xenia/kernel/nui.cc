/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/nui.h"
#include "xenia/base/assert.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
NUIController::NUIController() {};
bool NUIController::NuiInitialized() {
  // TODO(boma): Proper device readiness.
  return (KernelState::shared()->xconfig()->ReadSetting<uint32_t>(
              X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY,
              XCONFIG_USER_RETAIL_FLAGS) &
          X_RETAIL_FLAGS::KinectInitialized) != 0;
}
uint32_t NUIController::GetNUIDataPtr() { return nui_data_ptr; }
char NUIController::GetUnknown2() { return nui_unknown_2; }
void NUIController::SetTiltCallback(uint32_t callback) {
  tilt_callback_ = callback;
}
void NUIController::SetEngagedTrackingId(uint32_t tracking_id) {
  engaged_tracking_id = tracking_id;
}
uint32_t NUIController::GetEngagedTrackingId() { return engaged_tracking_id; }
uint32_t NUIController::GetInitFlags() { return init_flags_; }
void NUIController::SetInitFlags(uint32_t flags) { init_flags_ = flags; }
uint64_t NUIController::GetNUIVersion(uint32_t index) {
  if (index > 1) {
    // You set index out of bounds
    assert_always();
    return 0;
  }
  return nui_versions_[index];
}
void NUIController::SetNUIVersion(uint64_t version, uint32_t index) {
  nui_versions_[index] = version;
}
uint64_t NUIController::GetSessionId() { return session_id; }
void NUIController::SetSessionId(uint64_t id) { session_id = id; }

uint32_t NUIController::NuiHudCheck(uint64_t tracking_id) {
  if (!GetNUIDataPtr() || !GetUnknown2() || !NuiInitialized()) {
    return X_ERROR_ACCESS_DENIED;
  }
  if (tracking_id) {
    SetEngagedTrackingId(tracking_id);
  };
  return X_ERROR_SUCCESS;
};

}  // namespace kernel
}  // namespace xe
