/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_SERIALIZERS_H_
#define XENIA_AVATARS_SERIALIZERS_H_

#include <cmath>
#include <memory>
#include <vector>

#include "xenia/avatars/bit_stream.h"
#include "xenia/avatars/common.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

struct VectorSerializer {
 public:
  float quant_radius;
  float base_x;
  float base_y;
  float base_z;
  float delta_x;
  float delta_y;
  float delta_z;
  uint8_t bit_count_x;
  uint8_t bit_count_y;
  uint8_t bit_count_z;

  size_t element_bit_size() const {
    return static_cast<size_t>(bit_count_x) + static_cast<size_t>(bit_count_y) +
           static_cast<size_t>(bit_count_z);
  }

  static size_t serializer_bit_size() {
    return static_cast<size_t>(32) + 32 + 32 + 32 + 6 + 6 + 6;
  }

  static VectorSerializer From(BitStream& stream) {
    VectorSerializer instance;
    instance.quant_radius = stream.Read<float>();
    instance.base_x = stream.Read<float>();
    instance.base_y = stream.Read<float>();
    instance.base_z = stream.Read<float>();
    instance.bit_count_x = stream.Read<uint8_t>(6);
    instance.bit_count_y = stream.Read<uint8_t>(6);
    instance.bit_count_z = stream.Read<uint8_t>(6);
    instance.delta_x = instance.quant_radius * 2.f;
    instance.delta_y = instance.quant_radius * (2.f / 3.f) * std::sqrtf(6.f);
    instance.delta_z = instance.quant_radius * std::sqrtf(3.f);
    return instance;
  }

  void invert() {
    base_z = -base_z;
    delta_z = -delta_z;
  }

  Vector3<float> Read(BitStream& stream) const {
    int64_t x = stream.Read<int64_t>(bit_count_x);
    int64_t y = stream.Read<int64_t>(bit_count_y);
    int64_t z = stream.Read<int64_t>(bit_count_z);
    Vector3<float> instance;
    instance.y = base_y + delta_y * (float)y;
    instance.z = base_z + delta_z * (float)z;
    if ((y & 1) != 0) {
      // instance.z += 0.577350259f * delta_z;
      instance.z += (1.f / 3.f) * delta_z;
    }
    instance.x = base_x + delta_x * (float)x;
    if ((y & 1) != (z & 1)) {
      instance.x += 0.5f * delta_x;
    }
    return instance;
  }
};

struct QuaternionSerializer {
 public:
  VectorSerializer base_serializer;

  size_t element_bit_size() const { return base_serializer.element_bit_size(); }

  static size_t serializer_bit_size() {
    return VectorSerializer::serializer_bit_size();
  }

  static QuaternionSerializer From(BitStream& stream) {
    QuaternionSerializer instance;
    instance.base_serializer = VectorSerializer::From(stream);
    return instance;
  }

  void invert() {
    base_serializer.base_x = -base_serializer.base_x;
    base_serializer.delta_x = -base_serializer.delta_x;
    base_serializer.base_y = -base_serializer.base_y;
    base_serializer.delta_y = -base_serializer.delta_y;
  }

  Quaternion<float> Read(BitStream& stream) const {
    Vector3<float> vector = base_serializer.Read(stream);
    float num = std::sqrtf(vector.x * vector.x + vector.y * vector.y +
                           vector.z * vector.z);
    Quaternion<float> instance;
    if (num < 1E-07f) {
      instance.x = 0.f;
      instance.y = 0.f;
      instance.z = 0.f;
      instance.w = 1.f;
    } else {
      float num2 = std::sinf(num);
      float w = std::cosf(num);
      float num3 = num2 / num;
      instance.x = vector.x * num3;
      instance.y = vector.y * num3;
      instance.z = vector.z * num3;
      instance.w = w;
    }
    return instance;
  }
};

template <typename T>
struct ValueSerializer {
 public:
  T base_value;
  T bit_count;

  size_t element_bit_size() const { return static_cast<size_t>(bit_count); }

  static size_t serializer_bit_size() { return (sizeof(T) + sizeof(T)) * 8; }

  static ValueSerializer From(BitStream& stream) {
    ValueSerializer instance;
    instance.base_value = stream.Read<T>();
    instance.bit_count = stream.Read<T>();
    return instance;
  }

  T Read(BitStream& stream) const {
    uint64_t value = base_value + stream.Read(bit_count);
    return static_cast<T>(value);
  }
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_SERIALIZERS_H_
