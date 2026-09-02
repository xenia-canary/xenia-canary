/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_mac.h"

@interface XeniaAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation XeniaAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  return NSTerminateCancel;
}
@end

@interface XeniaRunLoopPumper : NSObject
@property (nonatomic, assign) xe::ui::MacWindowedAppContext* appContext;
- (void)pump:(NSTimer*)timer;
@end

@implementation XeniaRunLoopPumper
- (void)pump:(NSTimer*)timer {
  if (_appContext && !_appContext->HasQuitFromUIThread()) {
    _appContext->ExecutePendingFunctionsFromUIThread();
  }
}
@end

namespace xe {
namespace ui {

MacWindowedAppContext::MacWindowedAppContext() {
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  XeniaAppDelegate* delegate = [[XeniaAppDelegate alloc] init];
  [NSApp setDelegate:delegate];
  [NSApp finishLaunching];
}

MacWindowedAppContext::~MacWindowedAppContext() = default;

void MacWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!HasQuitFromUIThread()) {
      ExecutePendingFunctionsFromUIThread();
    }
  });
}

void MacWindowedAppContext::PlatformQuitFromUIThread() {
  [NSApp stop:nil];
  NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSMakePoint(0, 0)
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
  [NSApp postEvent:event atStart:YES];
}

}  // namespace ui
}  // namespace xe

int main(int argc, char** argv) {
  int result = EXIT_SUCCESS;

  @autoreleasepool {
    xe::ui::MacWindowedAppContext app_context;

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    cvar::ParseLaunchArguments(argc, argv,
                               app->GetPositionalOptionsUsage(),
                               app->GetPositionalOptions());

    xe::InitializeLogging(app->GetName());

    [NSApp activateIgnoringOtherApps:YES];

    if (app->OnInitialize()) {
      XeniaRunLoopPumper* pumper = [[XeniaRunLoopPumper alloc] init];
      pumper.appContext = &app_context;
      NSTimer* timer = [NSTimer timerWithTimeInterval:0.005
                                               target:pumper
                                             selector:@selector(pump:)
                                             userInfo:nil
                                              repeats:YES];
      [[NSRunLoop currentRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];

      while (!app_context.HasQuitFromUIThread()) {
        @autoreleasepool {
          while (true) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate distantPast]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event) {
              break;
            }
            [NSApp sendEvent:event];
          }
          [NSApp updateWindows];
        }
        app_context.ExecutePendingFunctionsFromUIThread();
        xe::threading::Sleep(std::chrono::milliseconds(1));
      }
      [timer invalidate];
      app->InvokeOnDestroy();
    } else {
      result = EXIT_FAILURE;
    }
  }

  return result;
}
