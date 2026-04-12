/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_wx.h"

namespace xe {
namespace ui {

WxWindowedAppContext::WxWindowedAppContext() {
#if XE_PLATFORM_WIN32
  hinstance_ = GetModuleHandle(nullptr);
#endif
}

WxWindowedAppContext::~WxWindowedAppContext() = default;

void WxWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Post an async call to the wx event loop to drain the pending function
  // queue. wxEvtHandler::CallAfter is thread-safe.
  wxTheApp->CallAfter([this]() { ExecutePendingFunctionsFromUIThread(); });
}

void WxWindowedAppContext::PlatformQuitFromUIThread() {
  wxTheApp->ExitMainLoop();
}

}  // namespace ui
}  // namespace xe
