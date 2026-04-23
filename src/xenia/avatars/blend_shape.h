/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_BLEND_SHAPE_H_
#define XENIA_AVATARS_BLEND_SHAPE_H_

#include <memory>
#include <vector>

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/common.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

class BitStream;

typedef uint32_t BlendShapeLoadOptions;

namespace BlendShapeLoadOption {

using Option = BlendShapeLoadOptions;

const Option kNone = 0;

const Option kInvert = 1 << 0;

}  // namespace BlendShapeLoadOption

struct BlendShapeIndexPatch {
 public:
  uint32_t total_buffer_size;
  AssetId original_asset_id;
  std::vector<int32_t> indices;

 public:
  static BlendShapeIndexPatch Read(BitStream& stream);
};

struct BlendShapeVertex {
 public:
  int32_t original_offset;
  Vector3<float> position;
  uint32_t normal;
  uint32_t blend_weight;
  uint32_t blend_indices;
  uint32_t color;
};

struct BlendShapeVertexPatch {
 public:
  uint32_t total_buffer_size;
  AssetId original_asset_id;
  std::vector<BlendShapeVertex> vertices;

 public:
  static BlendShapeVertexPatch Read(BitStream& stream,
                                    BlendShapeLoadOptions load_options);
};

class BlendShape {
 public:
  BlendShapeIndexPatch index_patch;
  BlendShapeVertexPatch vertex_patch;

 public:
  bool matches(const AssetId& asset_id) const;

 public:
  static std::shared_ptr<BlendShape> Load(const uint8_t* strb_buffer,
                                          size_t strb_size,
                                          BlendShapeLoadOptions load_options);

 private:
  static std::shared_ptr<BlendShape> Read(const uint8_t* data_buffer,
                                          size_t data_size,
                                          BlendShapeLoadOptions load_options);
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_BLEND_SHAPE_H_
