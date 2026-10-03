/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/kinect/usb_camera.h"

namespace xe {
namespace hid {
namespace kinect {

// TODO(knuckleslee): libusb is only built for Windows so far.
std::unique_ptr<UsbCamera> UsbCamera::Open(bool use_usbdk,
                                           std::function<void()> on_frame) {
  return nullptr;
}

}  // namespace kinect
}  // namespace hid
}  // namespace xe
