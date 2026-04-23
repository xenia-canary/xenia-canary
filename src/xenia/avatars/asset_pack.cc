/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/asset_pack_internal.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string.h"

namespace xe {
namespace avatars {

const uint8_t kAvatarTOCv1[16] = {0x9A, 0xD6, 0xEB, 0xCE, 0x62, 0x62,
                                  0x4E, 0xEB, 0x8A, 0x82, 0xA3, 0xF7,
                                  0x0B, 0x81, 0x73, 0x69};
const uint8_t kAvatarTOCv2[16] = {0x8A, 0x76, 0x2D, 0xF4, 0xC0, 0x67,
                                  0x4B, 0x66, 0xB4, 0xEE, 0x56, 0x46,
                                  0x6B, 0x1B, 0x82, 0x80};
const uint8_t kAvatarTOCv3[16] = {0x58, 0x0A, 0x07, 0xD6, 0x4B, 0xCD,
                                  0x40, 0xBE, 0xBF, 0x37, 0x25, 0x4B,
                                  0xE8, 0x26, 0xFB, 0x0B};

const uint8_t kGenericAvatarGuid[16] = {
    0x01, 0x00, 0x00, 0x00, 0x04, 0x48, 0x00, 0x01,
    0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0,
};

bool LoadAssetPackFromMemoryV1(const uint8_t* data, size_t data_size,
                               std::vector<AssetInfo>& asset_infos) {
  auto header_size = sizeof(AssetPackFileHeaderV1);
  if (data_size < header_size) {
    XELOGE("avatar asset pack V1: cannot load file header, buffer too small");
    return false;
  }

  const auto& header =
      *reinterpret_cast<const AssetPackFileHeaderV1*>(&data[0]);

  header_size += header.asset_count * sizeof(AssetPackAssetHeaderV1);
  if (data_size < header_size) {
    XELOGE("avatar asset pack V1: cannot load asset headers, buffer too small");
    return false;
  }

  auto asset_headers =
      reinterpret_cast<const AssetPackAssetHeaderV1*>(&data[sizeof(header)]);
  for (size_t i = 0; i < header.asset_count; ++i) {
    const auto& asset_header = asset_headers[i];
    auto asset_data_offset = size_t(asset_header.data_offset);
    auto asset_data_size = size_t(asset_header.data_size);
    if (data_size < asset_data_offset + asset_data_size) {
      XELOGE("avatar asset pack V1: cannot load asset #{}, buffer too small",
             i);
      asset_infos.push_back({});
      continue;
    }
    AssetInfo asset_info{};
    asset_info.categories = asset_header.categories;
    asset_info.bodies = asset_header.bodies;
    asset_info.random_bodies = asset_header.random_bodies;
    asset_info.flags = asset_header.flags;
    std::memcpy(asset_info.unknown_007, asset_header.unknown_007,
                sizeof(asset_info.unknown_007));
    asset_info.subcategory = asset_header.subcategory;
    std::memcpy(asset_info.asset_ids, asset_header.asset_ids,
                sizeof(asset_info.asset_ids));
    asset_info.name_offset = asset_header.name_offsets[1];
    asset_info.data_offset = asset_data_offset;
    asset_info.data_size = asset_data_size;
    asset_infos.push_back(asset_info);
  }
  return true;
}

bool LoadAssetPackFromMemoryV2(const uint8_t* data, size_t data_size,
                               std::vector<AssetInfo>& asset_infos) {
  auto header_size = sizeof(AssetPackFileHeaderV2);
  if (data_size < header_size) {
    XELOGE("avatar asset pack V2: cannot load file header, buffer too small");
    return false;
  }

  const auto& header =
      *reinterpret_cast<const AssetPackFileHeaderV2*>(&data[0]);

  header_size += header.asset_count * sizeof(AssetPackAssetHeaderV2);
  if (data_size < header_size) {
    XELOGE("avatar asset pack V2: cannot load asset headers, buffer too small");
    return false;
  }

  auto asset_headers =
      reinterpret_cast<const AssetPackAssetHeaderV2*>(&data[sizeof(header)]);
  for (size_t i = 0; i < header.asset_count; ++i) {
    const auto& asset_header = asset_headers[i];
    auto asset_data_offset = size_t(asset_header.data_offset);
    auto asset_data_size = size_t(asset_header.data_size);
    if (data_size < asset_data_offset + asset_data_size) {
      XELOGE("avatar asset pack V2: cannot load asset #{}, buffer too small",
             i);
      asset_infos.push_back({});
      continue;
    }

    AssetInfo asset_info{};
    asset_info.categories = asset_header.categories;
    asset_info.bodies = asset_header.bodies;
    asset_info.random_bodies = asset_header.random_bodies;
    asset_info.flags = asset_header.flags;
    std::memcpy(asset_info.unknown_007, asset_header.unknown_007,
                sizeof(asset_info.unknown_007));
    asset_info.subcategory = asset_header.subcategory;
    std::memcpy(asset_info.asset_ids, asset_header.asset_ids,
                sizeof(asset_info.asset_ids));
    asset_info.name_offset = asset_header.name_offsets[1];
    asset_info.data_offset = asset_data_offset;
    asset_info.data_size = asset_data_size;
    asset_infos.push_back(asset_info);
  }
  return true;
}

bool LoadAssetPackFromMemoryV3(const uint8_t* data, size_t data_size,
                               std::vector<AssetInfo>& asset_infos) {
  auto header_size = sizeof(AssetPackFileHeaderV3);
  if (data_size < header_size) {
    XELOGE("avatar asset pack V3: cannot load file header, buffer too small");
    return false;
  }

  const auto& header =
      *reinterpret_cast<const AssetPackFileHeaderV3*>(&data[0]);

  header_size += header.asset_count * sizeof(AssetPackAssetHeaderV3);
  if (data_size < header_size) {
    XELOGE("avatar asset pack V3: cannot load asset headers, buffer too small");
    return false;
  }

  auto asset_headers =
      reinterpret_cast<const AssetPackAssetHeaderV3*>(&data[sizeof(header)]);
  for (size_t i = 0; i < header.asset_count; ++i) {
    const auto& asset_header = asset_headers[i];
    auto asset_data_offset = size_t(asset_header.data_offset);
    auto asset_data_size = size_t(asset_header.data_size);
    if (data_size < asset_data_offset + asset_data_size) {
      XELOGE("avatar asset pack V3: cannot load asset #{}, buffer too small",
             i);
      asset_infos.push_back({});
      continue;
    }

    AssetInfo asset_info{};
    asset_info.categories = asset_header.categories;
    asset_info.bodies = asset_header.bodies;
    asset_info.random_bodies = asset_header.random_bodies;
    asset_info.flags = asset_header.flags;
    std::memcpy(asset_info.unknown_007, asset_header.unknown_007,
                sizeof(asset_info.unknown_007));
    asset_info.subcategory = asset_header.subcategory;
    std::memcpy(asset_info.asset_ids, asset_header.asset_ids,
                sizeof(asset_info.asset_ids));
    asset_info.name_offset = asset_header.name_offsets[1];
    asset_info.data_offset = asset_data_offset;
    asset_info.data_size = asset_data_size;
    asset_infos.push_back(asset_info);
  }
  return true;
}

bool LoadAssetPackFromMemory(const void* buffer, size_t buffer_size,
                             std::vector<AssetInfo>& asset_infos) {
  if (buffer_size < 16) {
    XELOGE("Cannot load avatar asset pack: buffer too small for version");
    return false;
  }

  auto data = static_cast<const uint8_t*>(buffer);

  const uint8_t* version_guids[3] = {kAvatarTOCv1, kAvatarTOCv2, kAvatarTOCv3};
  size_t version = 0;
  for (size_t i = 0; i < xe::countof(version_guids); ++i) {
    if (!std::memcmp(&data[0], version_guids[i], 16)) {
      version = 1 + i;
      break;
    }
  }

  if (!version) {
    XELOGE("Cannot load avatar asset pack: unknown version");
    return false;
  }

  switch (version) {
    case 1:
      return LoadAssetPackFromMemoryV1(data, buffer_size, asset_infos);
    case 2:
      return LoadAssetPackFromMemoryV2(data, buffer_size, asset_infos);
    case 3:
      return LoadAssetPackFromMemoryV3(data, buffer_size, asset_infos);
    default:
      break;
  }

  XELOGE("Cannot load avatar asset pack: unsupported version {}", version);
  return false;
}

AssetPack::AssetPack() : is_loaded_(false) {}

bool AssetPack::Load(std::vector<uint8_t>& data_bytes) {
  if (is_loaded_) {
    return true;
  }

  std::vector<AssetInfo> asset_infos;
  bool was_loaded = LoadAssetPackFromMemory(data_bytes.data(),
                                            data_bytes.size(), asset_infos);
  if (!was_loaded) {
    return false;
  }

  data_bytes.swap(data_bytes_);
  asset_infos.swap(asset_infos_);
  is_loaded_ = true;

  for (const auto& asset_info : asset_infos_) {
    const uint8_t* asset_buffer;
    size_t asset_size;
    if (!GetAssetData(asset_info.asset_ids[0], asset_buffer, asset_size) &&
        !GetAssetData(asset_info.asset_ids[1], asset_buffer, asset_size)) {
      continue;
    }
    const uint8_t* strb_buffer;
    size_t strb_size;
    if (!strb::GetSTRBBlock(asset_buffer, asset_size,
                            strb::STRBBlockId::kSkeleton, strb_buffer,
                            strb_size)) {
      continue;
    }

    // XELOGI("whoa!! {}", asset_info.asset_ids[0].to_string());
  }

  return true;
}

bool AssetPack::FindAsset(AssetId id, AssetInfo& info) const {
  auto index = id.b;
  if (index >= asset_infos_.size()) {
    return false;
  }
  info = asset_infos_[index];
  return true;
}

bool AssetPack::GetAssetData(AssetId id, const uint8_t*& buffer,
                             size_t& size) const {
  auto index = id.b;
  if (index >= asset_infos_.size()) {
    return false;
  }
  const auto& asset_info = asset_infos_[index];
  buffer = &(data_bytes_.data()[asset_info.data_offset]);
  size = asset_info.data_size;
  return true;
}

std::u16string AssetPack::GetAssetName(AssetId id) const {
  auto index = id.b;
  if (index >= asset_infos_.size()) {
    return std::u16string();
  }
  const auto& asset_info = asset_infos_[index];
  if (!asset_info.name_offset) {
    return std::u16string();
  }
  return load_and_swap<std::u16string>(
      &(data_bytes_.data()[asset_info.name_offset]));
}

}  // namespace avatars
}  // namespace xe
