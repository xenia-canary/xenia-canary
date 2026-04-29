/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NUI_NUI_BACKEND_H_
#define XENIA_HID_NUI_NUI_BACKEND_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenia/hid/nui/nui_constants.h"

namespace xe {
namespace hid {
namespace nui {

// ----------------------------------------------------------------------------
// Frame data structures.
//
// These are the host-side intermediate representation between concrete
// backends (libfreenect, recorded playback, MediaPipe) and the XAM NUI HAL
// in xam_nui.cc. Conversion to guest-side struct layouts happens in the
// HAL, not here.
// ----------------------------------------------------------------------------

struct SkeletonJoint {
  // Position in sensor-relative meters. +x right, +y up, +z forward.
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  // Per-joint tracking state. Games occasionally check this to decide
  // whether to render an inferred limb or hide it.
  SkeletonPositionTrackingState tracking_state = kSkeletonPositionNotTracked;
};

struct SkeletonFrame {
  // Skeleton slot index, 0..kMaxSkeletons-1.
  uint32_t skeleton_index = 0;

  // Overall tracking state for this slot.
  SkeletonTrackingState state = kSkeletonNotTracked;

  // Joint data. Only meaningful when state == kSkeletonTracked.
  // For state == kSkeletonPositionOnly, only the hip-center joint is valid.
  std::array<SkeletonJoint, kSkeletonPositionCount> joints{};

  // Microseconds since the backend started, on the host clock.
  // NuiManager translates this into guest time before exposing to xam_nui.cc.
  uint64_t host_timestamp_us = 0;
};

struct DepthFrame {
  uint16_t width = 0;
  uint16_t height = 0;

  // 16-bit packed depth in millimeters. Top bits may carry per-pixel
  // skeleton-id when a skeleton tracker is producing player segmentation.
  std::vector<uint16_t> pixels;

  uint64_t host_timestamp_us = 0;
};

struct ColorFrame {
  uint16_t width = 0;
  uint16_t height = 0;

  // BGRA, row-major, top-left origin.
  std::vector<uint8_t> pixels;

  uint64_t host_timestamp_us = 0;
};

// ----------------------------------------------------------------------------
// INuiBackend
//
// Abstract interface implemented by every NUI source — recorded playback,
// libfreenect, libfreenect2, MediaPipe, etc.
//
// Threading contract:
//   * Initialize() and Shutdown() are called from the main emulator thread.
//   * Poll*() methods are called from the kernel thread that services
//     XAM NUI syscalls. They must be lock-free or use a quick spinlock;
//     they must never block on I/O.
//   * Backends may run their own producer thread; if so, they double- or
//     triple-buffer frames internally so Poll*() returns immediately.
//
// All Poll*() methods return std::nullopt when no new frame is available
// since the previous call. They MUST NOT return a stale frame twice.
// ----------------------------------------------------------------------------

class INuiBackend {
 public:
  virtual ~INuiBackend() = default;

  // Lifecycle.
  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;
  virtual bool IsConnected() const = 0;

  // Static capabilities. May change after Initialize() if the backend
  // discovers what hardware is present, but should not flap.
  virtual uint32_t Capabilities() const = 0;
  bool SupportsSkeleton() const {
    return (Capabilities() & kCapabilitySkeleton) != 0;
  }
  bool SupportsDepth() const {
    return (Capabilities() & kCapabilityDepth) != 0;
  }
  bool SupportsColor() const {
    return (Capabilities() & kCapabilityColor) != 0;
  }

  // Frame polling. See class-level threading contract.
  virtual std::optional<SkeletonFrame> PollSkeleton(uint32_t index) = 0;
  virtual std::optional<DepthFrame> PollDepth() = 0;
  virtual std::optional<ColorFrame> PollColor() = 0;

  // Diagnostics. Used in --nui_status output and crash logs.
  virtual std::string Name() const = 0;
};

}  // namespace nui
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NUI_NUI_BACKEND_H_
