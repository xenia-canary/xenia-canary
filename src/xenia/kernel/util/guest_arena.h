/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_GUEST_ARENA_H_
#define XENIA_KERNEL_UTIL_GUEST_ARENA_H_

#include <concepts>
#include <memory_resource>
#include <optional>
#include <string_view>

namespace xe {
namespace kernel {
class KernelState;
}
}  // namespace xe

namespace xe {
namespace kernel {
namespace util {

template <typename T>
concept CharType = std::same_as<T, char> || std::same_as<T, char8_t> ||
                   std::same_as<T, char16_t>;

class GuestArena {
 public:
  GuestArena(KernelState* kernel_state, uint32_t guest_address, uint32_t size);
  ~GuestArena();

  uint32_t Allocate(uint32_t byte_count);
  // Add scalar types when needed!
  uint32_t Write(std::string_view s);
  uint32_t Write(std::u16string_view s);

  bool is_valid() const { return allocation_.has_value(); }

 private:
  template <CharType CharT>
  uint32_t WriteChars(std::basic_string_view<CharT> s);

  KernelState* kernel_state_;
  std::optional<std::pmr::monotonic_buffer_resource> allocation_;
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_GUEST_ARENA_H_
