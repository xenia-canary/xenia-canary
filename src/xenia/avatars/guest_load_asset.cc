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

#include "xenia/avatars/guest_load_asset.h"

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/blend_shape.h"
#include "xenia/avatars/blend_shape_apply.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/guest_animation.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/guest_load_animation.h"
#include "xenia/avatars/memory_block.h"
#include "xenia/avatars/model.h"
#include "xenia/avatars/model_save.h"
#include "xenia/avatars/prop.h"
#include "xenia/avatars/skeleton.h"
#include "xenia/avatars/skeleton_data.h"
#include "xenia/avatars/skeleton_scaling.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace avatars {

static BodyType GetBodyType(const X_AVATAR_METADATA& metadata) {
  const AssetId male_body_asset_id = {
      2, 0, 1, {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  const AssetId female_body_asset_id = {
      2, 0, 2, {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  if (metadata.body_component.asset_id == male_body_asset_id) {
    return BodyType::kMale;
  }
  if (metadata.body_component.asset_id == female_body_asset_id) {
    return BodyType::kFemale;
  }
  return BodyType::kUnknown;
}

static bool LoadFile(std::filesystem::path path, std::vector<uint8_t>& buffer) {
  bool was_loaded = false;
  auto handle = xe::filesystem::OpenFile(path, "rb");
  if (handle != nullptr) {
    fseek(handle, 0, SEEK_END);
    buffer.resize(ftell(handle));
    fseek(handle, 0, SEEK_SET);
    size_t bytes_read = fread(buffer.data(), 1, buffer.size(), handle);
    if (bytes_read == buffer.size()) {
      was_loaded = true;
    }
    fclose(handle);
  }
  return was_loaded;
}

static bool LoadAsset(AssetPack* asset_pack, const AssetId& asset_id,
                      const uint8_t*& buffer, size_t& size,
                      std::vector<uint8_t>& temp) {
  if (!asset_pack->GetAssetData(asset_id, buffer, size)) {
    // TODO(gibbed): load from user data path
    std::filesystem::path bin_path =
        fmt::format("avatar_blobs\\{}.bin", asset_id.to_string());
    if (!LoadFile(bin_path, temp)) {
      return false;
    }
    buffer = temp.data();
    size = temp.size();
  }
  return true;
}

template <typename Type>
static std::shared_ptr<Type> LoadAsset(AssetPack* asset_pack,
                                       const AssetId& asset_id) {
  const uint8_t* strb_buffer;
  size_t strb_size;
  std::vector<uint8_t> strb_bytes;
  if (!LoadAsset(asset_pack, asset_id, strb_buffer, strb_size, strb_bytes)) {
    return nullptr;
  }
  return Type::Load(strb_buffer, strb_size);
}

template <typename Type, typename Options>
static std::shared_ptr<Type> LoadAsset(AssetPack* asset_pack,
                                       const AssetId& asset_id,
                                       Options load_options) {
  const uint8_t* strb_buffer;
  size_t strb_size;
  std::vector<uint8_t> strb_bytes;
  if (!LoadAsset(asset_pack, asset_id, strb_buffer, strb_size, strb_bytes)) {
    return nullptr;
  }
  return Type::Load(strb_buffer, strb_size, load_options);
}

struct ShaderParameterOverride {
  uint32_t usage;
  float x;
  float y;
  float z;
  float w;
};

static bool VertexToGuest(X_AVATAR_VERTEX* guest, const Vertex& host) {
  guest->position.x = host.position.x;
  guest->position.y = host.position.y;
  guest->position.z = host.position.z;
  guest->normal = host.normal;
  guest->blend_weight = host.blend_weight;
  guest->blend_indices = host.blend_indices;
  guest->color = host.color;
  for (size_t i = 0; i < host.uvs.size(); ++i) {
    auto& guest_uv = guest->uvs[i];
    const auto& host_uv = host.uvs[i];
    guest_uv.u = host_uv.x;
    guest_uv.v = host_uv.y;
  }
  return true;
}

static void OverrideShaderParameter(
    X_AVATAR_SHADER_PARAM* parameters,
    const ShaderParameterOverride& parameter_override) {
  for (size_t i = 0, o = 19; i < 20; ++i, --o) {
    auto& parameter = parameters[o];
    if (parameter.usage != parameter_override.usage) {
      continue;
    }
    assert_true(
        parameter.type == uint32_t(ShaderParameterType::kPixelConstant) ||
        parameter.type == uint32_t(ShaderParameterType::kVertexConstant));
    parameter.constant_values[0] = parameter_override.x;
    parameter.constant_values[1] = parameter_override.y;
    parameter.constant_values[2] = parameter_override.z;
    parameter.constant_values[3] = parameter_override.w;
  }
}

static bool TriangleBatchToGuest(
    X_AVATAR_TRIANGLE_BATCH& guest, const TriangleBatch& host,
    MemoryBlock* cpu_memory, uint8_t* cpu_buffer, MemoryBlock* gpu_memory,
    uint8_t* gpu_buffer, uint32_t gpu_buffer_base_ptr,
    const std::vector<ShaderParameterOverride>& shader_parameter_overrides) {
  guest.shader_id = host.shader_id;
  std::memset(guest.shader_parameters, 0, sizeof(guest.shader_parameters));
  for (size_t i = 0; i < host.shader_parameters.size(); ++i) {
    auto& guest_parameter = guest.shader_parameters[i];
    const auto& host_parameter = host.shader_parameters[i];
    guest_parameter.type = static_cast<uint32_t>(host_parameter.type);
    guest_parameter.usage = host_parameter.usage;
    if (host_parameter.type == ShaderParameterType::kTexture) {
      guest_parameter.texture.index = host_parameter.texture.index;
      guest_parameter.texture.uv_index = host_parameter.texture.uv_layer;
      guest_parameter.texture.flags = host_parameter.texture.flags;
    } else {
      for (size_t j = 0; j < 4; ++j) {
        guest_parameter.constant_values[j] = host_parameter.constant_values[j];
      }
    }
  }

  for (const auto& shader_parameter_override : shader_parameter_overrides) {
    OverrideShaderParameter(guest.shader_parameters, shader_parameter_override);
  }

  guest.triangle_count = host.triangle_count;
  guest.vertex_count = static_cast<uint32_t>(host.vertices.size());
  guest.uv_count = host.uv_count;
  guest.vertex_size = host.vertex_size;
  guest.index_size = host.index_size;
  gpu_memory->SetPointer(&guest.vertices_ptr,
                         gpu_buffer_base_ptr + host.vertex_array_offset);
  gpu_memory->SetPointer(&guest.indices_ptr,
                         gpu_buffer_base_ptr + host.index_array_offset);
  uint32_t vertex_offset = host.vertex_array_offset;
  for (size_t i = 0; i < host.vertices.size(); ++i) {
    if (!VertexToGuest(
            reinterpret_cast<X_AVATAR_VERTEX*>(&gpu_buffer[vertex_offset]),
            host.vertices[i])) {
      return false;
    }
    vertex_offset += guest.vertex_size;
  }
  uint32_t index_offset = host.index_array_offset;
  for (size_t i = 0; i < host.indices.size(); ++i) {
    auto guest_index =
        reinterpret_cast<be<uint16_t>*>(&gpu_buffer[index_offset]);
    *guest_index = host.indices[i];
    index_offset += guest.index_size;
  }
  return true;
}

static bool TextureToGuest(X_AVATAR_TEXTURE& guest, const ModelTexture& host,
                           MemoryBlock* cpu_memory, uint8_t* cpu_buffer,
                           MemoryBlock* gpu_memory, uint8_t* gpu_buffer,
                           uint32_t gpu_buffer_base_ptr) {
  auto format = host.texture.format;
  format &= ~0x00000100u;
  guest.format = format;
  guest.width = host.texture.width;
  guest.height = host.texture.height;
  guest.total_base_size = host.texture.total_data_size;
  guest.total_mip_size = 0;
  guest.base_size = host.texture.data_size;
  guest.mip_size = 0;
  guest.mip_levels = 1;
  guest.layer_count = host.texture.layer_count;
  gpu_memory->SetPointer(&guest.base_data_ptr,
                         gpu_buffer_base_ptr + host.gpu_offset);
  guest.mip_data_ptr = 0;

  auto data_buffer = &gpu_buffer[host.gpu_offset];
  if (!host.texture.is_empty) {
    uint32_t output_align = (host.texture.format == 0x1A200152u ||
                             host.texture.format == 0x1A200052u)
                                ? 256u
                                : 512u;
    size_t input_stride = host.texture.data_stride;
    size_t output_stride = align(host.texture.data_stride, output_align);
    size_t input_offset = 0;
    size_t output_offset = 0;
    assert_true(host.texture.data_rows * output_stride <= host.gpu_size);
    for (size_t layer = 0; layer < host.texture.layer_count; ++layer) {
      for (size_t y = 0; y < host.texture.data_rows; ++y) {
        std::memcpy(&data_buffer[output_offset],
                    &host.texture.data_bytes.data()[input_offset],
                    input_stride);
        input_offset += input_stride;
        output_offset += output_stride;
      }
    }
  }
  return true;
}

static bool ModelToGuest(
    std::shared_ptr<Model> host_model, X_AVATAR_MODEL* guest_model,
    MemoryBlock* cpu_memory, MemoryBlock* gpu_memory, uint32_t category_mask,
    std::shared_ptr<Texture> replacement_textures[6],
    const std::vector<ShaderParameterOverride>& shader_parameter_overrides) {
  if (host_model == nullptr) {
    return false;
  }

  uint32_t cpu_buffer_ptr, gpu_buffer_ptr;
  uint8_t* cpu_buffer =
      cpu_memory->ClaimBytes(&cpu_buffer_ptr, host_model->cpu_size);
  uint8_t* gpu_buffer =
      gpu_memory->ClaimBytes(&gpu_buffer_ptr, host_model->gpu_size);

  *guest_model = {};
  guest_model->cpu_size = host_model->cpu_size;
  guest_model->gpu_size = host_model->gpu_size;
  guest_model->texture_size = host_model->texture_buffer_size;
  guest_model->vertex_size = host_model->vertex_buffer_size;
  guest_model->index_size = host_model->index_buffer_size;
  guest_model->triangle_batch_count =
      static_cast<uint32_t>(host_model->triangle_batches.size());
  guest_model->texture_count =
      static_cast<uint32_t>(host_model->textures.size());
  cpu_memory->SetPointer(&guest_model->cpu_buffer_ptr, cpu_buffer_ptr);
  gpu_memory->SetPointer(&guest_model->gpu_buffer_ptr, gpu_buffer_ptr);
  gpu_memory->SetPointer(&guest_model->vertex_buffer_ptr,
                         gpu_buffer_ptr + host_model->vertex_buffer_offset);
  gpu_memory->SetPointer(&guest_model->index_buffer_ptr,
                         gpu_buffer_ptr + host_model->index_buffer_offset);
  cpu_memory->SetPointer(
      &guest_model->triangle_batches_ptr,
      cpu_buffer_ptr + host_model->triangle_batch_array_offset);
  cpu_memory->SetPointer(&guest_model->textures_ptr,
                         cpu_buffer_ptr + host_model->texture_array_offset);

  auto guest_triangle_batches = reinterpret_cast<X_AVATAR_TRIANGLE_BATCH*>(
      &cpu_buffer[host_model->triangle_batch_array_offset]);
  for (size_t i = 0; i < guest_model->triangle_batch_count; ++i) {
    if (!TriangleBatchToGuest(guest_triangle_batches[i],
                              host_model->triangle_batches[i], cpu_memory,
                              cpu_buffer, gpu_memory, gpu_buffer,
                              gpu_buffer_ptr, shader_parameter_overrides)) {
      return false;
    }
  }

  // override head textures with those from the metadata
  if (category_mask == ComponentCategory::kHead) {
    int usage_indices[20];
    for (size_t i = 0; i < 20; ++i) {
      usage_indices[i] = -1;
    }
    const int usage_to_replacement_texture_indices[] = {
        -1, -1, -1, -1, -1, 5, 3, 2, 2, 1, 1, 4, 0,
    };
    std::vector<int> replacement_texture_indices(guest_model->texture_count);
    for (size_t i = 0; i < guest_model->triangle_batch_count; ++i) {
      const auto& triangle_batch = host_model->triangle_batches[i];
      for (const auto& shader_parameter : triangle_batch.shader_parameters) {
        if (shader_parameter.usage >= 20 ||
            shader_parameter.type != ShaderParameterType::kTexture) {
          continue;
        }
        const auto& usage = shader_parameter.usage;
        const auto& texture_index = shader_parameter.texture.index;
        assert_true(usage_indices[usage] == -1 ||
                    usage_indices[usage] == texture_index);
        usage_indices[usage] = texture_index;
        replacement_texture_indices[texture_index] =
            usage < countof(usage_to_replacement_texture_indices)
                ? usage_to_replacement_texture_indices[usage]
                : -1;
      }
    }

    auto guest_textures = reinterpret_cast<X_AVATAR_TEXTURE*>(
        &cpu_buffer[host_model->texture_array_offset]);
    for (size_t i = 0; i < guest_model->texture_count; ++i) {
      ModelTexture model_texture;
      auto replacement_texture_index = replacement_texture_indices[i];
      if (replacement_texture_index >= 0 &&
          replacement_textures[replacement_texture_index] != nullptr) {
        model_texture.gpu_offset = host_model->textures[i].gpu_offset;
        model_texture.gpu_size = host_model->textures[i].gpu_size;
        model_texture.texture =
            *replacement_textures[replacement_texture_index];
      } else {
        model_texture = host_model->textures[i];
      }
      if (!TextureToGuest(guest_textures[i], model_texture, cpu_memory,
                          cpu_buffer, gpu_memory, gpu_buffer, gpu_buffer_ptr)) {
        return false;
      }
    }
  } else {
    auto guest_textures = reinterpret_cast<X_AVATAR_TEXTURE*>(
        &cpu_buffer[host_model->texture_array_offset]);
    for (size_t i = 0; i < guest_model->texture_count; ++i) {
      if (!TextureToGuest(guest_textures[i], host_model->textures[i],
                          cpu_memory, cpu_buffer, gpu_memory, gpu_buffer,
                          gpu_buffer_ptr)) {
        return false;
      }
    }
  }

  return true;
}

static bool SkeletonToGuest(X_AVATAR_SKELETON* guest,
                            std::shared_ptr<Skeleton> host,
                            MemoryBlock* cpu_memory) {
  assert_true(host->joints.size() <= 72);
  uint8_t joint_count = static_cast<uint8_t>(host->joints.size());

  *guest = {};
  guest->joint_count = joint_count;

  const uint8_t invalid_index = 255;

  uint32_t guest_joints_ptr;
  auto guest_joints = cpu_memory->Claim<X_AVATAR_SKELETON_JOINT>(
      &guest_joints_ptr, joint_count);
  cpu_memory->SetPointer(&guest->joints_ptr, guest_joints_ptr);

  for (uint8_t i = 0; i < joint_count; ++i) {
    auto& guest_joint = guest_joints[i];
    const auto& host_joint = host->joints[i];
    guest_joint.parent_index = host_joint.parent_index;
    guest_joint.first_child_index = host_joint.first_child_index;
    guest_joint.next_index = host_joint.next_index;
    guest_joint.bindpose.position.x = host_joint.bindpose.position.x;
    guest_joint.bindpose.position.y = host_joint.bindpose.position.y;
    guest_joint.bindpose.position.z = host_joint.bindpose.position.z;
    guest_joint.bindpose.position.w = 1.f;
    guest_joint.bindpose.rotation.x = host_joint.bindpose.rotation.x;
    guest_joint.bindpose.rotation.y = host_joint.bindpose.rotation.y;
    guest_joint.bindpose.rotation.z = host_joint.bindpose.rotation.z;
    guest_joint.bindpose.rotation.w = host_joint.bindpose.rotation.w;
    guest_joint.pose.position.x = host_joint.pose.position.x;
    guest_joint.pose.position.y = host_joint.pose.position.y;
    guest_joint.pose.position.z = host_joint.pose.position.z;
    guest_joint.pose.position.w = 1.f;
    guest_joint.pose.rotation.x = host_joint.pose.rotation.x;
    guest_joint.pose.rotation.y = host_joint.pose.rotation.y;
    guest_joint.pose.rotation.z = host_joint.pose.rotation.z;
    guest_joint.pose.rotation.w = host_joint.pose.rotation.w;
    guest_joint.pose.scale.x = host_joint.pose.scale.x;
    guest_joint.pose.scale.y = host_joint.pose.scale.y;
    guest_joint.pose.scale.z = host_joint.pose.scale.z;
    guest_joint.pose.scale.w = 1.f;
  }

  return true;
}

static void GetShaderOverrides(
    const X_AVATAR_METADATA& metadata, uint32_t category_mask,
    std::vector<ShaderParameterOverride>& shader_parameter_overrides) {
  // skin color
  if (category_mask & ComponentCategory::kBody) {
    auto color = metadata.colors[0];
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 22;
    shader_parameter_override.x = ((color >> 16) & 0xFF) / 255.f;
    shader_parameter_override.y = ((color >> 8) & 0xFF) / 255.f;
    shader_parameter_override.z = ((color >> 0) & 0xFF) / 255.f;
    shader_parameter_override.w = ((color >> 24) & 0xFF) / 255.f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  }
  // hair color
  else if (category_mask & ComponentCategory::kHair) {
    auto color = metadata.colors[1];
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 22;
    shader_parameter_override.x = ((color >> 16) & 0xFF) / 255.f;
    shader_parameter_override.y = ((color >> 8) & 0xFF) / 255.f;
    shader_parameter_override.z = ((color >> 0) & 0xFF) / 255.f;
    shader_parameter_override.w = ((color >> 24) & 0xFF) / 255.f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  } else {
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 22;
    shader_parameter_override.x = 1.f;
    shader_parameter_override.y = 1.f;
    shader_parameter_override.z = 1.f;
    shader_parameter_override.w = 1.f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  }

  if (category_mask & ComponentCategory::kHead) {
    for (uint32_t i = 0, usage = 13; i < 9; ++i, ++usage) {
      auto color = metadata.colors[i];
      ShaderParameterOverride shader_parameter_override;
      shader_parameter_override.usage = usage;
      shader_parameter_override.x = ((color >> 16) & 0xFF) / 255.f;
      shader_parameter_override.y = ((color >> 8) & 0xFF) / 255.f;
      shader_parameter_override.z = ((color >> 0) & 0xFF) / 255.f;
      shader_parameter_override.w = ((color >> 24) & 0xFF) / 255.f;
      shader_parameter_overrides.push_back(shader_parameter_override);
    }
  }

  // rim light color
  {
    auto color = metadata.colors[0];
    ShaderParameterOverride shader_parameter_override;
    shader_parameter_override.usage = 27;
    // TODO(gibbed): calculate rim light color properly
    shader_parameter_override.x = 0.54901963f;
    shader_parameter_override.y = 0.5019608f;
    shader_parameter_override.z = 0.40392157f;
    shader_parameter_override.w = 1.6f;
    shader_parameter_overrides.push_back(shader_parameter_override);
  }
}

bool LoadAssetsToGuest(const X_AVATAR_METADATA& metadata,
                       uint32_t category_mask, uint32_t flags,
                       AssetPack* asset_pack, MemoryBlock* cpu_memory,
                       MemoryBlock* gpu_memory, uint32_t skeleton_version,
                       uint32_t coordinate_system) {
  bool want_prop = !!(category_mask & ComponentCategory::kProp);

  category_mask &= ~ComponentCategory::kProp;

  BlendShapeLoadOptions blend_shape_load_options = BlendShapeLoadOption::kNone;
  SkeletonLoadOptions skeleton_load_options = SkeletonLoadOption::kNone;
  ModelLoadOptions model_load_options = ModelLoadOption::kNone;
  if (coordinate_system == 0) {
    blend_shape_load_options |= BlendShapeLoadOption::kInvert;
    skeleton_load_options |= SkeletonLoadOption::kInvert;
    model_load_options |= ModelLoadOption::kInvert;
  }

  BodyType body_type = GetBodyType(metadata);

  auto skeleton = LoadSkeleton(skeleton_version, skeleton_load_options);
  if (skeleton == nullptr) {
    XELOGW("Failed to load avatar skeleton version {}!", skeleton_version);
    return false;
  }

  if (skeleton_version == 1) {
    ApplyScalesToSkeletonV1(body_type, metadata.weight_factor,
                            metadata.height_factor, skeleton);
  } else if (skeleton_version == 2) {
    ApplyScalesToSkeletonV2(body_type, metadata.weight_factor,
                            metadata.height_factor, skeleton);
  } else {
    XELOGW("Unknown avatar skeleton version {}!", skeleton_version);
    return false;
  }

  size_t blend_shape_failures = 0;
  std::vector<std::pair<AssetId, std::shared_ptr<BlendShape>>> blend_shapes;
  for (size_t i = 0; i < 6; ++i) {
    const auto& asset_id = metadata.blend_shapes[i].asset_id;
    if (asset_id.is_zero()) {
      continue;
    }
    auto blend_shape = LoadAsset<BlendShape, BlendShapeLoadOptions>(
        asset_pack, asset_id, blend_shape_load_options);
    if (blend_shape == nullptr) {
      XELOGE("Failed to load avatar blend shape {}!", asset_id.to_string());
      blend_shape_failures++;
      continue;
    }
    blend_shapes.push_back({asset_id, blend_shape});
  }

  std::vector<X_AVATAR_COMPONENT_INFO> source_component_infos;
  if (metadata.body_component.matches(category_mask)) {
    source_component_infos.push_back(metadata.body_component);
  }
  if (metadata.head_component.matches(category_mask)) {
    source_component_infos.push_back(metadata.head_component);
  }
  for (const auto& component : metadata.components) {
    if (component.matches(category_mask)) {
      source_component_infos.push_back(component);
    }
  }

  size_t component_failures = 0;
  std::vector<std::pair<X_AVATAR_COMPONENT_INFO, std::shared_ptr<Model>>>
      source_components;
  for (const auto& source_info : source_component_infos) {
    auto model = LoadAsset<Model, ModelLoadOptions>(
        asset_pack, source_info.asset_id, model_load_options);
    if (model != nullptr) {
      // SaveModel(source_info, model, asset_pack);
      source_components.push_back({source_info, model});
      continue;
    }
    XELOGE("Failed to load avatar asset {}, looking for fallback...",
           source_info.asset_id.to_string());
    X_AVATAR_COMPONENT_INFO fallback_info{};
    for (const auto& candidate_info : metadata.fallback_components) {
      if (candidate_info.categories == source_info.categories) {
        // TODO(gibbed): if this fails... fall back even further?
        model = LoadAsset<Model, ModelLoadOptions>(
            asset_pack, candidate_info.asset_id, model_load_options);
        fallback_info = candidate_info;
        break;
      }
    }
    if (model == nullptr) {
      if (!fallback_info.asset_id.is_zero()) {
        XELOGE("Failed to load fallback avatar asset {}!",
               fallback_info.asset_id.to_string());
        component_failures++;
      }
      continue;
    }
    source_components.push_back({fallback_info, model});
  }

  size_t replacement_texture_failures = 0;
  std::shared_ptr<Texture> replacement_textures[6];
  for (size_t i = 0; i < 6; ++i) {
    const auto& texture_info = metadata.textures[i];
    if (texture_info.asset_id.is_zero()) {
      continue;
    }
    auto texture = LoadAsset<Texture>(asset_pack, texture_info.asset_id);
    if (texture == nullptr) {
      XELOGE("Failed to load avatar replacement texture {}!",
             texture_info.asset_id.to_string());
      replacement_texture_failures++;
      continue;
    }
    replacement_textures[i] = texture;
  }

  for (const auto& source_component : source_components) {
    for (const auto& blend_shape : blend_shapes) {
      if (!blend_shape.second->matches(source_component.first.asset_id)) {
        continue;
      }
      if (!ApplyBlendShape(blend_shape.second, source_component.first.asset_id,
                           source_component.second)) {
        XELOGE("Failed to apply blend shape {} to asset {}!",
               blend_shape.first.to_string(),
               source_component.first.asset_id.to_string());
      }
      XELOGE("Applied blend shape {} to asset {}.",
             blend_shape.first.to_string(),
             source_component.first.asset_id.to_string());
    }
  }

  std::shared_ptr<Prop> prop = nullptr;
  X_AVATAR_COMPONENT_INFO prop_info{};
  if (want_prop) {
    for (const auto& component : metadata.components) {
      if (component.matches(ComponentCategory::kProp)) {
        prop_info = component;
        break;
      }
    }
    if (!prop_info.asset_id.is_zero()) {
      PropLoadOptions prop_load_options{};
      prop_load_options.model = model_load_options;
      prop_load_options.skeleton = skeleton_load_options;
      prop_load_options.animation = AnimationLoadOption::kGuest;
      prop_load_options.blend_shape = blend_shape_load_options;
      if (coordinate_system == 0) {
        prop_load_options.animation |= AnimationLoadOption::kInvert;
      }
      prop_load_options.model = model_load_options;
      prop = LoadAsset<Prop, PropLoadOptions>(asset_pack, prop_info.asset_id,
                                              prop_load_options);
    }
  }

  auto assets = cpu_memory->Claim<X_AVATAR_ASSETS>();
  *assets = {};

  uint32_t guest_skeleton_ptr;
  auto guest_skeleton =
      cpu_memory->Claim<X_AVATAR_SKELETON>(&guest_skeleton_ptr);

  if (!SkeletonToGuest(guest_skeleton, skeleton, cpu_memory)) {
    return false;
  }

  cpu_memory->SetPointer(&assets->skeleton_ptr, guest_skeleton_ptr);

  if (source_components.size()) {
    uint32_t component_infos_ptr;
    auto component_infos = cpu_memory->Claim<X_AVATAR_COMPONENT_INFO>(
        &component_infos_ptr, source_components.size());
    uint32_t component_models_ptr;
    auto component_models = cpu_memory->Claim<X_AVATAR_MODEL>(
        &component_models_ptr, source_components.size());

    assets->component_count = uint32_t(source_components.size());

    cpu_memory->SetPointer(&assets->component_infos_ptr, component_infos_ptr);
    cpu_memory->SetPointer(&assets->component_models_ptr, component_models_ptr);

    auto component_info = component_infos;
    auto component_model = component_models;
    for (const auto& source_component : source_components) {
      *component_info = source_component.first;

      std::vector<ShaderParameterOverride> shader_parameter_overrides;
      GetShaderOverrides(metadata, source_component.first.categories,
                         shader_parameter_overrides);

      ModelToGuest(source_component.second, component_model, cpu_memory,
                   gpu_memory, source_component.first.categories,
                   replacement_textures, shader_parameter_overrides);
      component_info++;
      component_model++;
    }
  } else {
    assets->component_count = 0;
  }

  if (prop != nullptr) {
    uint32_t guest_prop_ptr;
    auto guest_prop = cpu_memory->Claim<X_AVATAR_PROP>(&guest_prop_ptr);

    *guest_prop = {};
    guest_prop->component_info = prop_info;

    uint32_t guest_prop_skeleton_ptr;
    auto guest_prop_skeleton =
        cpu_memory->Claim<X_AVATAR_SKELETON>(&guest_prop_skeleton_ptr);

    if (!SkeletonToGuest(guest_prop_skeleton, prop->skeleton, cpu_memory)) {
      return false;
    }

    cpu_memory->SetPointer(&guest_prop->skeleton_ptr, guest_prop_skeleton_ptr);

    if (!ModelToGuest(prop->model, &guest_prop->component_model, cpu_memory,
                      gpu_memory, prop_info.categories, {}, {})) {
      return false;
    }

    if (prop->animation != nullptr) {
      uint32_t guest_animation_ptr;
      auto guest_animation =
          cpu_memory->Claim<X_AVATAR_ANIMATION>(&guest_animation_ptr);

      guest_animation->compressed_data_size =
          static_cast<uint32_t>(prop->animation->compressed_data_bytes.size());

      uint32_t guest_compressed_data_buffer_ptr;
      auto guest_compressed_data_buffer =
          cpu_memory->ClaimBytes(&guest_compressed_data_buffer_ptr,
                                 guest_animation->compressed_data_size);

      if (!LoadAnimationToGuest(prop_info.asset_id, prop->animation,
                                guest_animation,
                                guest_compressed_data_buffer)) {
        return false;
      }

      cpu_memory->SetPointer(&guest_animation->compressed_data_buffer_ptr,
                             guest_compressed_data_buffer_ptr);

      cpu_memory->SetPointer(&guest_prop->animation_ptr, guest_animation_ptr);
    }

    cpu_memory->SetPointer(&assets->prop_ptr, guest_prop_ptr);
  }

  return true;
}

}  // namespace avatars
}  // namespace xe
