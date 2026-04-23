/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/animation.h"

#include "xenia/avatars/bit_stream.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/serializers.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace avatars {

std::shared_ptr<Animation> Animation::Read(const uint8_t* data_buffer,
                                           size_t data_size,
                                           AnimationLoadOptions load_options) {
  BitStream stream(data_buffer, data_size * 8);

  auto instance = std::make_shared<Animation>();

  instance->frame_count = stream.Read<uint32_t>();
  instance->frames_per_second = stream.Read<float>();
  instance->pose_counts[0] = stream.Read<uint32_t>();
  instance->pose_counts[1] = stream.Read<uint32_t>();
  instance->motion_count = stream.Read<uint32_t>();
  instance->texture_count = stream.Read<uint32_t>();
  instance->pose_2_byte_offset = stream.Read<uint32_t>();
  instance->textures_byte_offset = stream.Read<uint32_t>();
  instance->motions_byte_offset = stream.Read<uint32_t>();
  size_t compressed_data_size = stream.Read<uint32_t>();
  stream.AlignToNextByte();

  // copy compressed data, if requested
  if (load_options & AnimationLoadOption::kCompressedData) {
    assert_true(stream.offset_bits() + compressed_data_size * 8 <=
                stream.size_bits());
    instance->compressed_data_bytes.resize(compressed_data_size);
    std::memcpy(instance->compressed_data_bytes.data(),
                &stream.buffer()[stream.offset_bits() >> 3],
                compressed_data_size);
  }

  auto slice = stream.Slice(compressed_data_size * 8);
  assert_true(stream.offset_bits() == stream.size_bits());
  stream = slice;

  instance->pose_frame_sets[0] = PoseFrameSet::Read(
      stream, load_options, instance->frame_count, instance->pose_counts[0]);
  stream.AlignToNextByte();
  assert_true(static_cast<size_t>(instance->pose_2_byte_offset) * 8 ==
              stream.offset_bits());

  instance->pose_frame_sets[1] = PoseFrameSet::Read(
      stream, load_options, instance->frame_count, instance->pose_counts[1]);
  stream.AlignToNextByte();
  assert_true(static_cast<size_t>(instance->motions_byte_offset) * 8 ==
              stream.offset_bits());

  instance->motion_frame_set = MotionFrameSet::Read(
      stream, load_options, instance->frame_count, instance->motion_count);
  stream.AlignToNextByte();
  assert_true(static_cast<size_t>(instance->textures_byte_offset) * 8 ==
              stream.offset_bits());

  instance->texture_frame_set = TextureFrameSet::Read(
      stream, load_options, instance->frame_count, instance->texture_count);
  stream.AlignToNextByte();

  // TODO(gibbed): figure out rest of trailing data
  // assert_true(stream.offset_bits() == stream.size_bits());

  return instance;
}

std::shared_ptr<Animation> Animation::Load(const uint8_t* strb_buffer,
                                           size_t strb_size,
                                           AnimationLoadOptions load_options) {
  const uint8_t* data_buffer;
  size_t data_size;
  if (!strb::GetSTRBBlock(strb_buffer, strb_size, strb::STRBBlockId::kAnimation,
                          data_buffer, data_size)) {
    return nullptr;
  }

  return Read(data_buffer, data_size, load_options);
}

}  // namespace avatars
}  // namespace xe
