/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/skeleton.h"

#include "xenia/avatars/bit_stream.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/serializers.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/kernel_state.h"

#include "third_party/glm/glm/gtc/quaternion.hpp"

namespace xe {
namespace avatars {

struct JointSerializer {
 public:
  VectorSerializer position_serializer;
  QuaternionSerializer rotation_serializer;

  size_t element_bit_size() const {
    return 8 + position_serializer.element_bit_size() +
           rotation_serializer.element_bit_size();
  }

 public:
  static JointSerializer From(BitStream& stream) {
    JointSerializer instance{};
    instance.position_serializer = VectorSerializer::From(stream);
    instance.rotation_serializer = QuaternionSerializer::From(stream);
    return instance;
  }

  void invert() {
    position_serializer.invert();
    rotation_serializer.invert();
  }

  Joint Read(BitStream& stream) const {
    Joint instance{};
    instance.parent_index = stream.Read<uint8_t>();
    instance.bindpose.position = position_serializer.Read(stream);
    instance.bindpose.rotation = rotation_serializer.Read(stream);
    return instance;
  }
};

void Skeleton::Initialize() {
  const uint8_t InvalidIndex = 255;

  assert_true(joints.size() < 255);

  uint8_t joint_count = static_cast<uint8_t>(joints.size());

  for (uint8_t i = 0; i < joint_count; ++i) {
    auto& joint = joints[i];
    joint.first_child_index = InvalidIndex;
    joint.next_index = InvalidIndex;
    joint.pose.position = joint.bindpose.position;
    joint.pose.rotation = joint.bindpose.rotation;
    joint.pose.scale = {1.0f, 1.0f, 1.0f};
  }

  for (uint8_t i = joint_count - 1; i >= 1; --i) {
    auto& joint = joints[i];
    const auto& parentJoint = joints[joint.parent_index];

    const glm::quat q(joint.bindpose.rotation.w, joint.bindpose.rotation.x,
                      joint.bindpose.rotation.y, joint.bindpose.rotation.z);
    glm::mat4 matrixA = glm::mat4_cast(q);

    matrixA[3] = glm::vec4(joint.bindpose.position.x, joint.bindpose.position.y,
                           joint.bindpose.position.z, 1.f);

    const glm::quat parentQ(1.f, parentJoint.bindpose.rotation.x,
                            parentJoint.bindpose.rotation.y,
                            parentJoint.bindpose.rotation.z);
    glm::mat4 matrix = glm::mat4_cast(parentQ);
    matrix[3] = glm::vec4(parentJoint.bindpose.position.x,
                          parentJoint.bindpose.position.y,
                          parentJoint.bindpose.position.z, 1.f);

    const glm::mat4 matrixB = glm::inverse(matrix);

    const glm::mat4 matrix2 = matrixB * matrixA;

    const glm::vec4 position = matrix2[3];

    const glm::quat rotation = glm::quat_cast(matrix2);

    joint.pose.position.x = position.x;
    joint.pose.position.y = position.y;
    joint.pose.position.z = position.z;

    joint.pose.rotation.x = rotation.x;
    joint.pose.rotation.y = rotation.y;
    joint.pose.rotation.z = rotation.z;
    joint.pose.rotation.w = rotation.w;
  }

  {
    auto& joint = joints[0];
    joint.pose.position = joint.bindpose.position;
    // joint.pose.position.w = 1.f;
    joint.pose.rotation = joint.bindpose.rotation;
  }

  for (uint8_t parent_index = 0; parent_index < joint_count; ++parent_index) {
    uint8_t first_child_index = InvalidIndex;
    for (uint8_t child_index = 0; child_index < joint_count; ++child_index) {
      if (joints[child_index].parent_index == parent_index) {
        first_child_index = child_index;
        break;
      }
    }

    // didn't find any children
    if (first_child_index == InvalidIndex) {
      continue;
    }

    joints[parent_index].first_child_index = first_child_index;

    // find next
    uint8_t prev_index = first_child_index;
    for (uint8_t next_index = first_child_index + 1; next_index < joint_count;
         ++next_index) {
      if (joints[next_index].parent_index == parent_index) {
        joints[prev_index].next_index = next_index;
        prev_index = next_index;
      }
    }
  }
}

std::shared_ptr<Skeleton> Skeleton::Read(const uint8_t* data_buffer,
                                         size_t data_size,
                                         SkeletonLoadOptions load_options) {
  BitStream stream(data_buffer, data_size * 8);

  auto instance = std::make_shared<Skeleton>();

  size_t joint_count = stream.Read<uint32_t>();
  assert_true(joint_count <= 72);

  auto joint_serializer = JointSerializer::From(stream);
  if (load_options & SkeletonLoadOption::kInvert) {
    joint_serializer.invert();
  }
  for (size_t i = 0; i < joint_count; ++i) {
    auto joint = joint_serializer.Read(stream);
    instance->joints.push_back(joint);
  }

  stream.AlignToNextByte();
  assert_true(stream.offset_bits() == stream.size_bits());

  instance->Initialize();

  return instance;
}

std::shared_ptr<Skeleton> Skeleton::Load(const uint8_t* strb_buffer,
                                         size_t strb_size,
                                         SkeletonLoadOptions load_options) {
  const uint8_t* compressed_buffer;
  size_t compressed_size;
  if (!strb::GetSTRBBlock(strb_buffer, strb_size, strb::STRBBlockId::kSkeleton,
                          compressed_buffer, compressed_size)) {
    return nullptr;
  }

  size_t data_size;
  if (!compression::GetUncompressedSize(compressed_buffer, compressed_size,
                                        data_size)) {
    assert_always();
    XELOGE("Failed to get uncompressed size for avatar skeleton!");
    return nullptr;
  }

  std::vector<uint8_t> data_bytes(data_size);
  if (!compression::Decompress(compressed_buffer, compressed_size,
                               data_bytes.data(), data_size)) {
    assert_always();
    XELOGE("Failed to decompress avatar skeleton!");
    return nullptr;
  }

  return Read(data_bytes.data(), data_bytes.size(), load_options);
}

}  // namespace avatars
}  // namespace xe
