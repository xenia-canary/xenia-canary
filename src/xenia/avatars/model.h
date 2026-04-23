/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_MODEL_H_
#define XENIA_AVATARS_MODEL_H_

#include <memory>
#include <vector>

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/texture.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

class BitStream;

typedef uint32_t ModelLoadOptions;

namespace ModelLoadOption {

using Option = ModelLoadOptions;

const Option kNone = 0;

const Option kInvert = 1 << 0;

}  // namespace ModelLoadOption

enum class ShaderParameterType : uint32_t {
  kInvalid = 0,
  kTexture = 1,
  kVertexConstant = 2,
  kPixelConstant = 3,
};

struct ShaderParameterTexture {
  uint16_t index;
  uint16_t uv_layer;
  uint16_t flags;
};

struct ShaderParameter {
 public:
  ShaderParameterType type;
  uint32_t usage;
  union {
    ShaderParameterTexture texture;
    float constant_values[4];
  };

 public:
  static ShaderParameter Load(BitStream& stream);
};

struct Vertex {
 public:
  Vector3<float> position;
  uint32_t normal;
  uint32_t blend_weight;
  uint32_t blend_indices;
  uint32_t color;
  std::vector<Vector2<uint16_t>> uvs;
};

struct TriangleBatch {
 public:
  uint32_t shader_id;
  std::vector<ShaderParameter> shader_parameters;
  uint32_t triangle_count;
  uint32_t uv_count;
  uint32_t vertex_size;
  uint32_t index_size;
  uint32_t vertex_array_offset;
  uint32_t index_array_offset;

  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;

 public:
  static TriangleBatch Read(BitStream& stream, ModelLoadOptions load_options);

 private:
  static std::vector<Vertex> ReadVertices(BitStream& stream,
                                          size_t vertex_count, size_t uv_count,
                                          ModelLoadOptions load_options);
  static std::vector<uint16_t> ReadIndices(BitStream& stream,
                                           size_t triangle_count);
};

struct ModelTexture {
 public:
  uint32_t gpu_offset;
  uint32_t gpu_size;
  Texture texture;

 public:
  static ModelTexture Read(BitStream& stream);
};

class Model {
 public:
  uint32_t cpu_size;
  uint32_t gpu_size;
  uint32_t texture_buffer_size;
  uint32_t vertex_buffer_size;
  uint32_t index_buffer_size;
  uint32_t vertex_buffer_offset;
  uint32_t index_buffer_offset;
  uint32_t triangle_batch_array_offset;
  uint32_t texture_array_offset;
  uint32_t texture_scratch_size;

  std::vector<TriangleBatch> triangle_batches;
  std::vector<ModelTexture> textures;

 public:
  static std::shared_ptr<Model> Load(const uint8_t* strb_buffer,
                                     size_t strb_size,
                                     ModelLoadOptions load_options);

 private:
  static std::shared_ptr<Model> Read(const uint8_t* data_buffer,
                                     size_t data_size,
                                     ModelLoadOptions load_options);
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_MODEL_H_
