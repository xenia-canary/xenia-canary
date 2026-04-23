/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_TEXTURE_H_
#define XENIA_AVATARS_TEXTURE_H_

#include <memory>
#include <vector>

#include "xenia/avatars/common.h"
#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

class BitStream;

struct Texture {
 public:
  uint32_t format;
  uint32_t width;
  uint32_t height;
  uint32_t total_data_size;
  uint32_t data_size;
  uint32_t layer_count;
  bool is_empty;
  bool is_tiled;
  uint32_t data_stride;
  uint32_t data_rows;
  std::vector<uint8_t> data_bytes;

 public:
  static Texture Read(BitStream& stream);

 public:
  static std::shared_ptr<Texture> Load(const uint8_t* strb_buffer,
                                       size_t strb_size);

 private:
  static std::shared_ptr<Texture> Read(const uint8_t* data_buffer,
                                       size_t data_size);
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_TEXTURE_H_
