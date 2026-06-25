/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app.h"

namespace xe {
namespace ui {

#if XE_UI_WINDOWED_APPS_IN_LIBRARY
// A zero-initialized pointer to remove dependence on the initialization order
// of the map relatively to the app creator proxies.
std::unordered_map<std::string, WindowedApp::Creator>* WindowedApp::creators_;
#else
// Dummy symbol to prevent "no symbols" error on platforms that don't use
// windowed apps in library mode (non-Android platforms).
// This ensures the object file always contains at least one symbol.
__attribute__((used)) static const int windowed_app_dummy_symbol = 0;
#endif  // XE_UI_WINDOWED_APPS_IN_LIBRARY

}  // namespace ui
}  // namespace xe
