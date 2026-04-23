/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_GUEST_LOAD_ANIMATION_H_
#define XENIA_AVATARS_GUEST_LOAD_ANIMATION_H_

#include <vector>

#include "xenia/avatars/animation.h"
#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/guest_animation.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/base/memory.h"

namespace xe {

namespace kernel {

class KernelState;

}  // namespace kernel

namespace avatars {

bool LoadAnimationToGuest(const AssetId& asset_id,
                          std::shared_ptr<Animation> animation,
                          X_AVATAR_ANIMATION* guest_animation,
                          uint8_t* guest_compressed_data);

bool LoadAnimationToGuest(AssetPack* asset_pack, const AssetId& asset_id,
                          X_AVATAR_ANIMATION* guest_animation,
                          uint8_t* guest_compressed_data,
                          uint32_t coordinate_system);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_GUEST_LOAD_ANIMATION_H_
