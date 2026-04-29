/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NUI_RECORDED_XNUIREC_READER_H_
#define XENIA_HID_NUI_RECORDED_XNUIREC_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "xenia/hid/nui/nui_backend.h"

namespace xe {
namespace hid {
namespace nui {
namespace recorded {

// ----------------------------------------------------------------------------
// XnuirecReader
//
// Thin wrapper around the flatbuffer-generated reader for .xnuirec files.
// Owns the file mapping; verifies header magic ("XNUI") and format_version.
// Optionally decompresses .xnuirec.zst on Open().
//
// Implementation lives in xnuirec_reader.cc and includes the generated
// xnuirec_generated.h. Keeping the flatbuffer types out of this header
// reduces compile times for everyone except the reader.cc file.
// ----------------------------------------------------------------------------

class XnuirecReader {
 public:
  XnuirecReader();
  ~XnuirecReader();

  // Open and validate. Returns false on any error (file missing, magic
  // mismatch, format_version unsupported, decompression failure).
  bool Open(const std::string& path);

  // Frame access.
  size_t FrameCount() const;
  uint64_t DurationUs() const;

  // Returns the frame index whose timestamp_us is the largest value
  // that is <= position_us. SIZE_MAX if no such frame.
  size_t FrameIndexAtOrBefore(uint64_t position_us) const;

  // Per-frame reads. Each materializes the requested data into the
  // host-side struct. Returns nullopt if the frame does not contain that
  // stream (e.g. depth missing on a skeleton-only fixture).
  std::optional<SkeletonFrame> ReadSkeleton(size_t frame_index,
                                            uint32_t skeleton_index) const;
  std::optional<DepthFrame> ReadDepth(size_t frame_index) const;
  std::optional<ColorFrame> ReadColor(size_t frame_index) const;

  // Capability bits derived from which streams the recording carries.
  // Set during Open(); matches what INuiBackend::Capabilities() returns.
  uint32_t ObservedCapabilities() const { return observed_capabilities_; }

  // Header passthrough.
  std::string DeviceKindString() const;
  std::string Comment() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint32_t observed_capabilities_ = 0;
};

}  // namespace recorded
}  // namespace nui
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NUI_RECORDED_XNUIREC_READER_H_
