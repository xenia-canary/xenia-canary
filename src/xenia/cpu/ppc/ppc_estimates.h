/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_ESTIMATES_H_
#define XENIA_CPU_PPC_PPC_ESTIMATES_H_

#include <cstdint>

namespace xe {
namespace cpu {
namespace ppc {

// The estimate instructions as the console computes them, bit for bit. The
// vector ones take and return float bits and treat denormal inputs as zero
// (VSCR[NJ], which titles run with).

// vrefp, vrefp128
uint32_t EstimateReciprocal(uint32_t value);
// vlogefp, vlogefp128
uint32_t EstimateLog2(uint32_t value);
// vexptefp, vexptefp128
uint32_t EstimatePow2(uint32_t value);
// fres: a single-precision result for a double input
double EstimateReciprocalSingle(double value);

}  // namespace ppc
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PPC_PPC_ESTIMATES_H_
