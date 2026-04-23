/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_GUEST_LOAD_ASSET_H_
#define XENIA_AVATARS_GUEST_LOAD_ASSET_H_

#include <vector>

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/memory_block.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

bool LoadAssetsToGuest(const X_AVATAR_METADATA& metadata,
                       uint32_t category_mask, uint32_t flags,
                       AssetPack* asset_pack, MemoryBlock* cpu_memory,
                       MemoryBlock* gpu_memory, uint32_t skeleton_version,
                       uint32_t coordinate_system);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_GUEST_LOAD_ASSET_H_
