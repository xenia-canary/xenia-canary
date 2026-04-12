/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOWED_APP_WX_H_
#define XENIA_UI_WINDOWED_APP_WX_H_

#include <memory>

#include <wx/wx.h>

#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_wx.h"

namespace xe {
namespace ui {

#if !XE_PLATFORM_WIN32
// Set by main() before wxEntry so OnInit can parse cvars from the original
// argv with proper positional options. Windows uses GetCommandLineW() instead.
extern int g_argc;
extern char** g_argv;
#endif

// wxApp subclass that drives the Xenia windowed application lifecycle.
// Platform-specific entry points (wWinMain, main) call wxEntry() which
// creates this app, runs OnInit -> MainLoop -> OnExit.
class XeniaWxApp : public wxApp {
 public:
  bool OnInit() override;
  int OnRun() override;
  int OnExit() override;

 private:
  std::unique_ptr<WxWindowedAppContext> app_context_;
  std::unique_ptr<WindowedApp> app_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOWED_APP_WX_H_
