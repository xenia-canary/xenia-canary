/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_KINECT_USB_CAMERA_H_
#define XENIA_HID_KINECT_USB_CAMERA_H_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace xe {
namespace hid {
namespace kinect {

// Calibration stored in the camera.
struct UsbCameraCalibration {
  // Registration record: 29 little-endian 32-bit words.
  std::array<uint8_t, 116> registration = {};
  float dcmos_emitter_distance = 0.0f;
  float dcmos_rcmos_distance = 0.0f;
  float reference_distance = 0.0f;
  float reference_pixel_size = 0.0f;
  uint16_t const_shift = 0;
};

// A Kinect for Xbox 360 camera streaming 640x480 11-bit packed depth and
// 640x480 Bayer color at 30 fps.
class UsbCamera {
 public:
  static constexpr size_t kDepthFrameBytes = 640 * 480 * 11 / 8;
  static constexpr size_t kColorFrameBytes = 640 * 480;

  virtual ~UsbCamera() = default;

  // Returns nullptr when no camera is found or it cannot be opened.
  // `on_frame` is called from a camera thread whenever a new frame is ready.
  static std::unique_ptr<UsbCamera> Open(bool use_usbdk,
                                         std::function<void()> on_frame);

  virtual const UsbCameraCalibration& calibration() const = 0;

  // Copies the newest complete frame if it is newer than `after`. Returns the
  // frame's sequence number, or 0 when there is no newer frame.
  virtual uint32_t CopyDepthFrame(uint32_t after,
                                  std::vector<uint8_t>& packed_depth) = 0;
  virtual uint32_t CopyColorFrame(uint32_t after,
                                  std::vector<uint8_t>& bayer) = 0;
};

}  // namespace kinect
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_KINECT_USB_CAMERA_H_
