/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/nui.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
// https://web.cs.ucdavis.edu/~okreylos/ResDev/Kinect/MainPage.html

struct X_NUI_DEVICE_STATUS {
  /* Notes:
     - for one side func of XamNuiGetDeviceStatus
       - if some data addressis less than zero then unk1 = it
       - else another func is called and its return can set unk1 = c0051200 or
     some value involving DetroitDeviceRequest
       - next PsCamDeviceRequest is called and if its return is less than zero
     then X_NUI_DEVICE_STATUS = return of PsCamDeviceRequest
       - else it equals an unknown local_1c
       - finally McaDeviceRequest is called and if its return is less than zero
     then unk2 = return of McaDeviceRequest
       - else it equals an unknown local_14
     - status can be set to X_NUI_DEVICE_STATUS[3] | 0x44 or | 0x40
  */
  xe::be<uint32_t> unk0;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> status;
  xe::be<uint32_t> serial_number_ptr;  // 0x12 for number & \0\0
  xe::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

// Get
dword_result_t XamNuiGetDeviceStatus_entry(
    pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  /* Notes:
     - it does return a value that is not always used
     - returns values are X_ERROR_SUCCESS, 0xC0050006, and others
     - 1) On func start *status_ptr = 0, status_ptr->unk1 = 0, status_ptr->unk2
     = 0, and status_ptr->status = 0
     - 2) calls XamXStudioRequest(6,&var <- = 0);
     - if return is greater than -1 && var & 0x80000000 != 0 then set
     status_ptr->unk1 = 0xC000009D, status_ptr->unk2 = 0xC000009D, and
     status_ptr->status = status_ptr[3] = 0x20
     - lots of branching functions after
  */

  status_ptr.Zero();

  const bool kinect_initialized = kernel_state()->nui()->NuiInitialized();

  status_ptr->status = kinect_initialized;
  return kinect_initialized ? X_ERROR_SUCCESS : 0xC0050006;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kStub);

dword_result_t XamUserNuiGetUserIndex_entry(unknown_t unk, lpdword_t index) {
  return X_E_NO_SUCH_USER;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

dword_result_t XamUserNuiGetUserIndexForSignin_entry(lpdword_t user_index) {
  if (!user_index) {
    return X_E_INVALIDARG;
  }

  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    auto profile = kernel_state()->xam_state()->GetUserProfile(i);
    if (profile) {
      *user_index = i;
      return X_E_SUCCESS;
    }
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForSignin, kNone, kImplemented);

dword_result_t XamUserNuiGetUserIndexForBind_entry(lpdword_t user_index) {
  if (!user_index) {
    return X_E_INVALIDARG;
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

struct X_NUI_DEPTH_CALIBRATION {
  uint8_t data[0x30];
};
static_assert_size(X_NUI_DEPTH_CALIBRATION, 0x30);

dword_result_t XamNuiGetDepthCalibration_entry(
    pointer_t<X_NUI_DEPTH_CALIBRATION> depth_data) {
  // always cleared before new data written
  std::memset(depth_data, 0, sizeof(X_NUI_DEPTH_CALIBRATION));
  return X_STATUS_NO_SUCH_FILE | X_FACILITY_NT_BIT;
}
DECLARE_XAM_EXPORT1(XamNuiGetDepthCalibration, kNone, kStub);

// Skeleton
qword_result_t XamNuiSkeletonGetBestSkeletonIndex_entry(int_t unk) {
  return 0xffffffffffffffff;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetBestSkeletonIndex, kNone, kStub);

/* XamNuiCamera Notes
   - most require message calls to xam in 0x0002Bxxx area
*/
dword_result_t XamNuiCameraTiltSetCallback_entry(dword_t callback) {
  kernel_state()->nui()->SetCallback(callback);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

dword_result_t XamNuiCameraTiltGetStatus_entry(
    pointer_t<X_NUI_TILT_STATUS> tilt_status) {
  /* Notes:
     - Used by XamNuiCameraElevationGetAngle, and XamNuiCameraSetFlags
     - if it returns anything greater than -1 then both above functions continue
     - Both funcs send in a param of *unk = 0x50 bytes to copy
     - unk2
     - Ghidra decompile fails
  */
  // TODO(knuckleslee): Report the Kinect accelerometer and motor status.
  constexpr int32_t kAccelerometerOneG = 819;
  if (!tilt_status || tilt_status->buffer_size < sizeof(X_NUI_TILT_STATUS) ||
      tilt_status->signature != make_fourcc("XtSs")) {
    return X_E_FAIL;
  }
  std::memset(tilt_status->unknown_08, 0, sizeof(tilt_status->unknown_08));
  // Level and not moving.
  tilt_status->accelerometer[0] = 0;
  tilt_status->accelerometer[1] = kAccelerometerOneG;
  tilt_status->accelerometer[2] = 0;
  tilt_status->flags = static_cast<X_TILT_STATUS_FLAGS>(0);
  std::memset(tilt_status->unknown_30, 0, sizeof(tilt_status->unknown_30));
  return X_E_FAIL;  // until implementation improves
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltGetStatus, kNone, kStub);

dword_result_t XamNuiCameraElevationGetAngle_entry(lpqword_t elevation_angle,
                                                   lpdword_t camera_moving) {
  X_NUI_TILT_STATUS tilt_status;
  tilt_status.buffer_size = sizeof(tilt_status);
  tilt_status.signature = make_fourcc("XtSs");  // (XtSs)?
  X_STATUS result = XamNuiCameraTiltGetStatus_entry(&tilt_status);
  if (XSUCCEEDED(result)) {
    // TODO(knuckleslee): Output the angle once its format is known.
    // uses accelerometer[2] & accelerometer[1] to find angle
    *elevation_angle = 0;
    *camera_moving =
        tilt_status.flags & (X_TILT_STATUS_FLAGS::TILT_STATUS_UNK1 |
                             X_TILT_STATUS_FLAGS::TILT_STATUS_UNK2);  // moving
    result = X_E_FAIL;  // until implementation improves
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kStub);

dword_result_t XamNuiCameraGetTiltControllerType_entry() {
  /* Notes:
     - calls DetroitDeviceRequest to check for kinect
     - (LZCOUNT(result) << 0x20) >> 0x25;
     - returns true or false for device connected
  */
  const bool kinect_initialized =
      kernel_state()->xconfig()->ReadSetting<uint32_t>(
          X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS) &
      X_RETAIL_FLAGS::KinectInitialized;
  return kinect_initialized;
}
DECLARE_XAM_EXPORT1(XamNuiCameraGetTiltControllerType, kNone, kStub);

dword_result_t XamNuiCameraSetFlags_entry(qword_t unk1, dword_t unk2) {
  X_STATUS result = X_E_DEVICE_NOT_CONNECTED;
  int controller_type = XamNuiCameraGetTiltControllerType_entry();

  if (controller_type == 1) {
    X_NUI_TILT_STATUS tilt_status;
    tilt_status.buffer_size = sizeof(tilt_status);
    tilt_status.signature = make_fourcc("XtSs");  // (XtSs)?
    result = XamNuiCameraTiltGetStatus_entry(&tilt_status);
    if (!XSUCCEEDED(result)) {
      return result;
    }
    // TODO(knuckleslee): Apply the flags once their meaning is known.
    return X_E_FAIL;  // until implementation improves
  }
  if (controller_type > 3) {
    result = X_E_FAIL;  // until implementation improves
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

dword_result_t XamIsNuiUIActive_entry() {
  return kernel_state()->xam_state()->is_xam_dialog_present_.load();
}
DECLARE_XAM_EXPORT1(XamIsNuiUIActive, kNone, kImplemented);

dword_result_t XamNuiIsDeviceReady_entry() {
  /* device_state Notes:
   - used with XNotifyBroadcast(kXNotificationSystemNUIHardwareStatusChanged,
   device_state)
   - known values:
     - 0x0001
     - 0x0004
     - 0x0040
  */

  return kernel_state()->nui()->NuiInitialized();
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamIsNuiAutomationEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNuiAutomationEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamIsNatalPlaybackEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNatalPlaybackEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamNuiIsChatMicEnabled_entry() {
  /* Notes:
     - call ExGetXConfigSetting(7,9,&xconfig_nui,0x1c,local_40);
     - check xconfig_nui.flag & 0x8
     - return True or false based on result
  */
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiIsChatMicEnabled, kNone, kImplemented);

/* HUD Notes:
   - XamNuiHudGetEngagedTrackingID, XamNuiHudIsEnabled,
   XamNuiHudSetEngagedTrackingID, XamNuiHudInterpretFrame, and
   XamNuiHudGetEngagedEnrollmentIndex all utilize the same data address
*/
uint32_t nui_unknown_1 = 0;
uint32_t engaged_tracking_id = 0;
char nui_unknown_2 = '\0';

dword_result_t XamNuiHudSetEngagedTrackingID_entry(dword_t id) {
  auto nui_ = kernel_state()->nui();
  if (id) {
    if (!nui_->GetNUIDataPtr()) {
      return X_E_FAIL;
    }
    nui_->SetEngagedTrackingId(id);
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudSetEngagedTrackingID, kNone, kImplemented);

qword_result_t XamNuiHudGetEngagedTrackingID_entry() {
  auto nui_ = kernel_state()->nui();
  if (nui_->GetNUIDataPtr()) {
    return nui_->GetEngagedTrackingId();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);

dword_result_t XamNuiHudIsEnabled_entry() {
  auto nui_ = kernel_state()->nui();
  bool result = XamNuiIsDeviceReady_entry();
  if (nui_->GetNUIDataPtr() && nui_->GetUnknown2() && result) {
    return true;
  }
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

dword_result_t XamNuiHudGetInitializeFlags_entry() {
  return kernel_state()->nui()->GetHudFlags();
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

void XamNuiHudGetVersions_entry(lpqword_t unk1, lpqword_t unk2) {
  auto nui_ = kernel_state()->nui();
  if (unk1) {
    *unk1 = nui_->GetNUIVerID(0);
  }
  if (unk2) {
    *unk2 = nui_->GetNUIVerID(1);
  }
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

/* XamNuiIdentity Notes:
   - most require message calls to xam in 0x0002Cxxx area
*/
qword_result_t XamNuiIdentityGetSessionId_entry() {
  auto nui_ = kernel_state()->nui();
  if (nui_->GetSessionId() == 0) {
    // xboxkrnl::XeCryptRandom_entry(NUI_Session_Id, 8);
    nui_->SetSessionId(0xDEADF00DDEADF00D);
  }
  return nui_->GetSessionId();
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kImplemented);

dword_result_t XamNuiIdentityEnrollForSignIn_entry(dword_t unk1, qword_t unk2,
                                                   qword_t unk3, dword_t unk4) {
  /* Notes:
     - Decompiler issues so double check
  */
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [2]
  // buffer[0] = unk
  // var = unk4
  // return func(0xfe,0x2c010,buffer,0xc,unk3);
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityEnrollForSignIn, kNone, kStub);

dword_result_t XamNuiIdentityAbort_entry(dword_t unk) {
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [4]
  // buffer[0] = unk
  // return func(0xfe,0x2c00e,buffer,4,0)
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityAbort, kNone, kStub);

// Other
dword_result_t XamUserNuiEnableBiometric_entry(dword_t user_index,
                                               int_t enable) {
  return X_E_INVALIDARG;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kStub);

struct X_NUI_PLAYER_ENGAGEMENT_UPDATE {
  uint8_t data[0x1C];
};
static_assert_size(X_NUI_PLAYER_ENGAGEMENT_UPDATE, 0x1C);

void XamNuiPlayerEngagementUpdate_entry(
    lpunknown_t unk1, unknown_t unk2,
    pointer_t<X_NUI_PLAYER_ENGAGEMENT_UPDATE> engagement_data) {
  // always cleared before new data written
  std::memset(engagement_data, 0, sizeof(X_NUI_PLAYER_ENGAGEMENT_UPDATE));
}
DECLARE_XAM_EXPORT1(XamNuiPlayerEngagementUpdate, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
