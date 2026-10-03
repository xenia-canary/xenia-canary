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

namespace xe {
namespace kernel {
NUIController::NUIController() {};
uint32_t NUIController::GetNUIDataPtr() { return nui_data_ptr; }
char NUIController::GetUnknown2() { return nui_unknown_2; }
void NUIController::SetCallback(uint32_t callback) { nui_callback = callback; }
void NUIController::SetEngagedTrackingId(uint32_t tracking_id) {
  engaged_tracking_id = tracking_id;
}
uint32_t NUIController::GetEngagedTrackingId() { return engaged_tracking_id; }
uint32_t NUIController::GetHudFlags() { return nui_hud_flags; }
void NUIController::SetHudFlags(uint32_t flags) { nui_hud_flags = flags; }
uint64_t NUIController::GetNUIVerID(uint32_t index) {
  if (index > 1) {
    // You set index out of bounds
    assert_always();
    return 0;
  }
  return nui_ver_id[index];
}
void NUIController::SetNUIVerID(uint64_t ver_id, uint32_t index) {
  nui_ver_id[index] = ver_id;
}
uint64_t NUIController::GetSessionId() { return session_id; }
void NUIController::SetSessionId(uint64_t id) { session_id = id; }

}  // namespace kernel
}  // namespace xe
