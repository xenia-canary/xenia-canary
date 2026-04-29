/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// =============================================================================
// xnuirec_reader.cc — STUB IMPLEMENTATION
//
// The real implementation depends on flatbuffer code generation from
// xnuirec.fbs (producing xnuirec_generated.h) and integration with our
// flatbuffers vendor lib. Neither is wired into the build yet.
//
// Until that lands, this stub provides:
//   * Definitions for every method declared in xnuirec_reader.h, so the
//     linker is satisfied.
//   * Open() always returns false, with a one-time XELOGW explaining why.
//   * Frame readers all return std::nullopt.
//
// Net effect: RecordedBackend with --nui_backend=recorded reports
// IsConnected() == false at runtime. This matches the documented
// "no fixture loaded" behavior in recorded_backend.h. Default
// --nui_backend=none uses NullNuiBackend and never even constructs a
// RecordedBackend, so the stub is exercised only when a user opts in.
//
// Replace this file once flatbuffers are wired. Tracking: Issue (Stage 1
// follow-up — recorded backend real implementation).
// =============================================================================

#include "xenia/hid/nui/recorded/xnuirec_reader.h"

#include <cstddef>

#include "xenia/base/logging.h"

namespace xe {
namespace hid {
namespace nui {
namespace recorded {

struct XnuirecReader::Impl {
  // Reserved. The real impl will own:
  //   * std::vector<uint8_t> file_buffer_ (mmap or read)
  //   * const Kinectix::Xnuirec::Recording* root_ (flatbuffer view)
  //   * uint64_t duration_us_
  //   * std::vector<uint64_t> frame_timestamps_us_  (for binary search)
};

XnuirecReader::XnuirecReader() : impl_(std::make_unique<Impl>()) {}
XnuirecReader::~XnuirecReader() = default;

bool XnuirecReader::Open(const std::string& path) {
  XELOGW(
      "XnuirecReader::Open(\"%s\") — stub implementation, always returns "
      "false. .xnuirec parsing requires the flatbuffers wiring landed in a "
      "future commit.",
      path.c_str());
  observed_capabilities_ = 0;
  return false;
}

size_t XnuirecReader::FrameCount() const { return 0; }

uint64_t XnuirecReader::DurationUs() const { return 0; }

size_t XnuirecReader::FrameIndexAtOrBefore(uint64_t /*position_us*/) const {
  return SIZE_MAX;
}

std::optional<SkeletonFrame> XnuirecReader::ReadSkeleton(
    size_t /*frame_index*/, uint32_t /*skeleton_index*/) const {
  return std::nullopt;
}

std::optional<DepthFrame> XnuirecReader::ReadDepth(
    size_t /*frame_index*/) const {
  return std::nullopt;
}

std::optional<ColorFrame> XnuirecReader::ReadColor(
    size_t /*frame_index*/) const {
  return std::nullopt;
}

std::string XnuirecReader::DeviceKindString() const { return "stub"; }

std::string XnuirecReader::Comment() const { return {}; }

}  // namespace recorded
}  // namespace nui
}  // namespace hid
}  // namespace xe
