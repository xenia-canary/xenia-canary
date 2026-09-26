/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_mac.h"

#import <AppKit/AppKit.h>

namespace xe {
namespace ui {

MacWindowedAppContext::MacWindowedAppContext()
    : link_(std::make_shared<Link>(Link{this})) {}

MacWindowedAppContext::~MacWindowedAppContext() {
  // Dispatched blocks that haven't run yet must not access the context.
  link_->context = nullptr;
}

void MacWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Coalesce notifications until the dispatched block starts executing.
  if (pending_functions_dispatched_.exchange(true)) {
    return;
  }
  std::shared_ptr<Link> link = link_;
  dispatch_async(dispatch_get_main_queue(), ^{
    MacWindowedAppContext* context = link->context;
    if (!context) {
      return;
    }
    // Allow new notifications before executing, so ones made during the
    // execution of the functions aren't missed.
    context->pending_functions_dispatched_.store(false);
    context->ExecutePendingFunctionsFromUIThread();
  });
}

void MacWindowedAppContext::PlatformQuitFromUIThread() {
  if (!is_running_) {
    return;
  }
  [NSApp stop:nil];
  // -stop: only takes effect after the next event is processed.
  NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
  [NSApp postEvent:event atStart:YES];
}

void MacWindowedAppContext::RunMainCocoaLoop() {
  // For safety, in case the quit request somehow happened before the loop.
  if (HasQuitFromUIThread()) {
    return;
  }
  is_running_ = true;
  [NSApp run];
  is_running_ = false;
  // Something else - not QuitFromUIThread - might have stopped the loop. Let
  // the context know, so pending functions won't be added pointlessly.
  QuitFromUIThread();
}

}  // namespace ui
}  // namespace xe
