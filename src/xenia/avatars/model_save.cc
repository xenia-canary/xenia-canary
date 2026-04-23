/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>
#include <stack>

#include "xenia/avatars/model_save.h"

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/model.h"
#include "xenia/base/logging.h"

namespace xe {
namespace avatars {

void SaveModel(const X_AVATAR_COMPONENT_INFO& component_info,
               std::shared_ptr<Model> model, AssetPack* asset_pack) {
  if (model == nullptr) {
    return;
  }

  auto asset_id = component_info.asset_id.to_string();
  auto asset_name =
      xe::to_utf8(asset_pack->GetAssetName(component_info.asset_id));

  size_t batch_index = 0;
  for (const auto& triangle_batch : model->triangle_batches) {
    std::string output_name;
    if (!asset_name.size()) {
      output_name = fmt::format("avatars\\{}_{}.obj", asset_id, batch_index++);
    } else {
      output_name = fmt::format("avatars\\{}_{}_{}.obj", asset_id, asset_name,
                                batch_index++);
    }
    FILE* output = fopen(output_name.c_str(), "wb");
    if (output != nullptr) {
      continue;
    }

    for (const auto& vertex : triangle_batch.vertices) {
      auto vertex_line = fmt::format("v {} {} {}\r\n", vertex.position.x,
                                     vertex.position.y, vertex.position.z);
      fwrite(vertex_line.c_str(), 1, vertex_line.size(), output);
    }

    for (size_t i = 0; i < triangle_batch.indices.size(); i += 3) {
      auto index0 = 1 + triangle_batch.indices[i + 0];
      auto index1 = 1 + triangle_batch.indices[i + 1];
      auto index2 = 1 + triangle_batch.indices[i + 2];
      auto line = fmt::format("f {} {} {}\r\n", index0, index1, index2);
      fwrite(line.c_str(), 1, line.size(), output);
    }

    fflush(output);
    fclose(output);
  }

  size_t texture_index = 0;
  for (const auto& model_texture : model->textures) {
    const auto& texture = model_texture.texture;
    struct {
      uint32_t size;
      uint32_t flags;
      uint32_t height;
      uint32_t width;
      uint32_t pitch_or_linear_size;
      uint32_t depth;
      uint32_t mip_levels;
      uint32_t reserved1[11];
      struct {
        uint32_t size;
        uint32_t flags;
        be<fourcc_t> fourcc;
        uint32_t rgb_bit_count;
        uint32_t r_bit_mask;
        uint32_t g_bit_mask;
        uint32_t b_bit_mask;
        uint32_t a_bit_mask;
      } pixel_format;
      uint32_t caps[4];
      uint32_t reserved2;
    } dds_header;

    auto format = texture.format & 0x3F;

    std::memset(&dds_header, 0, sizeof(dds_header));
    dds_header.size = sizeof(dds_header);
    dds_header.flags = 1u | 2u | 4u | 0x1000u | 0x20000u;
    if ((format >= 18 && format <= 20) || format == 49 ||
        (format >= 58 && format <= 60)) {
      dds_header.flags |= 0x80000u;
    } else {
      dds_header.flags |= 0x8u;
    }
    dds_header.height = texture.height;
    dds_header.width = texture.width;
    dds_header.pitch_or_linear_size = texture.data_stride;
    dds_header.mip_levels = 1;

    dds_header.pixel_format.size = sizeof(dds_header.pixel_format);
    switch (format) {
      case 18: {
        dds_header.pixel_format.flags = 0x4u;
        dds_header.pixel_format.fourcc = make_fourcc("DXT1");
        break;
      }
      case 19: {
        dds_header.pixel_format.flags = 0x4u;
        dds_header.pixel_format.fourcc = make_fourcc("DXT3");
        break;
      }
      case 20: {
        dds_header.pixel_format.flags = 0x4u;
        dds_header.pixel_format.fourcc = make_fourcc("DXT5");
        break;
      }
      case 6: {
        dds_header.pixel_format.flags = 0x1u | 0x40u;
        dds_header.pixel_format.rgb_bit_count = 32;
        dds_header.pixel_format.r_bit_mask = 0x00FF0000u;
        dds_header.pixel_format.g_bit_mask = 0x0000FF00u;
        dds_header.pixel_format.b_bit_mask = 0x000000FFu;
        dds_header.pixel_format.a_bit_mask = 0xFF000000u;
        break;
      }
      default: {
        assert_unhandled_case(src.format);
        std::memset(&dds_header.pixel_format, 0xCD,
                    sizeof(dds_header.pixel_format));
        XELOGW("Skipping {} for texture dump.", format);
        texture_index++;
        continue;
      }
    }

    std::string output_name;
    if (!asset_name.size()) {
      output_name = fmt::format("avatars\\{}_{}_{:X}.dds", asset_id,
                                texture_index++, texture.format);
    } else {
      output_name = fmt::format("avatars\\{}_{}_{}_{:X}.dds", asset_id,
                                asset_name, texture_index++, texture.format);
    }
    FILE* output = fopen(output_name.c_str(), "wb");
    if (output == nullptr) {
      continue;
    }
    const char signature[4] = {'D', 'D', 'S', ' '};
    fwrite(&signature, sizeof(signature), 1, output);
    fwrite(&dds_header, sizeof(dds_header), 1, output);

    std::vector<uint8_t> data_bytes(texture.data_bytes);
    for (size_t i = 0; i < data_bytes.size(); i += 2) {
      auto b = data_bytes[i];
      data_bytes[i] = data_bytes[i + 1];
      data_bytes[i + 1] = b;
    }

    fwrite(data_bytes.data(), 1, data_bytes.size(), output);
    fclose(output);
  }
}

}  // namespace avatars
}  // namespace xe
