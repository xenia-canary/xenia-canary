/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>

#include "xenia/ui/windowed_app_wx.h"

namespace xe {
namespace ui {

void SuppressMacAutoMenuItems() {
  // AppKit auto-injects "Show Tab Bar" / "Show All Tabs" into a "View" menu
  // when window tabbing is allowed; we're a single-window app and don't want
  // them.
  if ([NSWindow respondsToSelector:@selector(setAllowsAutomaticWindowTabbing:)]) {
    [NSWindow setAllowsAutomaticWindowTabbing:NO];
  }

  // AppKit auto-injects "Enter Full Screen" into a menu literally named
  // "View" unless this default is off; fullscreen is driven by our own UI.
  [[NSUserDefaults standardUserDefaults] registerDefaults:@{
    @"NSFullScreenMenuItemEverywhere" : @NO
  }];
}

void ActivateMacApp() {
  // exec'd children and non-LaunchServices launches don't auto-foreground.
  [NSApp activateIgnoringOtherApps:YES];
}

}  // namespace ui
}  // namespace xe
