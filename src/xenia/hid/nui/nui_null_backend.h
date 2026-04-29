/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NUI_NUI_NULL_BACKEND_H_
#define XENIA_HID_NUI_NUI_NULL_BACKEND_H_

#include "xenia/hid/nui/nui_backend.h"

namespace xe {
namespace hid {
namespace nui {

// ----------------------------------------------------------------------------
// NullNuiBackend
//
// Default backend installed when --nui_backend=none, when the user gives
// a backend that wasn't built into this binary, or when a real backend
// fails to Initialize().
//
// Reports as disconnected, exposes no capabilities, returns nullopt for
// every Poll*(). Games querying NUI status will see "no Kinect attached"
// and either skip Kinect features or fail gracefully — same behavior as
// running on a console with the sensor unplugged.
//
// Cheap to construct, never blocks. Safe to leave installed permanently.
// ----------------------------------------------------------------------------

class NullNuiBackend final : public INuiBackend {
 public:
  NullNuiBackend() = default;
  ~NullNuiBackend() override = default;

  bool Initialize() override { return true; }
  void Shutdown() override {}
  bool IsConnected() const override { return false; }
  uint32_t Capabilities() const override { return 0; }

  std::optional<SkeletonFrame> PollSkeleton(uint32_t /*index*/) override {
    return std::nullopt;
  }
  std::optional<DepthFrame> PollDepth() override { return std::nullopt; }
  std::optional<ColorFrame> PollColor() override { return std::nullopt; }

  std::string Name() const override { return "null"; }
};

}  // namespace nui
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NUI_NUI_NULL_BACKEND_H_
