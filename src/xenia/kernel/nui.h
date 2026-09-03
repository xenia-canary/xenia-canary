/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_NUI_H_
#define XENIA_KERNEL_NUI_H_

#include <cstdint>

namespace xe {
namespace kernel {

class NUIController {
  /* NUI Notes:
     - nui_hud_flags:
       - set by 0x2B003
       - set to 0 by unnamed func alongside version_id
       - known values:
          - 0x40000000
          - 0x200
     - nui_ver1 & 2:
       - set by 0x2B003
       - set to 0 by unnamed func alongside nui_hud_flags
       - known values:
          - 0x40000000
          - 0x200
  */
 public:
  NUIController();
  ~NUIController() = default;

  uint32_t GetNUIDataPtr();
  char GetUnknown2();
  void SetCallback(uint32_t callback);
  uint32_t GetEngagedTrackingId();
  void SetEngagedTrackingId(uint32_t tracking_id);
  uint32_t GetHudFlags();
  void SetHudFlags(uint32_t flags);
  uint64_t GetNUIVerID(uint32_t index);
  void SetNUIVerID(uint64_t ver_id, uint32_t index);
  uint64_t GetSessionId();
  void SetSessionId(uint64_t id);

 private:
  uint32_t nui_data_ptr = 0x1;  // Meant to be a ptr to a larger structure
  char nui_unknown_2 = 0x1;     // exists at 0x50 within nui data structure
  uint32_t engaged_tracking_id =
      0x0;  // exists at 0x118 within nui data structure
  uint64_t session_id = 0x0;
  uint32_t nui_callback = 0x0;
  uint32_t nui_hud_flags = 0x0;
  uint64_t nui_ver_id[2] = {};
};

struct X_NUI_TILT_STATUS {
  uint32_t buffer_size;
  uint32_t unk2;  // generally 0x58745373 (XtSs)?
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_NUI_H_
