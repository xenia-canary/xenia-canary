/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_codec/strb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace {
using xe::kernel::xam::avatar::codec::FindStrbBlock;

void Field(std::vector<uint8_t>& bytes, uint64_t value, size_t width,
           bool little) {
  for (size_t i = 0; i < width; ++i) {
    const size_t shift = 8 * (little ? i : width - 1 - i);
    bytes.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void Pad(std::vector<uint8_t>& bytes, size_t alignment) {
  while (bytes.size() % alignment) {
    bytes.push_back(0);
  }
}

TEST_CASE("STRB returns byte counts independently of element width",
          "[kernel][avatar]") {
  for (bool little : {false, true}) {
    for (uint8_t id_width : {1, 2, 4, 8}) {
      for (uint8_t length_width : {1, 2, 4, 8}) {
        for (uint8_t alignment : {1, 8}) {
          CAPTURE(little, int(id_width), int(length_width), int(alignment));
          std::vector<uint8_t> bytes(alignment == 1 ? 26 : 30, 0);
          bytes[0] = 'S';
          bytes[1] = 'T';
          bytes[2] = 'R';
          bytes[3] = 'B';
          bytes[4] = alignment != 1;
          bytes[5] = little;
          bytes[22] = id_width;
          bytes[23] = length_width;
          if (alignment != 1) {
            bytes[26] = alignment;
          }
          Pad(bytes, alignment);

          const uint64_t first_id = id_width == 1 ? 3 : 0x1234;
          Field(bytes, first_id, id_width, little);
          Field(bytes, 6, length_width, little);
          Field(bytes, 2, length_width, little);
          Pad(bytes, alignment);
          const size_t first_offset = bytes.size();
          bytes.insert(bytes.end(), {1, 2, 3, 4, 5, 6});
          Pad(bytes, alignment);

          Field(bytes, 2, id_width, little);
          Field(bytes, 1, length_width, little);
          Field(bytes, 1, length_width, little);
          Pad(bytes, alignment);
          bytes.push_back(42);

          const auto first = FindStrbBlock(bytes, first_id);
          REQUIRE(first.has_value());
          REQUIRE(first->size() == 6);
          REQUIRE(first->data() == bytes.data() + first_offset);
          const auto second = FindStrbBlock(bytes, 2);
          REQUIRE(second.has_value());
          REQUIRE(second->size() == 1);
          REQUIRE(second->front() == 42);
          REQUIRE_FALSE(FindStrbBlock(bytes, 99));
          bytes.pop_back();
          REQUIRE_FALSE(FindStrbBlock(bytes, first_id));
        }
      }
    }
  }
}

TEST_CASE("STRB validates the envelope without authenticating its prefix",
          "[kernel][avatar]") {
  std::vector<uint8_t> bytes(0x140 + 26, 0);
  const std::array<uint8_t, 4> prefix = {'Y', 'T', 'G', 'R'};
  const std::array<uint8_t, 4> body = {'S', 'T', 'R', 'B'};
  std::copy(prefix.begin(), prefix.end(), bytes.begin());
  std::copy(body.begin(), body.end(), bytes.begin() + 0x140);
  bytes[0x140 + 22] = bytes[0x140 + 23] = 1;
  bytes.insert(bytes.end(), {3, 4, 2, 11, 12, 13, 14});
  REQUIRE(FindStrbBlock(bytes, 3).has_value());
  for (size_t prefix_size = 0; prefix_size < bytes.size(); ++prefix_size) {
    REQUIRE_FALSE(FindStrbBlock(std::span(bytes).first(prefix_size), 3));
  }
  bytes[0x140 + 28] = 3;
  REQUIRE_FALSE(FindStrbBlock(bytes, 3));
  bytes[0x140 + 28] = 0;
  REQUIRE_FALSE(FindStrbBlock(bytes, 3));
}
}  // namespace
