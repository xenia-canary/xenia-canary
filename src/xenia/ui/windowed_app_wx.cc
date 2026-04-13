/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_wx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <wx/config.h>
#include <wx/fileconf.h>
#include <wx/image.h>

#include "xenia/base/platform.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/console.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#endif
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_wx.h"

#if XE_PLATFORM_WIN32
DECLARE_bool(enable_console);
#endif

namespace xe {
namespace ui {

#if !XE_PLATFORM_WIN32
int g_argc = 0;
char** g_argv = nullptr;
#endif

bool XeniaWxApp::OnInit() {
  // Skip wxApp::OnInit() — its default implementation parses argv with a
  // parser that doesn't know our cvars and would reject them. We do our own
  // parsing below, after the WindowedApp is constructed and can supply its
  // positional options.

  app_context_ = std::make_unique<WxWindowedAppContext>();
  app_ = GetWindowedAppCreator()(*app_context_);

#if XE_PLATFORM_WIN32
  // Use GetCommandLineW so Unicode paths in positional args survive.
  if (!xe::ParseWin32LaunchArguments(false, app_->GetPositionalOptionsUsage(),
                                     app_->GetPositionalOptions(), nullptr)) {
    return false;
  }
  xe::InitializeWin32App(app_->GetName());
  if (cvars::enable_console) {
    xe::AttachConsole();
  }
#else
  cvar::ParseLaunchArguments(g_argc, g_argv, app_->GetPositionalOptionsUsage(),
                             app_->GetPositionalOptions());
  xe::InitializeLogging(app_->GetName());
#endif

  wxInitAllImageHandlers();

  // Reduce wx event loop overhead -- only process idle events when explicitly
  // requested, not after every message. Critical for real-time rendering perf.
  wxIdleEvent::SetMode(wxIDLE_PROCESS_SPECIFIED);

  // wxPersistenceManager backing store. Windows: next to the exe to match
  // xenia's portable layout; other platforms use the wx default.
  std::string id(app_->GetTitle());
  std::transform(id.begin(), id.end(), id.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  const wxString id_wx = wxString::FromUTF8(id);
  const wxString vendor_wx = wxString::FromUTF8(app_->GetName());
  SetAppName(id_wx);
  SetVendorName(vendor_wx);
#if XE_PLATFORM_WIN32
  std::filesystem::path ini_path =
      xe::filesystem::GetExecutableFolder() / (id + ".ini");
  // path::u8string() is std::u8string in C++20; wx 3.2 takes const char*.
  auto u8 = ini_path.u8string();
  wxConfigBase::Set(new wxFileConfig(
      id_wx, vendor_wx,
      wxString::FromUTF8(reinterpret_cast<const char*>(u8.c_str()), u8.size()),
      wxEmptyString, wxCONFIG_USE_LOCAL_FILE));
#else
  wxConfigBase::Set(new wxFileConfig(id_wx, vendor_wx));
#endif

  if (!app_->OnInitialize()) {
    return false;
  }

  return true;
}

int XeniaWxApp::OnRun() {
#if XE_PLATFORM_WIN32
  // Use a tight Win32 message pump instead of wxApp's event loop to minimize
  // per-message overhead for real-time rendering.
  MSG message;
  while (!app_context_->HasQuitFromUIThread()) {
    BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == 0 || result == -1) {
      app_context_->QuitFromUIThread();
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return EXIT_SUCCESS;
#else
  return wxApp::OnRun();
#endif
}

int XeniaWxApp::OnExit() {
  if (app_) {
    app_->InvokeOnDestroy();
    app_.reset();
  }
  app_context_.reset();

#if XE_PLATFORM_WIN32
  xe::ShutdownWin32App();
#else
  xe::ShutdownLogging();
#endif

  return wxApp::OnExit();
}

}  // namespace ui
}  // namespace xe
