/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_BYTE_ORDER_H_
#define XENIA_BASE_BYTE_ORDER_H_

#include <cstdint>

#if defined(__has_include)
#define XE_BYTE_ORDER_HAS_INCLUDE_VERSION __has_include(<version>)
#define XE_BYTE_ORDER_HAS_INCLUDE_BIT __has_include(<bit>)
#define XE_BYTE_ORDER_HAS_INCLUDE_FMT_FORMAT __has_include(<fmt/format.h>)
#else
#define XE_BYTE_ORDER_HAS_INCLUDE_VERSION 0
#define XE_BYTE_ORDER_HAS_INCLUDE_BIT 0
#define XE_BYTE_ORDER_HAS_INCLUDE_FMT_FORMAT 0
#endif

#if XE_BYTE_ORDER_HAS_INCLUDE_VERSION
#include <version>
#endif
#if XE_BYTE_ORDER_HAS_INCLUDE_BIT
#include <bit>
#endif

#if defined(__cpp_lib_endian)
#define XE_BYTE_ORDER_HAS_STD_ENDIAN 1
#else
#define XE_BYTE_ORDER_HAS_STD_ENDIAN 0
#endif

#include "xenia/base/assert.h"
#include "xenia/base/platform.h"
#ifdef __APPLE__
#include <cstring>
#include <type_traits>
#include "third_party/endianness/endianness.h"
#endif

// Formatter for fmt to allow printing of xe::endian_store types.
#ifdef __APPLE__
#if XE_BYTE_ORDER_HAS_INCLUDE_FMT_FORMAT
#include <fmt/format.h>
#endif
#endif

#if !XE_BYTE_ORDER_HAS_STD_ENDIAN
// Polyfill
#ifdef __BYTE_ORDER__
namespace std {
enum class endian {
  little = __ORDER_LITTLE_ENDIAN__,
  big = __ORDER_BIG_ENDIAN__,
  native = __BYTE_ORDER__
};
}
#else
// Hardcode to little endian for now
namespace std {
enum class endian { little = 0, big = 1, native = 0 };
}
#endif
#endif
// Check for mixed endian
static_assert((std::endian::native == std::endian::big) ||
              (std::endian::native == std::endian::little));

namespace xe {

// chrispy: added workaround for clang, otherwise byteswap_ulong becomes calls
// to ucrtbase
#if XE_COMPILER_MSVC == 1 && !defined(__clang__)
#define XENIA_BASE_BYTE_SWAP_16 _byteswap_ushort
#define XENIA_BASE_BYTE_SWAP_32 _byteswap_ulong
#define XENIA_BASE_BYTE_SWAP_64 _byteswap_uint64
#else
#define XENIA_BASE_BYTE_SWAP_16 __builtin_bswap16
#define XENIA_BASE_BYTE_SWAP_32 __builtin_bswap32
#define XENIA_BASE_BYTE_SWAP_64 __builtin_bswap64
#endif  // XE_PLATFORM_WIN32

template <class T>
inline T byte_swap(T value) {
  static_assert(
      sizeof(T) == 8 || sizeof(T) == 4 || sizeof(T) == 2 || sizeof(T) == 1,
      "byte_swap(T value): Type T has illegal size");
#ifdef __APPLE__
  if constexpr (sizeof(T) == 1) {
    return value;
  } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
    // Universal integer/enum byte swap backend used throughout the project.
    return xe::endian::byte_swap(value);
  } else if constexpr (sizeof(T) == 2) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "byte_swap requires trivially copyable types");
    uint16_t temp = 0;
    std::memcpy(&temp, &value, sizeof(temp));
    temp = xe::endian::byte_swap(temp);
    T result;
    std::memcpy(&result, &temp, sizeof(result));
    return result;
  } else if constexpr (sizeof(T) == 4) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "byte_swap requires trivially copyable types");
    uint32_t temp = 0;
    std::memcpy(&temp, &value, sizeof(temp));
    temp = xe::endian::byte_swap(temp);
    T result;
    std::memcpy(&result, &temp, sizeof(result));
    return result;
  } else if constexpr (sizeof(T) == 8) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "byte_swap requires trivially copyable types");
    uint64_t temp = 0;
    std::memcpy(&temp, &value, sizeof(temp));
    temp = xe::endian::byte_swap(temp);
    T result;
    std::memcpy(&result, &temp, sizeof(result));
    return result;
  }
#else
  if constexpr (sizeof(T) == 8) {
    uint64_t temp =
        XENIA_BASE_BYTE_SWAP_64(*reinterpret_cast<uint64_t*>(&value));
    return *reinterpret_cast<T*>(&temp);
  } else if constexpr (sizeof(T) == 4) {
    uint32_t temp =
        XENIA_BASE_BYTE_SWAP_32(*reinterpret_cast<uint32_t*>(&value));
    return *reinterpret_cast<T*>(&temp);
  } else if constexpr (sizeof(T) == 2) {
    uint16_t temp =
        XENIA_BASE_BYTE_SWAP_16(*reinterpret_cast<uint16_t*>(&value));
    return *reinterpret_cast<T*>(&temp);
  } else if constexpr (sizeof(T) == 1) {
    return value;
  }
#endif
}

template <typename T, std::endian E>
struct endian_store {
#ifdef __APPLE__
  endian_store() = default;
  // implicit conversion from underlying type to make assignment convenient
  endian_store(const T& src) { set(src); }
  // Defaulted copy/move operations keep this type trivially copyable
  endian_store(const endian_store& other) = default;
  endian_store(endian_store&& other) noexcept = default;
  endian_store& operator=(const endian_store& other) = default;
  endian_store& operator=(endian_store&& other) noexcept = default;
  // assign from raw value
  endian_store& operator=(const T& src) {
    set(src);
    return *this;
  }
#else
  endian_store() = default;
  endian_store(const T& src) { set(src); }
  endian_store(const endian_store& other) { set(other); }
#endif
  operator T() const { return get(); }

  void set(const T& src) {
    if constexpr (std::endian::native == E) {
      value = src;
    } else {
      value = xe::byte_swap(src);
    }
  }
  void set(const endian_store& other) { value = other.value; }
  T get() const {
    if constexpr (std::endian::native == E) {
      return value;
    }
    return xe::byte_swap(value);
  }

  endian_store<T, E>& operator+=(int a) {
    *this = *this + a;
    return *this;
  }
  endian_store<T, E>& operator-=(int a) {
    *this = *this - a;
    return *this;
  }
  endian_store<T, E>& operator++() {
    *this += 1;
    return *this;
  }  // ++a
  endian_store<T, E> operator++(int) {
    *this += 1;
    return (*this - 1);
  }  // a++
  endian_store<T, E>& operator--() {
    *this -= 1;
    return *this;
  }  // --a
  endian_store<T, E> operator--(int) {
    *this -= 1;
    return (*this + 1);
  }  // a--

  T value;
};

template <typename T>
using be = endian_store<T, std::endian::big>;
template <typename T>
using le = endian_store<T, std::endian::little>;

}  // namespace xe

// Provide a fmt::formatter specialization so fmt can format endian_store by
// formatting the underlying value.
#ifdef __APPLE__
#if XE_BYTE_ORDER_HAS_INCLUDE_FMT_FORMAT
namespace fmt {
template <typename T, std::endian E, typename Char>
struct formatter<xe::endian_store<T, E>, Char> : formatter<T, Char> {
  template <typename FormatContext>
  auto format(const xe::endian_store<T, E>& v, FormatContext& ctx) {
    // Convert to underlying type then format.
    return formatter<T, Char>::format(static_cast<T>(v), ctx);
  }
};
}  // namespace fmt
#endif
#endif

#endif  // XENIA_BASE_BYTE_ORDER_H_
