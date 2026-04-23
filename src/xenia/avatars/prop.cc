/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/prop.h"

#include "xenia/avatars/animation.h"
#include "xenia/avatars/bit_stream.h"
#include "xenia/avatars/blend_shape.h"
#include "xenia/avatars/blend_shape_apply.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/model.h"
#include "xenia/avatars/skeleton.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/logging.h"

namespace xe {
namespace avatars {

std::shared_ptr<Prop> Prop::Load(const uint8_t* strb_buffer, size_t strb_size,
                                 const PropLoadOptions& load_options) {
  auto model = Model::Load(strb_buffer, strb_size, load_options.model);
  if (model == nullptr) {
    return nullptr;
  }

  auto skeleton = Skeleton::Load(strb_buffer, strb_size, load_options.skeleton);
  if (skeleton == nullptr) {
    return nullptr;
  }

  auto instance = std::make_shared<Prop>();
  instance->model = model;
  instance->skeleton = skeleton;
  instance->animation =
      Animation::Load(strb_buffer, strb_size, load_options.animation);

  auto blend_shape =
      BlendShape::Load(strb_buffer, strb_size, load_options.blend_shape);
  if (blend_shape != nullptr) {
    assert_always("needs testing");
    ApplyBlendShape(blend_shape, AssetId{}, model);
  }

  return instance;
}

}  // namespace avatars
}  // namespace xe
