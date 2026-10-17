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
class Memory;
namespace kernel {
namespace xboxkrnl {

void VdQueryVideoMode(X_VIDEO_MODE* video_mode, bool is_internal_resolution);

// Restores the video variables titles set (the D3D device pointers) to their
// initial values, so the next title doesn't see the previous title's device.
void ResetVideoVariables(Memory* memory);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_VIDEO_H_
