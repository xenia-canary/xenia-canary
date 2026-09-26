/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <AppKit/AppKit.h>

#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_mac.h"

@interface XeniaApplicationDelegate : NSObject <NSApplicationDelegate>
@end

@implementation XeniaApplicationDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:
    (NSApplication*)sender {
  // Quitting from the menu or the Dock - close the windows instead so the app
  // shuts down through its usual path (the app quits when its main window is
  // closed) rather than exiting the process abruptly.
  for (NSWindow* window in [NSApp.windows copy]) {
    if (window.visible) {
      [window performClose:nil];
    }
  }
  return NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return NO;
}

@end

int main(int argc, char** argv) {
  int result;

  @autoreleasepool {
    [NSApplication sharedApplication];
    // Needed for a bare (not bundled) executable to have a Dock icon and a
    // menu bar, and to be able to receive keyboard focus.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    XeniaApplicationDelegate* app_delegate =
        [[XeniaApplicationDelegate alloc] init];
    NSApp.delegate = app_delegate;
    [NSApp finishLaunching];

    {
      xe::ui::MacWindowedAppContext app_context;

      std::unique_ptr<xe::ui::WindowedApp> app =
          xe::ui::GetWindowedAppCreator()(app_context);

      cvar::ParseLaunchArguments(argc, argv, app->GetPositionalOptionsUsage(),
                                 app->GetPositionalOptions());

      // Initialize logging. Needs parsed cvars.
      xe::InitializeLogging(app->GetName());

      if (app->OnInitialize()) {
        [NSApp activateIgnoringOtherApps:YES];
        app_context.RunMainCocoaLoop();
        result = EXIT_SUCCESS;
      } else {
        result = EXIT_FAILURE;
      }

      app->InvokeOnDestroy();
    }

    // Logging may still be needed in the destructors.
    xe::ShutdownLogging();
  }

  return result;
}
