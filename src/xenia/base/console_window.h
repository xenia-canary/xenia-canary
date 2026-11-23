/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CONSOLE_WINDOW_H_
#define XENIA_BASE_CONSOLE_WINDOW_H_

#include <memory>

#include "xenia/base/cvar.h"

DECLARE_bool(enable_console);

namespace xe {

// Shows the stdout and stderr output of the process in a console window.
class ConsoleWindow {
 public:
  static std::unique_ptr<ConsoleWindow> Create();

  // Must be called after the config has been loaded.
  static void OpenIfEnabled();

  ConsoleWindow(const ConsoleWindow&) = delete;
  ConsoleWindow& operator=(const ConsoleWindow&) = delete;
  virtual ~ConsoleWindow() = default;

  // Also returns true if the process already had a console.
  virtual bool Open() = 0;

 protected:
  ConsoleWindow() = default;
};

}  // namespace xe

#endif  // XENIA_BASE_CONSOLE_WINDOW_H_
