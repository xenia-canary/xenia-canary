/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOWED_APP_CONTEXT_WX_H_
#define XENIA_UI_WINDOWED_APP_CONTEXT_WX_H_

#include <wx/wx.h>

#include "xenia/base/platform.h"
#include "xenia/ui/windowed_app_context.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace ui {

class WxWindowedAppContext final : public WindowedAppContext {
 public:
  WxWindowedAppContext();
  ~WxWindowedAppContext();

#if XE_PLATFORM_WIN32
  HINSTANCE hinstance() const { return hinstance_; }
#endif

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

 private:
#if XE_PLATFORM_WIN32
  HINSTANCE hinstance_ = nullptr;
#endif
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOWED_APP_CONTEXT_WX_H_
