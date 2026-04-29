/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NUI_RECORDED_RECORDED_BACKEND_H_
#define XENIA_HID_NUI_RECORDED_RECORDED_BACKEND_H_

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "xenia/hid/nui/nui_backend.h"

namespace xe {
namespace hid {
namespace nui {
namespace recorded {

class XnuirecReader;  // forward; defined in xnuirec_reader.h

// ----------------------------------------------------------------------------
// RecordedBackend
//
// INuiBackend implementation that streams skeleton/depth/color frames from
// a .xnuirec file. Loops at end-of-file by default.
//
// Determinism: playback is driven by the *guest* frame counter (see
// SetGuestFrameCounter), not by host wall clock. The same fixture file
// produces identical XAM NUI state on every run regardless of host
// performance. This is essential for CI fixture replay.
//
// If the constructor receives an empty path, the backend behaves like
// NullNuiBackend — connected = false, polls return nullopt. This is
// useful when the user passes --nui_backend=recorded with no path:
// it still allows xam_nui.cc to talk to a "real" recorded backend
// instance whose source can be set later via Reload().
// ----------------------------------------------------------------------------

class RecordedBackend final : public INuiBackend {
 public:
  explicit RecordedBackend(const std::string& xnuirec_path);
  ~RecordedBackend() override;

  // INuiBackend.
  bool Initialize() override;
  void Shutdown() override;
  bool IsConnected() const override;
  uint32_t Capabilities() const override;
  std::optional<SkeletonFrame> PollSkeleton(uint32_t index) override;
  std::optional<DepthFrame> PollDepth() override;
  std::optional<ColorFrame> PollColor() override;
  std::string Name() const override { return "recorded"; }

  // Switch to a different recording at runtime. Returns false on failure;
  // current playback state is unchanged on failure.
  bool Reload(const std::string& xnuirec_path);

  // Drive playback. The kernel calls this once per guest frame; the
  // backend uses it to decide which recorded frame to surface next.
  // Without this hook, playback would not advance and Poll*() would
  // always return the first frame.
  void SetGuestFrameCounter(uint64_t guest_frame);

  // Looping behavior. Default: loop at EOF (true).
  void SetLoop(bool loop);

 private:
  // Find the recording frame whose timestamp_us is closest to but not
  // exceeding the current playback position derived from the guest
  // frame counter.
  size_t FrameIndexForCurrentPosition() const;

  std::string path_;
  std::unique_ptr<XnuirecReader> reader_;

  uint32_t capabilities_ = 0;
  std::atomic<bool> connected_{false};

  // Most recently delivered frame index, per stream. Used to suppress
  // duplicate deliveries: each Poll*() returns std::nullopt when the
  // currently-selected frame index has not advanced since the previous
  // call on that stream.
  size_t last_skeleton_frame_returned_ = SIZE_MAX;
  size_t last_depth_frame_returned_ = SIZE_MAX;
  size_t last_color_frame_returned_ = SIZE_MAX;

  uint64_t guest_frame_counter_ = 0;
  bool loop_ = true;
};

}  // namespace recorded
}  // namespace nui
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NUI_RECORDED_RECORDED_BACKEND_H_
