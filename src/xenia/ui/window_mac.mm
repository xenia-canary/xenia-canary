/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/window.h"

#include "xenia/base/logging.h"
#include "xenia/ui/menu_item.h"

namespace xe {
namespace ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  XELOGE("Window::Create: native macOS window not yet implemented");
  return nullptr;
}

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  // Return a base MenuItem — no native menu integration yet.
  return std::unique_ptr<MenuItem>(new MenuItem(type, text, hotkey, std::move(callback)));
}

}  // namespace ui
}  // namespace xe
