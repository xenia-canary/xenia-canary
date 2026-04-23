/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_ASSET_PACK_INTERNAL_H_
#define XENIA_AVATARS_ASSET_PACK_INTERNAL_H_

#include "xenia/avatars/asset_pack.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

struct AssetPackFileHeaderV1 {
  uint8_t type_guid[16];
  be<uint32_t> unknown_004;
  be<uint32_t> unknown_008;
  be<uint32_t> unknown_00C;
  be<uint32_t> asset_count;
};
static_assert_size(AssetPackFileHeaderV1, 0x20);

struct AssetPackFileHeaderV2 : AssetPackFileHeaderV1 {
  be<uint32_t> unknown_020;
  be<uint32_t> unknown_024;
};
static_assert_size(AssetPackFileHeaderV2, 0x28);

struct AssetPackFileHeaderV3 : AssetPackFileHeaderV2 {
  be<uint32_t> unknown_028;
  be<uint32_t> unknown_02C;
};
static_assert_size(AssetPackFileHeaderV3, 0x30);

struct AssetPackAssetHeaderV1 {
  be<uint32_t> categories;
  uint8_t bodies;
  uint8_t random_bodies;
  uint8_t flags;
  uint8_t unknown_007[0x91];
  be<uint32_t> subcategory;
  AssetId asset_ids[2];
  be<uint32_t> name_offsets[13];
  be<uint32_t> data_offset;
  be<uint32_t> data_size;
};
static_assert_size(AssetPackAssetHeaderV1, 0xF8);

typedef AssetPackAssetHeaderV1 AssetPackAssetHeaderV2;

struct AssetPackAssetHeaderV3 {
  be<uint32_t> categories;
  uint8_t bodies;
  uint8_t random_bodies;
  uint8_t flags;
  uint8_t unknown_007[0x91];
  be<uint32_t> subcategory;
  AssetId asset_ids[2];
  be<uint32_t> name_offsets[18];
  be<uint32_t> data_offset;
  be<uint32_t> data_size;
};
static_assert_size(AssetPackAssetHeaderV3, 0x10C);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_ASSET_PACK_INTERNAL_H_
