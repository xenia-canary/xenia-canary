/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_BLEND_SHAPE_APPLY_H_
#define XENIA_AVATARS_BLEND_SHAPE_APPLY_H_

#include <memory>
#include <vector>

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/blend_shape.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/model.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

bool ApplyBlendShape(std::shared_ptr<BlendShape> blend_shape,
                     const AssetId& model_asset_id,
                     std::shared_ptr<Model> model);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_BLEND_SHAPE_APPLY_H_
