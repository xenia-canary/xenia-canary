/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_ASSET_PACK_H_
#define XENIA_AVATARS_ASSET_PACK_H_

#include <string>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

struct AssetId {
  be<uint32_t> a;
  be<uint16_t> b;
  be<uint16_t> c;
  uint8_t d[8];

  bool operator==(const AssetId& other) const {
    return a == other.a && b == other.b && c == other.c &&
           !std::memcmp(d, other.d, sizeof(d));
  }

  bool operator!=(const AssetId& other) const {
    return a != other.a || b != other.b || c != other.c ||
           std::memcmp(d, other.d, sizeof(d));
  }

  bool is_zero() const { return *this == AssetId{}; };

  std::string to_string() const {
    return fmt::format(
        "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:"
        "02x}",
        a.get(), b.get(), c.get(), d[0], d[1], d[2], d[3], d[4], d[5], d[6],
        d[7]);
  }
};
static_assert_size(AssetId, 16);

struct AssetInfo {
  uint32_t categories;
  uint8_t bodies;
  uint8_t random_bodies;
  uint8_t flags;
  uint8_t unknown_007[0x91];
  uint32_t subcategory;
  AssetId asset_ids[2];
  // std::string names[18];
  size_t name_offset;
  size_t data_offset;
  size_t data_size;
};

class AssetPack {
 public:
  AssetPack();
  ~AssetPack() = default;

  bool Load(std::vector<uint8_t>& data_bytes);

  bool FindAsset(AssetId id, AssetInfo& info) const;

  bool GetAssetData(AssetId id, const uint8_t*& buffer, size_t& size) const;

  std::u16string GetAssetName(AssetId id) const;

  bool is_loaded() const { return is_loaded_; }
  const std::vector<AssetInfo>& asset_infos() const { return asset_infos_; }

 private:
  bool is_loaded_;
  std::vector<uint8_t> data_bytes_;
  std::vector<AssetInfo> asset_infos_;
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_ASSET_PACK_H_
