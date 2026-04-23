/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_GUEST_ANIMATION_H_
#define XENIA_AVATARS_GUEST_ANIMATION_H_

#include <memory>

#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

struct X_AVATAR_VECTOR_SERIALIZER {
  be<float> quant_radius;
  be<float> delta_x;
  be<float> delta_y;
  be<float> delta_z;
  be<float> base_x;
  be<float> base_y;
  be<float> base_z;
  uint8_t unknown[12];
  be<uint32_t> bit_count_x;
  be<uint32_t> bit_count_y;
  be<uint32_t> bit_count_z;
};
static_assert_size(X_AVATAR_VECTOR_SERIALIZER, 0x34);

struct X_AVATAR_QUATERNION_SERIALIZER {
  X_AVATAR_VECTOR_SERIALIZER base_serializer;
};
static_assert_size(X_AVATAR_QUATERNION_SERIALIZER, 0x34);

struct X_AVATAR_DWORD_SERIALIZER {
  be<uint32_t> bit_count;
  be<uint32_t> base_value;
  uint8_t unknown[4];
};
static_assert_size(X_AVATAR_DWORD_SERIALIZER, 0xC);

struct X_AVATAR_JOINT_SERIALIZER {
  X_AVATAR_VECTOR_SERIALIZER position_serializer;
  X_AVATAR_QUATERNION_SERIALIZER rotation_serializer;
  X_AVATAR_VECTOR_SERIALIZER scale_serializer;
};
static_assert_size(X_AVATAR_JOINT_SERIALIZER, 0x9C);

struct X_AVATAR_MOTION_SERIALIZER {
  X_AVATAR_VECTOR_SERIALIZER position_serializer;
  X_AVATAR_QUATERNION_SERIALIZER rotation_serializer;
};
static_assert_size(X_AVATAR_MOTION_SERIALIZER, 0x68);

struct X_AVATAR_TEXTURE_SERIALIZER {
  X_AVATAR_DWORD_SERIALIZER layer_index_serializer;
};
static_assert_size(X_AVATAR_TEXTURE_SERIALIZER, 0xC);

struct X_AVATAR_POSE_FRAME_SET {
  be<uint32_t> frame_count;
  be<uint32_t> element_count;
  be<uint32_t> frame_bit_count;
  X_AVATAR_JOINT_SERIALIZER element_serializers[72];
};
static_assert_size(X_AVATAR_POSE_FRAME_SET, 0x2BEC);

struct X_AVATAR_MOTION_FRAME_SET {
  be<uint32_t> frame_count;
  be<uint32_t> element_count;
  be<uint32_t> frame_bit_count;
  X_AVATAR_MOTION_SERIALIZER element_serializers[3];
};
static_assert_size(X_AVATAR_MOTION_FRAME_SET, 0x144);

struct X_AVATAR_TEXTURE_FRAME_SET {
  be<uint32_t> frame_count;
  be<uint32_t> element_count;
  be<uint32_t> frame_bit_count;
  X_AVATAR_TEXTURE_SERIALIZER element_serializers[5];
};
static_assert_size(X_AVATAR_TEXTURE_FRAME_SET, 0x48);

struct X_AVATAR_ANIMATION {
  X_AVATAR_POSE_FRAME_SET pose_frame_sets[2];
  X_AVATAR_MOTION_FRAME_SET motion_frame_set;
  X_AVATAR_TEXTURE_FRAME_SET texture_frame_set;
  be<uint32_t> frame_count;
  be<float> frames_per_second;
  be<uint32_t> pose_counts[2];
  be<uint32_t> motion_count;
  be<uint32_t> texture_count;
  be<uint32_t> pose_2_offset;
  be<uint32_t> motions_offset;
  be<uint32_t> textures_offset;
  be<uint32_t> compressed_data_size;
  be<uint32_t> compressed_data_buffer_ptr;
};
static_assert_size(X_AVATAR_ANIMATION, 0x5990);

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_GUEST_ANIMATION_H_
