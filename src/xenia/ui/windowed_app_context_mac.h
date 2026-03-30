/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOWED_APP_CONTEXT_MAC_H_
#define XENIA_UI_WINDOWED_APP_CONTEXT_MAC_H_

#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace ui {

class MacWindowedAppContext final : public WindowedAppContext {
 public:
  MacWindowedAppContext() = default;
  ~MacWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

  void RunMainLoop();
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOWED_APP_CONTEXT_MAC_H_
