/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_USER_UTILS_H_
#define XENIA_KERNEL_XAM_USER_UTILS_H_

#include <cstdint>

#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {
namespace xam {

// Gets the language from the first signed-in user profile (or defaults to
// XLanguage::kEnglish if no user is signed in or kernel_state is null).
// See XLanguage enum in xenia/xbox.h for valid values.
int32_t GetUserLanguage(KernelState* kernel_state);

// Gets the country from the first signed-in user profile (or defaults to
// XOnlineCountry::kUnitedStates if no user is signed in or kernel_state is
// null). See XOnlineCountry enum in xenia/xbox.h for valid values.
int32_t GetUserCountry(KernelState* kernel_state);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_USER_UTILS_H_
