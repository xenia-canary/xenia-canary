/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/console_window.h"

DEFINE_bool(enable_console, false, "Open a console window with the main window",
            "Logging");

namespace xe {
namespace ui {

void ConsoleWindow::OpenIfEnabled() {
  if (!cvars::enable_console) {
    return;
  }
  std::unique_ptr<ConsoleWindow> console_window = Create();
  if (console_window) {
    console_window->Open();
  }
}

}  // namespace ui
}  // namespace xe
