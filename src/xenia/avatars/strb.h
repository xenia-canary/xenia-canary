/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_STRB_H_
#define XENIA_AVATARS_STRB_H_

#include <cstdint>

#include "xenia/base/memory.h"

namespace xe {
namespace avatars {
namespace strb {

enum class STRBBlockId : size_t {
  kInvalid = 0,
  kAnimation = 1,
  kTexture = 2,
  kModel = 3,
  kShapeOverrides = 4,
  kSkeleton = 5,
  kAssetMetadataUnversioned = 6,
  kCustomColorTable = 7,
  kAssetMetadataVersioned = 8,
};

bool GetSTRBBlock(const uint8_t* strb_buffer, size_t strb_size,
                  STRBBlockId target_id, const uint8_t*& target_buffer,
                  size_t& target_size);

}  // namespace strb
}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_STRB_H_
