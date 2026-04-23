/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_COMPRESSION_H_
#define XENIA_AVATARS_COMPRESSION_H_

#include "xenia/base/memory.h"

namespace xe {
namespace avatars {
namespace compression {

bool GetUncompressedSize(const uint8_t* input, size_t input_size,
                         size_t& output_size);
bool Decompress(const uint8_t* input, size_t input_size, uint8_t* output,
                size_t output_size);

}  // namespace compression
}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_AVATAR_INFO_H_
