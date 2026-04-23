/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_GUEST_ASSET_H_
#define XENIA_AVATARS_GUEST_ASSET_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/avatars/asset_pack.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_key.h"
#include "xenia/base/string_util.h"
#include "xenia/xbox.h"

namespace xe {
namespace avatars {

enum class BodyType : uint32_t {
  kUnknown = 0,
  kMale = 1,
  kFemale = 2,
};

typedef uint32_t ComponentCategories;

namespace ComponentCategory {

const ComponentCategories kNone = 0;             // 0x00000000
const ComponentCategories kHead = 1 << 0;        // 0x00000001
const ComponentCategories kBody = 1 << 1;        // 0x00000002
const ComponentCategories kHair = 1 << 2;        // 0x00000004
const ComponentCategories kTop = 1 << 3;         // 0x00000008
const ComponentCategories kBottom = 1 << 4;      // 0x00000010
const ComponentCategories kShoes = 1 << 5;       // 0x00000020
const ComponentCategories kHat = 1 << 6;         // 0x00000040
const ComponentCategories kGloves = 1 << 7;      // 0x00000080
const ComponentCategories kGlasses = 1 << 8;     // 0x00000100
const ComponentCategories kWristwear = 1 << 9;   // 0x00000200
const ComponentCategories kEarrings = 1 << 10;   // 0x00000400
const ComponentCategories kRing = 1 << 11;       // 0x00000800
const ComponentCategories kProp = 1 << 12;       // 0x00001000
const ComponentCategories kAnimation = 1 << 22;  // 0x00400000

}  // namespace ComponentCategory

struct X_AVATAR_BLEND_SHAPE {
  AssetId asset_id;
};
static_assert_size(X_AVATAR_BLEND_SHAPE, 16);

struct X_AVATAR_METADATA_TEXTURE {
  AssetId asset_id;
  be<float> scale;
  be<float> rotation;
  be<float> translation[2];
};
static_assert_size(X_AVATAR_METADATA_TEXTURE, 32);

struct X_AVATAR_COMPONENT_INFO {
  AssetId asset_id;
  be<uint16_t> categories;
  be<uint8_t> maybe_padding[14];

  bool matches(uint32_t category_mask) const {
    return !asset_id.is_zero() && !!(categories & category_mask);
  }
};
static_assert_size(X_AVATAR_COMPONENT_INFO, 32);

struct X_AVATAR_METADATA {
  be<uint32_t> version;                            // 000
  be<float> weight_factor;                         // 004
  be<float> height_factor;                         // 008
  X_AVATAR_BLEND_SHAPE blend_shapes[3];            // 00C
  X_AVATAR_METADATA_TEXTURE textures[6];           // 03C
  be<uint32_t> colors[9];                          // 0FC
  X_AVATAR_COMPONENT_INFO body_component;          // 120
  X_AVATAR_COMPONENT_INFO head_component;          // 140
  X_AVATAR_COMPONENT_INFO components[13];          // 160
  X_AVATAR_COMPONENT_INFO fallback_components[4];  // 300
  be<uint64_t> owner_xuid;                         // 380
  uint8_t source_console_id[5];                    // 388
  uint8_t maybe_padding[91];                       // 38D
};
static_assert_size(X_AVATAR_METADATA, 1000);

struct X_AVATAR_VECTOR {
  be<float> x;
  be<float> y;
  be<float> z;
  be<float> w;
};
static_assert_size(X_AVATAR_VECTOR, 16);

struct X_AVATAR_QUATERNION {
  be<float> x;
  be<float> y;
  be<float> z;
  be<float> w;
};
static_assert_size(X_AVATAR_VECTOR, 16);

struct X_AVATAR_SKELETON_JOINT_POSE {
  X_AVATAR_VECTOR position;
  X_AVATAR_QUATERNION rotation;
  X_AVATAR_VECTOR scale;
};
static_assert_size(X_AVATAR_SKELETON_JOINT_POSE, 48);

struct X_AVATAR_SKELETON_JOINT_BINDPOSE {
  X_AVATAR_VECTOR position;
  X_AVATAR_QUATERNION rotation;
};
static_assert_size(X_AVATAR_SKELETON_JOINT_BINDPOSE, 32);

struct X_AVATAR_SKELETON_JOINT {
  be<uint32_t> parent_index;
  be<uint32_t> first_child_index;
  be<uint32_t> next_index;
  be<uint32_t> unknown;
  X_AVATAR_SKELETON_JOINT_BINDPOSE bindpose;
  X_AVATAR_SKELETON_JOINT_POSE pose;
};
static_assert_size(X_AVATAR_SKELETON_JOINT, 96);

struct X_AVATAR_SKELETON {
  be<uint32_t> joint_count;
  be<uint32_t> joints_ptr;
};
static_assert_size(X_AVATAR_SKELETON, 8);

struct X_AVATAR_VECTOR3F {
  be<float> x;
  be<float> y;
  be<float> z;
};
static_assert_size(X_AVATAR_VECTOR3F, 12);

struct X_AVATAR_UV {
  be<uint16_t> u;
  be<uint16_t> v;
};

struct X_AVATAR_VERTEX {
  X_AVATAR_VECTOR3F position;
  be<uint32_t> normal;
  be<uint32_t> blend_weight;
  be<uint32_t> blend_indices;
  be<uint32_t> color;
  X_AVATAR_UV uvs[1];
};
static_assert_size(X_AVATAR_VERTEX, 32);

struct X_AVATAR_SHADER_TEXTURE {
  be<uint16_t> index;
  be<uint16_t> uv_index;
  be<uint32_t> flags;
};

struct X_AVATAR_SHADER_PARAM {
  be<uint32_t> type;
  be<uint32_t> usage;
  union {
    X_AVATAR_SHADER_TEXTURE texture;
    be<float> constant_values[4];
  };
};

struct X_AVATAR_TRIANGLE_BATCH {
  be<uint32_t> shader_id;
  X_AVATAR_SHADER_PARAM shader_parameters[20];
  be<uint32_t> triangle_count;
  be<uint32_t> vertex_count;
  be<uint32_t> uv_count;
  be<uint32_t> vertex_size;
  be<uint32_t> index_size;
  be<uint32_t> vertices_ptr;
  be<uint32_t> indices_ptr;
};
static_assert_size(X_AVATAR_TRIANGLE_BATCH, 512);

struct X_AVATAR_TEXTURE {
  be<uint32_t> format;
  be<uint32_t> width;
  be<uint32_t> height;
  be<uint32_t> total_base_size;
  be<uint32_t> total_mip_size;
  be<uint32_t> base_size;
  be<uint32_t> mip_size;
  be<uint32_t> mip_levels;
  be<uint32_t> layer_count;
  be<uint32_t> base_data_ptr;
  be<uint32_t> mip_data_ptr;
};

struct X_AVATAR_MODEL {
  be<uint32_t> cpu_size;
  be<uint32_t> gpu_size;
  be<uint32_t> texture_size;
  be<uint32_t> vertex_size;
  be<uint32_t> index_size;
  be<uint32_t> triangle_batch_count;
  be<uint32_t> texture_count;
  be<uint32_t> cpu_buffer_ptr;
  be<uint32_t> gpu_buffer_ptr;
  be<uint32_t> vertex_buffer_ptr;
  be<uint32_t> index_buffer_ptr;
  be<uint32_t> triangle_batches_ptr;
  be<uint32_t> textures_ptr;
};
static_assert_size(X_AVATAR_MODEL, 52);

struct X_AVATAR_PROP {
  be<uint32_t> skeleton_ptr;
  X_AVATAR_COMPONENT_INFO component_info;
  X_AVATAR_MODEL component_model;
  be<uint32_t> animation_ptr;
};
static_assert_size(X_AVATAR_PROP, 92);

struct X_AVATAR_ASSETS {
  be<uint32_t> skeleton_ptr;
  be<uint32_t> prop_ptr;
  be<uint32_t> component_count;
  be<uint32_t> component_infos_ptr;
  be<uint32_t> component_models_ptr;
};
static_assert_size(X_AVATAR_ASSETS, 20);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_GUEST_ASSET_H_
