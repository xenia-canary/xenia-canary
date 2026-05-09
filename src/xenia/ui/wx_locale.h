/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WX_LOCALE_H_
#define XENIA_UI_WX_LOCALE_H_

namespace xe {
namespace ui {

// Initializes wxLocale from cvars::ui_locale. Call once at app init.
void InitializeWxLocale();

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WX_LOCALE_H_
