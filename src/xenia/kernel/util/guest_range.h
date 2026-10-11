/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_GUEST_RANGE_H_
#define XENIA_KERNEL_UTIL_GUEST_RANGE_H_

#include <cstdint>

#include "xenia/base/memory.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {
namespace util {

// Reject null, empty, uncommitted, and inaccessible guest ranges.
inline bool IsGuestRangeReadable(Memory* memory, uint32_t address,
                                 uint32_t length) {
  if (!address || !length) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  return heap->QueryRangeAccess(address, address + length - 1) !=
         xe::memory::PageAccess::kNoAccess;
}

inline bool IsGuestRangeWritable(Memory* memory, uint32_t address,
                                 uint32_t length) {
  if (!address || !length) {
    return false;
  }
  auto* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  const auto access = heap->QueryRangeAccess(address, address + length - 1);
  return access == xe::memory::PageAccess::kReadWrite ||
         access == xe::memory::PageAccess::kExecuteReadWrite;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_GUEST_RANGE_H_
