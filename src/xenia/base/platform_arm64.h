/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_PLATFORM_ARM64_H_
#define XENIA_BASE_PLATFORM_ARM64_H_
#include <cstdint>

namespace xe {
namespace arm64 {
enum A64FeatureFlags : uint64_t {
  kA64EmitLSE = 1 << 0,

};

XE_NOALIAS
uint64_t GetFeatureFlags();
XE_COLD
void InitFeatureFlags();

}  // namespace arm64
}  // namespace xe

#endif  // XENIA_BASE_PLATFORM_ARM64_H_
