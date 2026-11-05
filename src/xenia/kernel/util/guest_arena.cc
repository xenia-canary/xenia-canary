/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/guest_arena.h"

#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {
namespace util {

GuestArena::GuestArena(KernelState* kernel_state, uint32_t guest_address,
                       uint32_t size)
    : kernel_state_(kernel_state) {
  if (!kernel_state_->memory()
           ->LookupHeap(guest_address)
           ->AllocFixed(guest_address, size, 0x1000, kMemoryAllocationCommit,
                        kMemoryProtectRead | kMemoryProtectWrite)) {
    return;
  }

  allocation_.emplace(
      kernel_state_->memory()->TranslateVirtual<void*>(guest_address), size,
      std::pmr::null_memory_resource());
}

GuestArena::~GuestArena() {
  if (allocation_) {
    allocation_->release();
  }
}

uint32_t GuestArena::Allocate(uint32_t byte_count) {
  if (!allocation_) {
    return 0;
  }

  return kernel_state_->memory()->HostToGuestVirtual(
      allocation_->allocate(byte_count));
}

uint32_t GuestArena::Write(std::string_view s) { return WriteChars(s); }
uint32_t GuestArena::Write(std::u16string_view s) { return WriteChars(s); }

template <CharType CharT>
uint32_t GuestArena::WriteChars(std::basic_string_view<CharT> s) {
  const size_t count = s.size() + 1;
  auto* dst = static_cast<CharT*>(
      allocation_->allocate(count * sizeof(CharT), alignof(CharT)));

  if constexpr (sizeof(CharT) == sizeof(char)) {
    std::memcpy(dst, s.data(), s.size());
    dst[s.size()] = CharT{};
  } else {
    string_util::copy_and_swap_truncating(dst, s, s.size() + 1);
  }

  return kernel_state_->memory()->HostToGuestVirtual(dst);
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
