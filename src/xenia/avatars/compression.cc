/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/compression.h"
#include "xenia/cpu/lzx.h"

namespace xe {
namespace avatars {
namespace compression {

struct BlockHeader {
  le<uint32_t> compressed_size;
  le<uint32_t> uncompressed_offset;
  le<uint32_t> uncompressed_size;
};
static_assert_size(BlockHeader, 12);

bool GetUncompressedSize(const uint8_t* input_buffer, size_t input_size,
                         size_t& output_size) {
  if (input_size < sizeof(BlockHeader)) {
    return false;
  }
  output_size = 0;
  size_t offset = 0;
  do {
    const auto& block_header = *(BlockHeader*)&input_buffer[offset];
    output_size += block_header.uncompressed_size;
    offset += sizeof(BlockHeader);
    offset += block_header.compressed_size;
  } while (offset + sizeof(BlockHeader) <= input_size);
  return true;
}

bool Decompress(const uint8_t* input_buffer, size_t input_size,
                uint8_t* output_buffer, size_t output_size) {
  size_t input_offset = 0;
  size_t output_offset = 0;
  do {
    const auto& block_header = *(BlockHeader*)&input_buffer[input_offset];
    input_offset += sizeof(BlockHeader);

    assert_true(output_offset + block_header.uncompressed_size <= output_size);
    assert_true(output_offset == block_header.uncompressed_offset);

    auto result = lzx_decompress(
        &input_buffer[input_offset], block_header.compressed_size,
        &output_buffer[output_offset], block_header.uncompressed_size, 0x8000,
        nullptr, 0);
    assert_zero(result);
    if (result) {
      return false;
    }

    input_offset += block_header.compressed_size;
    output_offset += block_header.uncompressed_size;
  } while (input_offset < input_size);
  return true;
}

}  // namespace compression
}  // namespace avatars
}  // namespace xe
