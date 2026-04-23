/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/blend_shape_apply.h"

#include "xenia/avatars/blend_shape.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/model.h"
#include "xenia/base/logging.h"

namespace xe {
namespace avatars {

static void ApplyIndexPatch(const BlendShapeIndexPatch& patch,
                            std::shared_ptr<Model> model) {
  std::vector<size_t> offsets(model->triangle_batches.size() + 1);
  {
    size_t offset = 0;
    size_t i;
    for (i = 0; i < model->triangle_batches.size(); ++i) {
      offsets[i] = offset;
      offset += model->triangle_batches[i].triangle_count;
    }
    offsets[i] = offset;
  }
  for (size_t i = 0; i < patch.indices.size(); ++i) {
    size_t index = patch.indices[i];
    for (size_t j = 1; j <= index; ++j) {
      if (index < offsets[j]) {
        j--;
        index -= offsets[j];
        auto& indices = model->triangle_batches[j].indices;
        int value = indices[index * 3 + 0];
        indices[index * 3 + 1] = value;
        indices[index * 3 + 2] = value;
        break;
      }
    }
  }
}

static void ApplyVertexPatch(const BlendShapeVertexPatch& patch,
                             std::shared_ptr<Model> model) {
  std::vector<size_t> offsets(model->triangle_batches.size() + 1);
  std::vector<size_t> sizes(model->triangle_batches.size());
  {
    size_t offset = 0;
    size_t i;
    for (i = 0; i < model->triangle_batches.size(); ++i) {
      const auto& triangleBatch = model->triangle_batches[i];
      offsets[i] = offset;
      sizes[i] = triangleBatch.vertex_size;
      offset += triangleBatch.vertices.size() * triangleBatch.vertex_size;
    }
    offsets[i] = offset;
  }
  for (size_t i = 0; i < patch.vertices.size(); ++i) {
    const auto& vertexPatch = patch.vertices[i];
    size_t originalOffset = vertexPatch.original_offset;
    for (size_t j = 1; j <= model->triangle_batches.size(); ++j) {
      if (originalOffset < offsets[j]) {
        j--;
        size_t index = (originalOffset - offsets[j]) / sizes[j];
        auto& vertex = model->triangle_batches[j].vertices[index];
        vertex.position = vertexPatch.position;
        vertex.normal = vertexPatch.normal;
        vertex.blend_weight = vertexPatch.blend_weight;
        vertex.blend_indices = vertexPatch.blend_indices;
        vertex.color = vertexPatch.color;
        break;
      }
    }
  }
}

bool ApplyBlendShape(std::shared_ptr<BlendShape> blendShape,
                     const AssetId& modelAssetId,
                     std::shared_ptr<Model> model) {
  if (blendShape->index_patch.original_asset_id != modelAssetId) {
    return false;
  }
  if (blendShape->vertex_patch.original_asset_id != modelAssetId) {
    return false;
  }

  size_t totalIndexBufferSize = 0;
  size_t totalVertexBufferSize = 0;
  for (const auto& batch : model->triangle_batches) {
    totalIndexBufferSize += batch.triangle_count * 6;
    totalVertexBufferSize += batch.vertices.size() * batch.vertex_size;
  }
  totalVertexBufferSize = (totalVertexBufferSize + 127) & ~127;

  if (totalIndexBufferSize != blendShape->index_patch.total_buffer_size) {
    return false;
  }

  if (totalVertexBufferSize != blendShape->vertex_patch.total_buffer_size) {
    return false;
  }

  ApplyIndexPatch(blendShape->index_patch, model);
  ApplyVertexPatch(blendShape->vertex_patch, model);
  return true;
}

}  // namespace avatars
}  // namespace xe
