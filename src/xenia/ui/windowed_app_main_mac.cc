/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>

#include <wx/wx.h>

#include "xenia/ui/windowed_app_wx.h"

// wxWidgets app instance - XeniaWxApp handles OnInit/OnExit lifecycle.
// Cvar/logging init and command-line parsing happen in XeniaWxApp::OnInit
// after the WindowedApp is constructed and can supply its positional options.
wxIMPLEMENT_APP_NO_MAIN(xe::ui::XeniaWxApp);

int main(int argc, char** argv) {
  xe::ui::g_argc = argc;
  xe::ui::g_argv = argv;

  return wxEntry(argc, argv);
}
