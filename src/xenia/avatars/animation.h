/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_ANIMATION_H_
#define XENIA_AVATARS_ANIMATION_H_

#include <memory>
#include <vector>

#include "xenia/avatars/common.h"
#include "xenia/avatars/serializers.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

class BitStream;

struct AnimationPose {
  Vector3<float> position;
  Quaternion<float> rotation;
  Vector3<float> scale;
};

struct AnimationPoseSerializer {
 public:
  VectorSerializer position_serializer;
  QuaternionSerializer rotation_serializer;
  VectorSerializer scale_serializer;

  size_t element_bit_size() const {
    return position_serializer.element_bit_size() +
           rotation_serializer.element_bit_size() +
           scale_serializer.element_bit_size();
  }

  static size_t serializer_bit_size() {
    return VectorSerializer::serializer_bit_size() +
           QuaternionSerializer::serializer_bit_size() +
           VectorSerializer::serializer_bit_size();
  }

 public:
  static AnimationPoseSerializer From(BitStream& stream) {
    AnimationPoseSerializer instance;
    instance.position_serializer = VectorSerializer::From(stream);
    instance.rotation_serializer = QuaternionSerializer::From(stream);
    instance.scale_serializer = VectorSerializer::From(stream);
    return instance;
  }

  void invert() {
    position_serializer.invert();
    rotation_serializer.invert();
  }

  AnimationPose Read(BitStream& stream) const {
    AnimationPose instance;
    instance.position = position_serializer.Read(stream);
    instance.rotation = rotation_serializer.Read(stream);
    instance.scale = scale_serializer.Read(stream);
    return instance;
  }
};

struct AnimationMotion {
  Vector3<float> position;
  Quaternion<float> rotation;
};

struct AnimationMotionSerializer {
 public:
  VectorSerializer position_serializer;
  QuaternionSerializer rotation_serializer;

  size_t element_bit_size() const {
    return position_serializer.element_bit_size() +
           rotation_serializer.element_bit_size();
  }

  static size_t serializer_bit_size() {
    return VectorSerializer::serializer_bit_size() +
           QuaternionSerializer::serializer_bit_size();
  }

 public:
  static AnimationMotionSerializer From(BitStream& stream) {
    AnimationMotionSerializer instance;
    instance.position_serializer = VectorSerializer::From(stream);
    instance.rotation_serializer = QuaternionSerializer::From(stream);
    return instance;
  }

  void invert() {
    position_serializer.invert();
    rotation_serializer.invert();
  }

  AnimationMotion Read(BitStream& stream) const {
    AnimationMotion instance;
    instance.position = position_serializer.Read(stream);
    instance.rotation = rotation_serializer.Read(stream);
    return instance;
  }
};

struct AnimationTexture {
  uint32_t layer_index;
};

struct AnimationTextureSerializer {
 public:
  using LayerIndexSerializer = ValueSerializer<uint32_t>;

  LayerIndexSerializer layer_index_serializer;

  size_t element_bit_size() const {
    return layer_index_serializer.element_bit_size();
  }

  static size_t serializer_bit_size() {
    return LayerIndexSerializer::serializer_bit_size();
  }

 public:
  static AnimationTextureSerializer From(BitStream& stream) {
    AnimationTextureSerializer instance;
    instance.layer_index_serializer = LayerIndexSerializer::From(stream);
    return instance;
  }

  void invert() {}

  AnimationTexture Read(BitStream& stream) const {
    AnimationTexture instance;
    instance.layer_index = layer_index_serializer.Read(stream);
    return instance;
  }
};

typedef uint32_t AnimationLoadOptions;

namespace AnimationLoadOption {

using Option = AnimationLoadOptions;

const Option kNone = 0;

const Option kElements = 1 << 0;        // parse elements
const Option kCompressedData = 1 << 1;  // retain copy of compressed data
const Option kInvert = 1 << 2;

const Option kGuest = kCompressedData;

}  // namespace AnimationLoadOption

template <typename Element, size_t MaxElementCount, typename ElementSerializer>
struct AnimationFrameSet {
 public:
  struct Frame;
  ElementSerializer element_serializers[MaxElementCount];
  size_t frame_bit_count;
  size_t frame_count;
  std::vector<Frame> frames;

 public:
  struct Frame {
    std::vector<Element> elements;
  };

 public:
  static AnimationFrameSet Read(BitStream& stream,
                                AnimationLoadOptions load_options,
                                size_t frame_count, size_t element_count) {
    size_t local_frame_count = stream.Read<uint32_t>();
    assert_true(local_frame_count == frame_count);

    size_t local_element_count = stream.Read<uint32_t>();
    assert_true(local_element_count == element_count);
    assert_true(local_element_count <= MaxElementCount);

    AnimationFrameSet instance;
    size_t frame_bit_count = 0;
    for (size_t i = 0, o = element_count - 1; i < element_count; ++i, --o) {
      auto element_serializer = ElementSerializer::From(stream);
      if (load_options & AnimationLoadOption::kInvert) {
        element_serializer.invert();
      }
      frame_bit_count += element_serializer.element_bit_size();
      instance.element_serializers[o] = element_serializer;
    }
    for (size_t i = element_count; i < MaxElementCount; ++i) {
      stream.Advance(ElementSerializer::serializer_bit_size());
      instance.element_serializers[i] = {};
    }
    instance.frame_bit_count = frame_bit_count;
    instance.frame_count = frame_count;
    if (!!(load_options & AnimationLoadOption::kElements)) {
      for (size_t i = 0; i < frame_count; ++i) {
        Frame frame;
        for (size_t j = 0; j < element_count; ++j) {
          const auto& element_serializer = instance.element_serializers[j];
          Element element = element_serializer.Read(stream);
          frame.elements.push_back(element);
        }
        instance.frames.push_back(frame);
      }
    } else {
      stream.Advance(frame_bit_count * frame_count);
    }
    return instance;
  }
};

class Animation {
 public:
  using PoseFrameSet =
      AnimationFrameSet<AnimationPose, 72, AnimationPoseSerializer>;
  using MotionFrameSet =
      AnimationFrameSet<AnimationMotion, 3, AnimationMotionSerializer>;
  using TextureFrameSet =
      AnimationFrameSet<AnimationTexture, 5, AnimationTextureSerializer>;

 public:
  uint32_t frame_count;
  float frames_per_second;
  uint32_t pose_counts[2];
  uint32_t motion_count;
  uint32_t texture_count;
  uint32_t pose_2_byte_offset;
  uint32_t textures_byte_offset;
  uint32_t motions_byte_offset;

  std::vector<uint8_t> compressed_data_bytes;

  PoseFrameSet pose_frame_sets[2];
  MotionFrameSet motion_frame_set;
  TextureFrameSet texture_frame_set;

 public:
  static std::shared_ptr<Animation> Load(const uint8_t* strb_buffer,
                                         size_t strb_size,
                                         AnimationLoadOptions load_options);

 private:
  static std::shared_ptr<Animation> Read(const uint8_t* data_buffer,
                                         size_t data_size,
                                         AnimationLoadOptions load_options);
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_ANIMATION_H_
