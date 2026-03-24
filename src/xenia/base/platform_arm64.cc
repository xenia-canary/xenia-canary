/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"
#define XBYAK_NO_OP_NAMES
#include "third_party/xbyak_aarch64/xbyak_aarch64/xbyak_aarch64.h"
#include "third_party/xbyak_aarch64/xbyak_aarch64/xbyak_aarch64_util.h"
DEFINE_int64(a64_extension_mask, -1LL,
             "Allow the detection and utilization of specific instruction set "
             "features.\n"
             "    0 = armv8.0\n"
             "    1 = Large System Extensions(LSE) atomic operations\n"
             "   -1 = Detect and utilize all possible processor features\n",
             "a64");
namespace xe {
namespace arm64 {
static uint64_t g_feature_flags = 0U;
static bool g_did_initialize_feature_flags = false;
uint64_t GetFeatureFlags() {
  if (!g_did_initialize_feature_flags) {
    InitFeatureFlags();
  }
  return g_feature_flags;
}
XE_COLD
XE_NOINLINE
void InitFeatureFlags() {
  uint64_t feature_flags_ = 0U;
  {
    Xbyak_aarch64::util::Cpu cpu_;
#define TEST_EMIT_FEATURE(emit, ext)                \
  if ((cvars::a64_extension_mask & emit) == emit) { \
    feature_flags_ |= (cpu_.has(ext) ? emit : 0);   \
  }
    TEST_EMIT_FEATURE(kA64EmitLSE,
                      Xbyak_aarch64::util::XBYAK_AARCH64_HWCAP_ATOMIC);
#undef TEST_EMIT_FEATURE
  }
  g_feature_flags = feature_flags_;
  g_did_initialize_feature_flags = true;
}
}  // namespace arm64
}  // namespace xe
