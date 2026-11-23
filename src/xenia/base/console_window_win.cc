/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/console.h"
#include "xenia/base/console_window.h"

namespace xe {

namespace {

class Win32ConsoleWindow final : public ConsoleWindow {
 public:
  bool Open() override {
    xe::AttachConsole();
    return xe::has_console_attached();
  }
};

}  // namespace

std::unique_ptr<ConsoleWindow> ConsoleWindow::Create() {
  return std::make_unique<Win32ConsoleWindow>();
}

}  // namespace xe
