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

#include <atomic>
#include <memory>

#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace ui {

// Runs the AppKit event loop on the main thread, which is the only thread
// AppKit can be used from.
class MacWindowedAppContext final : public WindowedAppContext {
 public:
  MacWindowedAppContext();
  ~MacWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;

  void PlatformQuitFromUIThread() override;

  void RunMainCocoaLoop();

 private:
  // Shared with the blocks dispatched to the main queue, which may outlive the
  // context. Only accessed on the main (UI) thread.
  struct Link {
    MacWindowedAppContext* context;
  };
  std::shared_ptr<Link> link_;

  std::atomic<bool> pending_functions_dispatched_{false};
  bool is_running_ = false;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOWED_APP_CONTEXT_MAC_H_
