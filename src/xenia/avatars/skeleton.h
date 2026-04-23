/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_SKELETON_H_
#define XENIA_AVATARS_SKELETON_H_

#include <memory>
#include <vector>

#include "xenia/avatars/common.h"
#include "xenia/avatars/serializers.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

class BitStream;

struct JointBindPose {
  Vector3<float> position;
  Quaternion<float> rotation;
};

struct JointPose {
  Vector3<float> position;
  Quaternion<float> rotation;
  Vector3<float> scale;
};

struct Joint {
  uint8_t parent_index;
  uint8_t first_child_index;
  uint8_t next_index;
  JointBindPose bindpose;
  JointPose pose;
};

typedef uint32_t SkeletonLoadOptions;

namespace SkeletonLoadOption {

using Option = SkeletonLoadOptions;

const Option kNone = 0;

const Option kInvert = 1 << 0;

}  // namespace SkeletonLoadOption

class Skeleton {
 public:
  std::vector<Joint> joints;

 private:
  void Initialize();

 public:
  static std::shared_ptr<Skeleton> Load(const uint8_t* strb_buffer,
                                        size_t strb_size,
                                        SkeletonLoadOptions load_options);

  static std::shared_ptr<Skeleton> Read(const uint8_t* data_buffer,
                                        size_t data_size,
                                        SkeletonLoadOptions load_options);
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_SKELETON_H_
