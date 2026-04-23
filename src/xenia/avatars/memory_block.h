/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_AVATARS_MEMORY_BLOCK_H_
#define XENIA_AVATARS_MEMORY_BLOCK_H_

#include <vector>

#include "xenia/base/memory.h"

namespace xe {
namespace avatars {

class MemoryBlock {
 public:
  MemoryBlock(uint8_t* buffer, size_t alignment)
      : buffer_(buffer),
        alignment_(alignment),
        current_offset_(0),
        offset_pointers_() {}

  uint8_t* ClaimBytes(uint32_t* claimed_offset, size_t size) {
    auto current_offset = current_offset_;
    auto claim = &buffer_[current_offset];
    current_offset_ += align(size, alignment_);
    if (claimed_offset != nullptr)
      *claimed_offset = static_cast<uint32_t>(current_offset);
    return claim;
  }

  template <typename T>
  T* Claim(uint32_t* claimed_offset = nullptr, size_t count = 1) {
    auto claim = ClaimBytes(claimed_offset, sizeof(T) * count);
    return reinterpret_cast<T*>(claim);
  }

  void SetPointer(be<uint32_t>* pointer, uint32_t offset) {
    *pointer = offset;
    offset_pointers_.push_back(pointer);
  }

  void ResolvePointers(uint32_t base_address) {
    for (const auto& offset_pointer : offset_pointers_) {
      if (offset_pointer) {
        be<uint32_t> offset = *offset_pointer;
        *offset_pointer = base_address + offset;
      }
    }
  }

 private:
  uint8_t* buffer_;
  size_t alignment_;
  size_t current_offset_;
  std::vector<be<uint32_t>*> offset_pointers_;
};

}  // namespace avatars
}  // namespace xe

#endif  // XENIA_AVATARS_MEMORY_BLOCK_H_
