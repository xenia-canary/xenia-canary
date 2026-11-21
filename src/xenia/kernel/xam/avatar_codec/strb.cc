/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_codec/strb.h"

#include <bit>
#include <cstddef>
#include <cstring>

namespace xe::kernel::xam::avatar::codec {
namespace {

bool HasTag(std::span<const uint8_t> bytes, const char (&tag)[5]) {
  return bytes.size() >= 4 && std::memcmp(bytes.data(), tag, 4) == 0;
}

bool IsFieldWidth(uint8_t width) {
  return width == 1 || width == 2 || width == 4 || width == 8;
}

uint64_t ReadField(std::span<const uint8_t> bytes, bool little_endian) {
  uint64_t value = 0;
  for (size_t i = 0; i < bytes.size(); ++i) {
    value = (value << 8) | bytes[little_endian ? bytes.size() - 1 - i : i];
  }
  return value;
}

// Consume one field, including any padding up to the next block boundary.
bool Advance(std::span<const uint8_t>& input, size_t size, size_t alignment,
             bool final_padding_optional = false) {
  if (size > input.size()) {
    return false;
  }
  input = input.subspan(size);
  if (final_padding_optional && input.empty()) {
    return true;
  }
  const size_t padding = (0 - size) & (alignment - 1);
  if (padding > input.size()) {
    return false;
  }
  input = input.subspan(padding);
  return true;
}

}  // namespace

std::optional<std::span<const uint8_t>> FindStrbBlock(
    std::span<const uint8_t> bytes, uint64_t wanted_id) {
  // STRB header/field widths: gibbed/xenia bc08cec2, avatars/strb.cc.
  if (HasTag(bytes, "YTGR")) {
    constexpr size_t kPrefixSize = 0x140;
    if (bytes.size() < kPrefixSize) {
      return std::nullopt;
    }
    bytes = bytes.subspan(kPrefixSize);
  }
  if (bytes.size() < 26 || !HasTag(bytes, "STRB")) {
    return std::nullopt;
  }
  const bool aligned = bytes[4] != 0;
  const bool little_endian = bytes[5] != 0;
  const size_t id_width = bytes[22], length_width = bytes[23];
  if (!IsFieldWidth(bytes[22]) || !IsFieldWidth(bytes[23]) ||
      (aligned && bytes.size() < 30)) {
    return std::nullopt;
  }
  const size_t alignment = aligned ? bytes[26] : 1;
  if (!std::has_single_bit(alignment) ||
      !Advance(bytes, aligned ? 30 : 26, alignment)) {
    return std::nullopt;
  }
  std::optional<std::span<const uint8_t>> result;
  const size_t header_size = id_width + 2 * length_width;
  while (!bytes.empty()) {
    const auto header = bytes;
    if (!Advance(bytes, header_size, alignment)) {
      return std::nullopt;
    }
    const auto id = ReadField(header.first(id_width), little_endian);
    const auto length =
        ReadField(header.subspan(id_width, length_width), little_endian);
    const auto stride = ReadField(
        header.subspan(id_width + length_width, length_width), little_endian);
    // Velocity cf0b84cc, AvatarAsset::readBlocks and ReadBlockData, treat
    // length as bytes and stride as the size of one element.
    if (!length || !stride || length % stride || length > bytes.size()) {
      return std::nullopt;
    }
    const auto size = static_cast<size_t>(length);
    if (id == wanted_id && !result) {
      result = bytes.first(size);
    }
    if (!Advance(bytes, size, alignment, true)) {
      return std::nullopt;
    }
  }
  return result;
}

}  // namespace xe::kernel::xam::avatar::codec
