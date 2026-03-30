/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>

#import <Cocoa/Cocoa.h>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_mac.h"

@interface XeniaAppDelegate : NSObject <NSApplicationDelegate> {
  xe::ui::MacWindowedAppContext* app_context_;
  std::unique_ptr<xe::ui::WindowedApp> app_;
}
- (instancetype)initWithContext:(xe::ui::MacWindowedAppContext*)context
                           argc:(int)argc
                           argv:(char**)argv;
@end

@implementation XeniaAppDelegate

- (instancetype)initWithContext:(xe::ui::MacWindowedAppContext*)context
                           argc:(int)argc
                           argv:(char**)argv {
  self = [super init];
  if (self) {
    app_context_ = context;
    app_ = xe::ui::GetWindowedAppCreator()(*context);

    cvar::ParseLaunchArguments(argc, argv, app_->GetPositionalOptionsUsage(),
                               app_->GetPositionalOptions());
    xe::InitializeLogging(app_->GetName());
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  if (!app_->OnInitialize()) {
    [NSApp terminate:nil];
  }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  app_->InvokeOnDestroy();
  return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  return YES;
}

@end

int main(int argc, char** argv) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    xe::ui::MacWindowedAppContext app_context;
    XeniaAppDelegate* delegate = [[XeniaAppDelegate alloc] initWithContext:&app_context
                                                                      argc:argc
                                                                      argv:argv];
    [NSApp setDelegate:delegate];
    app_context.RunMainLoop();
  }

  xe::ShutdownLogging();
  return EXIT_SUCCESS;
}
