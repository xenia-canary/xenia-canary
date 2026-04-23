/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>

#include "xenia/avatars/animation.h"
#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/guest_animation.h"
#include "xenia/avatars/guest_load_animation.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace avatars {

static void VectorSerializerToGuest(
    const VectorSerializer& host_serializer,
    X_AVATAR_VECTOR_SERIALIZER& guest_serializer) {
  guest_serializer.quant_radius = host_serializer.quant_radius;
  guest_serializer.delta_x = host_serializer.delta_x;
  guest_serializer.delta_y = host_serializer.delta_y;
  guest_serializer.delta_z = host_serializer.delta_z;
  guest_serializer.base_x = host_serializer.base_x;
  guest_serializer.base_y = host_serializer.base_y;
  guest_serializer.base_z = host_serializer.base_z;
  // unknown[12]
  std::memset(guest_serializer.unknown, 0, sizeof(guest_serializer.unknown));
  guest_serializer.bit_count_x = host_serializer.bit_count_x;
  guest_serializer.bit_count_y = host_serializer.bit_count_y;
  guest_serializer.bit_count_z = host_serializer.bit_count_z;
}

static void QuaternionSerializerToGuest(
    const QuaternionSerializer& host_serializer,
    X_AVATAR_QUATERNION_SERIALIZER& guest_serializer) {
  VectorSerializerToGuest(host_serializer.base_serializer,
                          guest_serializer.base_serializer);
}

static void DwordSerializerToGuest(
    const ValueSerializer<uint32_t>& host_serializer,
    X_AVATAR_DWORD_SERIALIZER& guest_serializer) {
  guest_serializer.bit_count = host_serializer.bit_count;
  guest_serializer.base_value = host_serializer.base_value;
  // unknown[4]
  std::memset(guest_serializer.unknown, 0, sizeof(guest_serializer.unknown));
}

static void PoseFrameSetToGuest(const Animation::PoseFrameSet& host,
                                X_AVATAR_POSE_FRAME_SET& guest,
                                size_t element_count) {
  guest.frame_count = static_cast<uint32_t>(host.frame_count);
  guest.element_count = static_cast<uint32_t>(element_count);
  guest.frame_bit_count = static_cast<uint32_t>(host.frame_bit_count);
  assert_true(countof(guest.element_serializers) ==
              countof(host.element_serializers));
  for (size_t i = 0; i < countof(guest.element_serializers); ++i) {
    const auto& host_element_serializer = host.element_serializers[i];
    auto& guest_element_serializer = guest.element_serializers[i];
    VectorSerializerToGuest(host_element_serializer.position_serializer,
                            guest_element_serializer.position_serializer);
    QuaternionSerializerToGuest(host_element_serializer.rotation_serializer,
                                guest_element_serializer.rotation_serializer);
    VectorSerializerToGuest(host_element_serializer.scale_serializer,
                            guest_element_serializer.scale_serializer);
  }
}

static void MotionFrameSetToGuest(const Animation::MotionFrameSet& host,
                                  X_AVATAR_MOTION_FRAME_SET& guest,
                                  size_t element_count) {
  guest.frame_count = static_cast<uint32_t>(host.frame_count);
  guest.element_count = static_cast<uint32_t>(element_count);
  guest.frame_bit_count = static_cast<uint32_t>(host.frame_bit_count);
  assert_true(countof(guest.element_serializers) ==
              countof(host.element_serializers));
  for (size_t i = 0; i < countof(guest.element_serializers); ++i) {
    const auto& host_element_serializer = host.element_serializers[i];
    auto& guest_element_serializer = guest.element_serializers[i];
    VectorSerializerToGuest(host_element_serializer.position_serializer,
                            guest_element_serializer.position_serializer);
    QuaternionSerializerToGuest(host_element_serializer.rotation_serializer,
                                guest_element_serializer.rotation_serializer);
  }
}

static void TextureFrameSetToGuest(const Animation::TextureFrameSet& host,
                                   X_AVATAR_TEXTURE_FRAME_SET& guest,
                                   size_t element_count) {
  guest.frame_count = static_cast<uint32_t>(host.frame_count);
  guest.element_count = static_cast<uint32_t>(element_count);
  guest.frame_bit_count = static_cast<uint32_t>(host.frame_bit_count);
  assert_true(countof(guest.element_serializers) ==
              countof(host.element_serializers));
  for (size_t i = 0; i < countof(guest.element_serializers); ++i) {
    const auto& host_element_serializer = host.element_serializers[i];
    auto& guest_element_serializer = guest.element_serializers[i];
    DwordSerializerToGuest(host_element_serializer.layer_index_serializer,
                           guest_element_serializer.layer_index_serializer);
  }
}

bool LoadAnimationToGuest(const AssetId& asset_id,
                          std::shared_ptr<Animation> animation,
                          X_AVATAR_ANIMATION* guest_animation,
                          uint8_t* guest_compressed_buffer) {
  if (animation->compressed_data_bytes.size() >
      guest_animation->compressed_data_size) {
    XELOGE(
        "Not enough space to copy avatar animation compressed data for {}! ({} "
        "> {})",
        asset_id.to_string(), animation->compressed_data_bytes.size(),
        guest_animation->compressed_data_size.get());
    return false;
  }

  assert_true(countof(guest_animation->pose_frame_sets) ==
              countof(animation->pose_frame_sets));
  for (size_t i = 0; i < countof(guest_animation->pose_frame_sets); ++i) {
    const auto& pose_frame_set = animation->pose_frame_sets[i];
    auto& guest_pose_frame_set = guest_animation->pose_frame_sets[i];
    PoseFrameSetToGuest(pose_frame_set, guest_pose_frame_set,
                        animation->pose_counts[i]);
  }

  MotionFrameSetToGuest(animation->motion_frame_set,
                        guest_animation->motion_frame_set,
                        animation->motion_count);

  TextureFrameSetToGuest(animation->texture_frame_set,
                         guest_animation->texture_frame_set,
                         animation->texture_count);

  assert_true(offsetof(X_AVATAR_ANIMATION, frame_count) == 0x5964);
  assert_true(offsetof(X_AVATAR_ANIMATION, frames_per_second) == 0x5968);
  assert_true(offsetof(X_AVATAR_ANIMATION, pose_counts) == 0x596C);
  assert_true(offsetof(X_AVATAR_ANIMATION, motion_count) == 0x5974);
  assert_true(offsetof(X_AVATAR_ANIMATION, texture_count) == 0x5978);
  assert_true(offsetof(X_AVATAR_ANIMATION, pose_2_offset) == 0x597C);
  assert_true(offsetof(X_AVATAR_ANIMATION, motions_offset) == 0x5980);
  assert_true(offsetof(X_AVATAR_ANIMATION, textures_offset) == 0x5984);
  assert_true(offsetof(X_AVATAR_ANIMATION, compressed_data_size) == 0x5988);
  assert_true(offsetof(X_AVATAR_ANIMATION, compressed_data_buffer_ptr) ==
              0x598C);

  guest_animation->frame_count = animation->frame_count;
  guest_animation->frames_per_second = animation->frames_per_second;
  guest_animation->pose_counts[0] = animation->pose_counts[0];
  guest_animation->pose_counts[1] = animation->pose_counts[1];
  guest_animation->motion_count = animation->motion_count;
  guest_animation->texture_count = animation->texture_count;
  guest_animation->pose_2_offset = animation->pose_2_byte_offset;
  guest_animation->motions_offset = animation->motions_byte_offset;
  guest_animation->textures_offset = animation->textures_byte_offset;
  guest_animation->compressed_data_size =
      static_cast<uint32_t>(animation->compressed_data_bytes.size());

  std::memcpy(guest_compressed_buffer, animation->compressed_data_bytes.data(),
              guest_animation->compressed_data_size);

  return true;
}

bool LoadAnimationToGuest(AssetPack* asset_pack, const AssetId& asset_id,
                          X_AVATAR_ANIMATION* guest_animation,
                          uint8_t* guest_compressed_data,
                          uint32_t coordinate_system) {
  const uint8_t* strb_buffer;
  size_t strb_size;
  if (!asset_pack->GetAssetData(asset_id, strb_buffer, strb_size)) {
    XELOGE("Failed to find avatar animation {}!", asset_id.to_string());
    return false;
  }

  AnimationLoadOptions load_options = AnimationLoadOption::kGuest;
  if (coordinate_system == 0) {
    load_options |= AnimationLoadOption::kInvert;
  }

  auto animation = Animation::Load(strb_buffer, strb_size, load_options);
  if (!animation) {
    XELOGE("Failed to load avatar animation {}!", asset_id.to_string());
    return false;
  }

  return LoadAnimationToGuest(asset_id, animation, guest_animation,
                              guest_compressed_data);
}

}  // namespace avatars
}  // namespace xe
