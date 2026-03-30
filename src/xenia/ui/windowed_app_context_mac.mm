/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_mac.h"

#import <Cocoa/Cocoa.h>

namespace xe {
namespace ui {

MacWindowedAppContext::~MacWindowedAppContext() {
  // Execute any remaining pending functions before destruction.
  ExecutePendingFunctionsFromUIThread();
}

void MacWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Post a no-op event to wake the run loop so it processes pending functions.
  @autoreleasepool {
    NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:NO];
  }
}

void MacWindowedAppContext::PlatformQuitFromUIThread() {
  [NSApp stop:nil];
  // Post a dummy event to ensure the run loop wakes up and processes the stop.
  NotifyUILoopOfPendingFunctions();
}

void MacWindowedAppContext::RunMainLoop() {
  if (HasQuitFromUIThread()) {
    return;
  }
  [NSApp run];
  QuitFromUIThread();
}

}  // namespace ui
}  // namespace xe
