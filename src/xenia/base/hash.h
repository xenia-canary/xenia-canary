/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_HASH_H_
#define XENIA_BASE_HASH_H_

#include <cstddef>
#include <cstdint>

#include "xenia/base/xxhash.h"

namespace xe {
namespace hash {

// For use in unordered_sets and unordered_maps (primarily multisets and
// multimaps, with manual collision resolution), where the hash is calculated
// externally (for instance, as XXH3), possibly requiring context data rather
// than a pure function to calculate the hash
template <typename Key>
struct IdentityHasher {
  size_t operator()(const Key& key) const { return static_cast<size_t>(key); }
};

template <typename Key>
struct XXHasher {
  size_t operator()(const Key& key) const {
    return static_cast<size_t>(XXH3_64bits(&key, sizeof(key)));
  }
};

// Spreads every input bit into the result.
constexpr uint64_t SplitMix64(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

}  // namespace hash
}  // namespace xe

#endif  // XENIA_BASE_HASH_H_
