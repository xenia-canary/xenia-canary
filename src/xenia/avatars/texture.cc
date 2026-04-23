/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/texture.h"

#include "xenia/avatars/bit_stream.h"
#include "xenia/avatars/common.h"
#include "xenia/avatars/compression.h"
#include "xenia/avatars/strb.h"
#include "xenia/base/logging.h"

namespace xe {
namespace avatars {

Texture Texture::Read(BitStream& stream) {
  Texture instance{};
  instance.format = stream.Read<uint32_t>();
  instance.width = stream.Read<uint32_t>();
  instance.height = stream.Read<uint32_t>();
  instance.total_data_size = stream.Read<uint32_t>();
  instance.data_size = stream.Read<uint32_t>();
  instance.layer_count = stream.Read<uint32_t>();
  instance.is_empty = stream.Read<bool>();
  instance.is_tiled = stream.Read<bool>();
  instance.data_stride = stream.Read<uint32_t>();
  instance.data_rows = stream.Read<uint32_t>();
  stream.AlignToNextByte();

  assert_true(instance.width <= 1024);
  assert_true(instance.height <= 1024);
  assert_true(instance.layer_count >= 1 && instance.layer_count <= 14);

  if (!instance.is_empty) {
    size_t data_size = instance.data_stride;
    data_size *= instance.data_rows;
    data_size *= instance.layer_count;
    size_t data_bit_size = data_size * 8;
    assert_true(stream.offset_bits() + data_bit_size <= stream.size_bits());
    instance.data_bytes.resize(data_size);
    std::memcpy(instance.data_bytes.data(),
                &stream.buffer()[stream.offset_bits() >> 3], data_size);
    stream.Advance(data_bit_size);
  }

  return instance;
}

std::shared_ptr<Texture> Texture::Read(const uint8_t* data_buffer,
                                       size_t data_size) {
  BitStream stream(data_buffer, data_size * 8);

  auto instance = Read(stream);

  assert_true(stream.offset_bits() == stream.size_bits());

  return std::make_shared<Texture>(instance);
}

std::shared_ptr<Texture> Texture::Load(const uint8_t* strb_buffer,
                                       size_t strb_size) {
  const uint8_t* compressed_buffer;
  size_t compressed_size;
  if (!strb::GetSTRBBlock(strb_buffer, strb_size, strb::STRBBlockId::kTexture,
                          compressed_buffer, compressed_size)) {
    return nullptr;
  }

  size_t data_size;
  if (!compression::GetUncompressedSize(compressed_buffer, compressed_size,
                                        data_size)) {
    assert_always();
    XELOGE("Failed to get uncompressed size for avatar texture!");
    return nullptr;
  }

  std::vector<uint8_t> data_bytes(data_size);
  if (!compression::Decompress(compressed_buffer, compressed_size,
                               data_bytes.data(), data_size)) {
    assert_always();
    XELOGE("Failed to decompress avatar texture!");
    return nullptr;
  }

  return Read(data_bytes.data(), data_bytes.size());
}

}  // namespace avatars
}  // namespace xe
