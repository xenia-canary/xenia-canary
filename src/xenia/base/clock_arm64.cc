/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/clock.h"
#include "xenia/base/platform.h"

#if XE_ARCH_ARM64 && XE_CLOCK_RAW_AVAILABLE

// Wrap all these different cpu compiler intrinsics.
// So no inline assembler here and the compiler will remove the clutter.
#if XE_COMPILER_MSVC
#include <intrin.h>
constexpr uint32_t CNTFRQ_EL0 = ARM64_SYSREG(3, 3, 14, 0, 0);
constexpr uint32_t CNTVCT_EL0 = ARM64_SYSREG(3, 3, 14, 0, 2);
#define xe_cpu_mrs(reg) _ReadStatusReg(reg)
#elif XE_COMPILER_CLANG || XE_COMPILER_GNUC
#include <arm_acle.h>
#define xe_cpu_mrs(reg) __arm_rsr64(#reg)
#else
#error "No cpu instruction wrappers for current compiler implemented."
#endif

namespace xe {
XE_NOINLINE
uint64_t Clock::host_tick_frequency_raw() { return xe_cpu_mrs(CNTFRQ_EL0); }
XE_NOINLINE
uint64_t Clock::host_tick_count_raw() { return xe_cpu_mrs(CNTVCT_EL0); }

}  // namespace xe

#endif
