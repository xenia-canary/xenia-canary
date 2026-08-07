/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/optical_drive_device.h"

#include <cstring>

namespace xe {
namespace vfs {
namespace {

constexpr size_t kSecuritySectorRangeEntrySize = 9;
constexpr size_t kSecuritySectorRangeEntryCountV1 = 23;
constexpr size_t kSecuritySectorRangeEntryCountV2 = 21;
constexpr size_t kSecuritySectorRangesAOffset = 1633;
constexpr size_t kSecuritySectorRangesBOffset = 1840;

uint32_t ParseBE24(const uint8_t* data) {
  return (uint32_t(data[0]) << 16) | (uint32_t(data[1]) << 8) |
         uint32_t(data[2]);
}

bool IsAllZero(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (data[i] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool OpticalDriveDevice::IsSecuritySectorSane(
    std::span<const uint8_t> payload) {
  if (payload.size() != SecuritySectorPayloadSize()) {
    return false;
  }

  size_t expected_count = 0;
  if (payload[768] == 1) {
    expected_count = kSecuritySectorRangeEntryCountV1;
  } else if (payload[768] == 2) {
    expected_count = kSecuritySectorRangeEntryCountV2;
  } else {
    return false;
  }

  if (payload[769] != expected_count || payload[1632] != expected_count) {
    return false;
  }

  const size_t ranges_bytes = expected_count * kSecuritySectorRangeEntrySize;
  const uint8_t* ranges_a = payload.data() + kSecuritySectorRangesAOffset;
  const uint8_t* ranges_b = payload.data() + kSecuritySectorRangesBOffset;
  if (payload[768] != 2 && std::memcmp(ranges_a, ranges_b, ranges_bytes) != 0) {
    return false;
  }

  bool has_non_zero_entry = false;
  for (size_t i = 0; i < expected_count; ++i) {
    const uint8_t* entry = ranges_a + i * kSecuritySectorRangeEntrySize;
    if (IsAllZero(entry, kSecuritySectorRangeEntrySize)) {
      continue;
    }
    has_non_zero_entry = true;

    const uint32_t start_psn = ParseBE24(entry + 3);
    const uint32_t end_psn = ParseBE24(entry + 6);
    if (payload[768] == 1 && start_psn > end_psn) {
      return false;
    }
  }

  return has_non_zero_entry;
}

}  // namespace vfs
}  // namespace xe
