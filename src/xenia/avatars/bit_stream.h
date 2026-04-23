/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_BIT_STREAM_H_
#define XENIA_AVATARS_BIT_STREAM_H_

#include <cstddef>
#include <cstdint>

#include "xenia/base/assert.h"

namespace xe {
namespace avatars {

class BitStream {
 public:
  BitStream(const uint8_t* buffer, size_t size_in_bits);
  ~BitStream();

  const uint8_t* buffer() const { return buffer_; }
  size_t offset_bits() const { return offset_bits_; }
  size_t size_bits() const { return size_bits_; }

  void Advance(size_t num_bits);
  void SetOffset(size_t offset_bits);
  size_t BitsRemaining() const;

  void AlignToNextByte() {
    size_t offset_bits = (offset_bits_ + 7) & ~7;
    assert_true(offset_bits <= size_bits_);
    offset_bits_ = offset_bits;
  }

  BitStream Slice(size_t num_bits) {
    assert_true(offset_bits_ + num_bits <= size_bits_);
    size_t read_bits = offset_bits_ % 8;
    BitStream stream(&buffer_[offset_bits_ >> 3], read_bits + num_bits);
    stream.SetOffset(read_bits);
    Advance(num_bits);
    return stream;
  }

  uint64_t Peek(size_t num_bits) const;
  uint64_t Read(size_t num_bits);

  void ReadBytes(uint8_t* buffer, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      buffer[i] = Read<uint8_t>();
    }
  }

  template <typename T>
  T Read() {
    auto value = Read(sizeof(T) * 8);
    return static_cast<T>(value);
  }

  template <typename T>
  T Read(size_t num_bits) {
    assert_true(num_bits <= sizeof(T) * 8);
    auto value = Read(num_bits);
    return static_cast<T>(value);
  }

  template <>
  bool Read() {
    return Read<uint8_t>(1) != 0;
  }

  template <>
  float Read() {
    union {
      uint32_t u;
      float f;
    } value;
    value.u = Read<uint32_t>();
    return value.f;
  }

 private:
  const uint8_t* buffer_ = nullptr;
  size_t offset_bits_ = 0;
  size_t size_bits_ = 0;
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_BIT_STREAM_H_
