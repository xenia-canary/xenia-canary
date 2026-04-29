/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/nui/recorded/recorded_backend.h"

#include <algorithm>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/hid/nui/recorded/xnuirec_reader.h"

namespace xe {
namespace hid {
namespace nui {
namespace recorded {

namespace {
// Conservative guest frame rate assumption used to map guest frame
// counter -> playback position in microseconds. Real Xbox 360 games run
// at 30 or 60 fps; we pick 60 since that's the upper bound and the
// recorded backend's job is to surface the *closest* frame, never to
// outrun the recording.
//
// A future refinement: read the game's actual frame timing from
// kernel_state. Stage 0 telemetry will tell us if it's worth the wiring.
constexpr double kGuestFrameRateHz = 60.0;
constexpr uint64_t kMicrosPerGuestFrame =
    static_cast<uint64_t>(1'000'000.0 / kGuestFrameRateHz);
}  // namespace

RecordedBackend::RecordedBackend(const std::string& xnuirec_path)
    : path_(xnuirec_path) {}

RecordedBackend::~RecordedBackend() = default;

bool RecordedBackend::Initialize() {
  if (path_.empty()) {
    XELOGI(
        "RecordedBackend: no --nui_record_path provided; backend will "
        "report disconnected. Call Reload() at runtime to attach a fixture.");
    connected_.store(false);
    capabilities_ = 0;
    return true;  // Not a hard failure — see Reload().
  }
  return Reload(path_);
}

void RecordedBackend::Shutdown() {
  reader_.reset();
  connected_.store(false);
}

bool RecordedBackend::Reload(const std::string& xnuirec_path) {
  auto reader = std::make_unique<XnuirecReader>();
  if (!reader->Open(xnuirec_path)) {
    XELOGE("RecordedBackend: failed to open %s", xnuirec_path.c_str());
    return false;
  }

  path_ = xnuirec_path;
  reader_ = std::move(reader);
  capabilities_ = reader_->ObservedCapabilities();
  connected_.store(true);
  last_skeleton_frame_returned_ = SIZE_MAX;
  last_depth_frame_returned_ = SIZE_MAX;
  last_color_frame_returned_ = SIZE_MAX;
  guest_frame_counter_ = 0;

  XELOGI("RecordedBackend: loaded %s (%u frames, caps=0x%X, device=%s)",
         path_.c_str(), reader_->FrameCount(), capabilities_,
         reader_->DeviceKindString().c_str());
  return true;
}

bool RecordedBackend::IsConnected() const { return connected_.load(); }

uint32_t RecordedBackend::Capabilities() const { return capabilities_; }

void RecordedBackend::SetGuestFrameCounter(uint64_t guest_frame) {
  guest_frame_counter_ = guest_frame;
}

void RecordedBackend::SetLoop(bool loop) { loop_ = loop; }

size_t RecordedBackend::FrameIndexForCurrentPosition() const {
  if (!reader_ || reader_->FrameCount() == 0) {
    return SIZE_MAX;
  }

  uint64_t playback_us = guest_frame_counter_ * kMicrosPerGuestFrame;
  const uint64_t duration_us = reader_->DurationUs();

  if (loop_ && duration_us > 0) {
    playback_us %= duration_us;
  } else if (playback_us >= duration_us) {
    return reader_->FrameCount() - 1;
  }

  return reader_->FrameIndexAtOrBefore(playback_us);
}

std::optional<SkeletonFrame> RecordedBackend::PollSkeleton(uint32_t index) {
  if (!connected_.load() || !reader_) {
    return std::nullopt;
  }
  const size_t fi = FrameIndexForCurrentPosition();
  if (fi == SIZE_MAX) {
    return std::nullopt;
  }
  // Only deliver each frame once per stream — kernel side will poll
  // every guest frame, but we should not surface duplicates.
  if (fi == last_skeleton_frame_returned_) {
    return std::nullopt;
  }
  auto frame = reader_->ReadSkeleton(fi, index);
  last_skeleton_frame_returned_ = fi;
  return frame;
}

std::optional<DepthFrame> RecordedBackend::PollDepth() {
  if (!connected_.load() || !reader_) {
    return std::nullopt;
  }
  const size_t fi = FrameIndexForCurrentPosition();
  if (fi == SIZE_MAX || fi == last_depth_frame_returned_) {
    return std::nullopt;
  }
  auto frame = reader_->ReadDepth(fi);
  last_depth_frame_returned_ = fi;
  return frame;
}

std::optional<ColorFrame> RecordedBackend::PollColor() {
  if (!connected_.load() || !reader_) {
    return std::nullopt;
  }
  const size_t fi = FrameIndexForCurrentPosition();
  if (fi == SIZE_MAX || fi == last_color_frame_returned_) {
    return std::nullopt;
  }
  auto frame = reader_->ReadColor(fi);
  last_color_frame_returned_ = fi;
  return frame;
}

}  // namespace recorded
}  // namespace nui
}  // namespace hid
}  // namespace xe
