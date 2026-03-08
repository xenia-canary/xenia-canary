/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_XMP_VOLUME_PATCH_H_
#define XENIA_KERNEL_UTIL_XMP_VOLUME_PATCH_H_

#include <cstdint>
#include <memory>
#include <string>

namespace xe {
namespace kernel {

class KernelState;

// Patch for games that store their BGM volume in a save file but never call
// XMPSetVolume. Each game just needs to specify:
//   - file_name:    substring to match in the file path
//   - write_length: exact length of the write containing the volume byte
//                   (0 to match any write >= offset + 1)
//   - offset:       byte offset of the volume value within the write buffer
//   - max_value:    maximum volume value (maps to 1.0f)
//
// On savegame read/write, the patch reads the volume byte and syncs it to
// the XMP audio player. It also captures the guest memory address of the
// buffer and re-reads it on controller input changes to detect in-game
// volume adjustments before they are saved.
struct XmpVolumeConfig {
  const char* file_name;
  uint32_t write_length;
  uint32_t offset;
  uint8_t max_value;
};

class XmpVolumePatch {
 public:
  XmpVolumePatch(KernelState* kernel_state, const XmpVolumeConfig& config)
      : kernel_state_(kernel_state), config_(config) {}

  void OnFileWrite(const std::string& path, const uint8_t* buffer,
                   uint32_t length, uint32_t guest_address);
  void OnFileRead(const std::string& path, const uint8_t* buffer,
                  uint32_t length, uint32_t guest_address);
  void OnInputPoll(uint32_t packet_number);

  static std::unique_ptr<XmpVolumePatch> CreateForTitle(uint32_t title_id,
                                                        KernelState* ks);

 private:
  bool Matches(const std::string& path, uint32_t length) const;
  void ApplyVolume(const uint8_t* buffer, uint32_t length);
  void PollGuestVolume();

  KernelState* kernel_state_;
  XmpVolumeConfig config_;
  uint32_t settings_guest_addr_ = 0;
  uint8_t last_volume_ = 0xFF;
  uint32_t last_packet_number_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_XMP_VOLUME_PATCH_H_
