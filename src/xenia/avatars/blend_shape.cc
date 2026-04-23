/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/blend_shape.h"

#include "xenia/avatars/bit_stream.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/serializers.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/logging.h"

namespace xe {
namespace avatars {

using IndexSerializer = ValueSerializer<int32_t>;
using NormalSerializer = ValueSerializer<uint32_t>;
using BlendWeightSerializer = ValueSerializer<uint32_t>;
using BlendIndexSerializer = ValueSerializer<uint32_t>;
using ColorSerializer = ValueSerializer<uint32_t>;

static AssetId ReadAssetId(BitStream& stream) {
  AssetId assetId{};
  assetId.a = stream.Read<uint32_t>();
  assetId.b = stream.Read<uint16_t>();
  assetId.c = stream.Read<uint16_t>();
  stream.ReadBytes(reinterpret_cast<uint8_t*>(&assetId.d), sizeof(assetId.d));
  return assetId;
}

BlendShapeIndexPatch BlendShapeIndexPatch::Read(BitStream& stream) {
  BlendShapeIndexPatch instance{};

  uint32_t index_count = stream.Read<uint32_t>();
  assert_true(index_count <= 8192);

  instance.total_buffer_size = stream.Read<uint32_t>();
  instance.original_asset_id = ReadAssetId(stream);

  auto indexSerializer = IndexSerializer::From(stream);
  for (uint32_t i = 0; i < index_count; ++i) {
    instance.indices.push_back(indexSerializer.Read(stream));
  }

  return instance;
}

struct BlendShapeVertexSerializer {
 public:
  bool inverted;
  IndexSerializer original_index_serializer;
  VectorSerializer position_serializer;
  NormalSerializer normal_serializer;
  BlendWeightSerializer blend_weight_serializer;
  BlendIndexSerializer blend_index_serializer;
  ColorSerializer color_serializer;

  size_t element_bit_size() const {
    return original_index_serializer.element_bit_size() +
           position_serializer.element_bit_size() +
           normal_serializer.element_bit_size() +
           blend_weight_serializer.element_bit_size() +
           blend_index_serializer.element_bit_size() +
           color_serializer.element_bit_size();
  }

  static size_t serializer_bit_size() {
    return IndexSerializer::serializer_bit_size() +
           VectorSerializer::serializer_bit_size() +
           NormalSerializer::serializer_bit_size() +
           BlendWeightSerializer::serializer_bit_size() +
           BlendIndexSerializer::serializer_bit_size() +
           ColorSerializer::serializer_bit_size();
  }

 public:
  static BlendShapeVertexSerializer From(BitStream& stream) {
    BlendShapeVertexSerializer instance{};
    instance.inverted = false;
    instance.original_index_serializer = IndexSerializer::From(stream);
    instance.position_serializer = VectorSerializer::From(stream);
    instance.normal_serializer = NormalSerializer::From(stream);
    instance.blend_weight_serializer = BlendWeightSerializer::From(stream);
    instance.blend_index_serializer = BlendIndexSerializer::From(stream);
    instance.color_serializer = ColorSerializer::From(stream);
    return instance;
  }

  void invert() {
    inverted = !inverted;
    position_serializer.invert();
  }

  BlendShapeVertex Read(BitStream& stream) const {
    BlendShapeVertex instance{};
    instance.original_offset = original_index_serializer.Read(stream);
    instance.position = position_serializer.Read(stream);
    instance.normal = normal_serializer.Read(stream);
    if (inverted) {
      uint32_t normal_z = instance.normal >> 22;
      normal_z = ~normal_z + 1;
      instance.normal &= ~(0x3FFu << 22);
      instance.normal |= normal_z << 22;
    }
    instance.blend_weight = blend_weight_serializer.Read(stream);
    instance.blend_indices = blend_index_serializer.Read(stream);
    instance.color = color_serializer.Read(stream);
    return instance;
  }
};

BlendShapeVertexPatch BlendShapeVertexPatch::Read(
    BitStream& stream, BlendShapeLoadOptions load_options) {
  using IndexSerializer = ValueSerializer<int32_t>;

  BlendShapeVertexPatch instance{};

  uint32_t vertex_count = stream.Read<uint32_t>();
  assert_true(vertex_count <= 8192);

  instance.total_buffer_size = stream.Read<uint32_t>();
  instance.original_asset_id = ReadAssetId(stream);

  auto vertexSerializer = BlendShapeVertexSerializer::From(stream);
  if (load_options & BlendShapeLoadOption::kInvert) {
    vertexSerializer.invert();
  }
  for (uint32_t i = 0; i < vertex_count; ++i) {
    instance.vertices.push_back(vertexSerializer.Read(stream));
  }

  return instance;
}

bool BlendShape::matches(const AssetId& assetId) const {
  return index_patch.original_asset_id == assetId &&
         vertex_patch.original_asset_id == assetId;
}

std::shared_ptr<BlendShape> BlendShape::Read(
    const uint8_t* data_buffer, size_t data_size,
    BlendShapeLoadOptions load_options) {
  BitStream stream(data_buffer, data_size * 8);

  auto instance = std::make_shared<BlendShape>();

  instance->index_patch = BlendShapeIndexPatch::Read(stream);
  instance->vertex_patch = BlendShapeVertexPatch::Read(stream, load_options);

  stream.AlignToNextByte();

  assert_true(stream.offset_bits() == stream.size_bits());

  return instance;
}

std::shared_ptr<BlendShape> BlendShape::Load(
    const uint8_t* strb_buffer, size_t strb_size,
    BlendShapeLoadOptions load_options) {
  const uint8_t* data_buffer;
  size_t data_size;
  if (!strb::GetSTRBBlock(strb_buffer, strb_size,
                          strb::STRBBlockId::kShapeOverrides, data_buffer,
                          data_size)) {
    return nullptr;
  }

  return Read(data_buffer, data_size, load_options);
}

}  // namespace avatars
}  // namespace xe
