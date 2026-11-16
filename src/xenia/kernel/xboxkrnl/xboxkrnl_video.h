/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_

#include "xenia/kernel/kernel.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void VdQueryVideoMode(X_VIDEO_MODE* video_mode, bool is_internal_resolution);

enum X_VIDEO_OUT : uint32_t {
  X_TV = 0x0,
  X_VGA = 0x1,
  X_DVI = 0x2,
  X_HDMI = 0x3,
};

enum VD_VIDEO_FLAGS : uint32_t {
  has_3d = 0x00000001,
  digital = 0x00000100,
};

enum VD_OPTION_CATEGORIES : uint32_t {
  color_space = 0x1,
  context_read = 0x2,
  unknown_3 = 0x3,
  reset_settings = 0x4,
  mode_3d = 0x5,
};

enum VD_OPTION_COLOR_SPACE : uint32_t {
  RGB = 0x00000000,
  YCbCr_601 = 0x00000001,
  YCbCr_709 = 0x00000002,
  Source = 0x00000006,
  Auto = 0x00000007,
};

enum VD_OPTION_3D_MODE : uint32_t {
  mode_on = 0x0,
  mode_off = 0x1,
};

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_
