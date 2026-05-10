/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_GDFX_UTIL_H_
#define XENIA_VFS_GDFX_UTIL_H_

#include <cstdint>
#include <cstring>
#include <optional>

#include "xenia/base/memory.h"

namespace xe {
namespace vfs {

constexpr size_t kGdfxSectorSize = 2048;
constexpr char kGdfxMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kGdfxMagicSize = 20;
constexpr uint8_t kGdfxFileAttributeDirectory = 0x10;

// Known offsets where the game partition might start.
constexpr size_t kGdfxLikelyOffsets[] = {
    0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000,
};

struct GdfxPartitionInfo {
  size_t game_offset;
  uint32_t root_sector;
  uint32_t root_size;
};

inline bool GdfxVerifyMagic(const uint8_t* data, size_t size, size_t offset) {
  if (offset + kGdfxMagicSize > size) {
    return false;
  }
  return std::memcmp(data + offset, kGdfxMagic, kGdfxMagicSize) == 0;
}

inline std::optional<GdfxPartitionInfo> GdfxFindPartition(const uint8_t* data,
                                                          size_t size) {
  for (size_t offset : kGdfxLikelyOffsets) {
    // Magic is at sector 32 of the game partition.
    if (GdfxVerifyMagic(data, size, offset + (32 * kGdfxSectorSize))) {
      size_t sector32_offset = offset + (32 * kGdfxSectorSize);
      if (sector32_offset + 28 > size) {
        continue;
      }

      const uint8_t* fs_ptr = data + sector32_offset;
      uint32_t root_sector = xe::load<uint32_t>(fs_ptr + 20);
      uint32_t root_size = xe::load<uint32_t>(fs_ptr + 24);

      if (root_size < 13 || root_size > 32 * 1024 * 1024) {
        continue;
      }

      return GdfxPartitionInfo{offset, root_sector, root_size};
    }
  }
  return std::nullopt;
}

struct GdfxFileLocation {
  size_t offset;
  size_t length;
};

inline bool GdfxEqualsIgnoreCase(const char* a, size_t a_len, const char* b,
                                 size_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  for (size_t i = 0; i < a_len; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca += 32;
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb += 32;
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

namespace detail {

inline std::optional<GdfxFileLocation> GdfxFindFileInDirectory(
    const uint8_t* image_data, size_t image_size, size_t game_offset,
    const uint8_t* dir_buffer, size_t dir_size, uint16_t entry_ordinal,
    const char* target_name, size_t target_len, int depth) {
  if (depth > 100) {
    return std::nullopt;
  }

  size_t entry_offset = static_cast<size_t>(entry_ordinal) * 4;
  if (entry_offset + 14 > dir_size) {
    return std::nullopt;
  }

  const uint8_t* p = dir_buffer + entry_offset;
  uint16_t node_l = xe::load<uint16_t>(p + 0);
  uint16_t node_r = xe::load<uint16_t>(p + 2);
  uint32_t sector = xe::load<uint32_t>(p + 4);
  uint32_t length = xe::load<uint32_t>(p + 8);
  uint8_t attributes = p[12];
  uint8_t name_length = p[13];

  if (entry_offset + 14 + name_length > dir_size) {
    return std::nullopt;
  }

  const char* name = reinterpret_cast<const char*>(p + 14);

  if (node_l != 0) {
    auto result = GdfxFindFileInDirectory(image_data, image_size, game_offset,
                                          dir_buffer, dir_size, node_l,
                                          target_name, target_len, depth + 1);
    if (result) {
      return result;
    }
  }

  bool is_directory = (attributes & kGdfxFileAttributeDirectory) != 0;
  if (!is_directory &&
      GdfxEqualsIgnoreCase(name, name_length, target_name, target_len)) {
    size_t file_offset =
        game_offset + (static_cast<size_t>(sector) * kGdfxSectorSize);
    return GdfxFileLocation{file_offset, length};
  }

  if (node_r != 0) {
    auto result = GdfxFindFileInDirectory(image_data, image_size, game_offset,
                                          dir_buffer, dir_size, node_r,
                                          target_name, target_len, depth + 1);
    if (result) {
      return result;
    }
  }

  return std::nullopt;
}

}  // namespace detail

inline std::optional<GdfxFileLocation> GdfxFindFile(
    const uint8_t* data, size_t size, const GdfxPartitionInfo& partition,
    const char* filename) {
  size_t root_offset =
      partition.game_offset +
      (static_cast<size_t>(partition.root_sector) * kGdfxSectorSize);

  if (root_offset + partition.root_size > size) {
    return std::nullopt;
  }

  const uint8_t* root_buffer = data + root_offset;
  size_t filename_len = std::strlen(filename);

  return detail::GdfxFindFileInDirectory(data, size, partition.game_offset,
                                         root_buffer, partition.root_size, 0,
                                         filename, filename_len, 0);
}

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_GDFX_UTIL_H_
