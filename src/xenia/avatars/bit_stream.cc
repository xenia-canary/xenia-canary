/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/bit_stream.h"

#include <algorithm>
#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"

namespace xe {
namespace avatars {

BitStream::BitStream(const uint8_t* buffer, size_t size_in_bits)
    : buffer_(buffer), size_bits_(size_in_bits) {}

BitStream::~BitStream() = default;

void BitStream::SetOffset(size_t offset_bits) {
  assert_true(offset_bits <= size_bits_);
  offset_bits_ = std::min(offset_bits, size_bits_);
}

size_t BitStream::BitsRemaining() const { return size_bits_ - offset_bits_; }

uint64_t BitStream::Peek(size_t num_bits) const {
  assert_true(num_bits <= 64);
  assert_true(offset_bits_ + num_bits <= size_bits_);
  size_t offset_bits = offset_bits_;
  uint64_t result = 0;
  size_t shift = 0;
  while (num_bits > 0) {
    size_t remaining_bits = offset_bits % 8;
    size_t desired_bits = std::min(8 - remaining_bits, num_bits);

    uint64_t mask = (1ull << desired_bits) - 1;
    uint64_t value = buffer_[offset_bits >> 3] >> remaining_bits;

    result |= (mask & value) << shift;
    shift += desired_bits;
    num_bits -= desired_bits;
    offset_bits += desired_bits;
  }
  return result;
}

uint64_t BitStream::Read(size_t num_bits) {
  uint64_t val = Peek(num_bits);
  Advance(num_bits);
  return val;
}

void BitStream::Advance(size_t num_bits) { SetOffset(offset_bits_ + num_bits); }

}  // namespace avatars
}  // namespace xe
