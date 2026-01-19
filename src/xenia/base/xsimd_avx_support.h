/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XSIMD_AVX_SUPPORT_H_
#define XSIMD_AVX_SUPPORT_H_

#include <cstdint>
#include "xenia/base/platform.h"

namespace xe {
namespace xsimd_support {

enum class SIMDArchitecture : uint32_t {
  kUnknown = 0,
  kScalar = 1,   // No SIMD support
  kSSE2 = 2,     // SSE2
  kSSE3 = 3,     // SSE3
  kSSSE3 = 4,    // SSSE3
  kSSE4_1 = 5,   // SSE4.1
  kSSE4_2 = 6,   // SSE4.2
  kAVX = 7,      // AVX
  kAVX2 = 8,     // AVX2
  kAVX512 = 9,   // AVX512
  kNEON = 10,    // ARM NEON
  kNEON64 = 11,  // ARM64 NEON
};

bool IsAVXSupported();
bool IsAVX2Supported();
SIMDArchitecture GetBestSIMDArchitecture();
const char* GetSIMDArchitectureName(SIMDArchitecture arch);
void InitializeSIMDSupport();
bool IsRunningUnderRosetta2();

}  // namespace xsimd_support
}  // namespace xe

#endif  // XSIMD_AVX_SUPPORT_H_
