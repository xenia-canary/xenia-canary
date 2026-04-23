/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_MODEL_SAVE_H_
#define XENIA_AVATARS_MODEL_SAVE_H_

#include <vector>

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/model.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

void SaveModel(const X_AVATAR_COMPONENT_INFO& component_info,
               std::shared_ptr<Model> model, AssetPack* asset_pack);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_GUEST_LOAD_ASSET_H_
