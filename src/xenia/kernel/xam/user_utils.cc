/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/user_utils.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

int32_t GetUserLanguage(KernelState* kernel_state) {
  if (kernel_state && kernel_state->xam_state()) {
    // Try to find the first signed-in user (indices 0-3)
    for (uint32_t i = 0; i < 4; ++i) {
      auto user = kernel_state->xam_state()->GetUserProfile(i);
      if (user) {
        int32_t language = user->GetLanguage();
        if (language != 0) {
          return language;
        }
      }
    }
  }
  // Default to English if no user is signed in or language is invalid
  return static_cast<int32_t>(XLanguage::kEnglish);
}

int32_t GetUserCountry(KernelState* kernel_state) {
  if (kernel_state && kernel_state->xam_state()) {
    // Try to find the first signed-in user (indices 0-3)
    for (uint32_t i = 0; i < 4; ++i) {
      auto user = kernel_state->xam_state()->GetUserProfile(i);
      if (user) {
        int32_t country = user->GetCountry();
        if (country != 0) {
          return country;
        }
      }
    }
  }
  // Default to US if no user is signed in or country is invalid
  return static_cast<int32_t>(XOnlineCountry::kUnitedStates);
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
