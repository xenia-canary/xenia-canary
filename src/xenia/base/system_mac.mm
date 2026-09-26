/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <AppKit/AppKit.h>
#include <CoreFoundation/CoreFoundation.h>

#include <string>

#include "xenia/base/system.h"

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  @autoreleasepool {
    NSString* url_string =
        [[NSString alloc] initWithBytes:url.data()
                                 length:url.size()
                               encoding:NSUTF8StringEncoding];
    NSURL* ns_url = url_string ? [NSURL URLWithString:url_string] : nil;
    if (ns_url) {
      [[NSWorkspace sharedWorkspace] openURL:ns_url];
    }
  }
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  @autoreleasepool {
    NSURL* ns_url = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:path.string().c_str()]];
    if (ns_url) {
      [[NSWorkspace sharedWorkspace] openURL:ns_url];
    }
  }
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  // CFUserNotification can be used from any thread, unlike NSAlert, and this
  // may be called before the application event loop is running.
  CFStringRef title;
  CFOptionFlags flags;
  switch (type) {
    default:
    case SimpleMessageBoxType::Help:
      title = CFSTR("Xenia Help");
      flags = kCFUserNotificationNoteAlertLevel;
      break;
    case SimpleMessageBoxType::Warning:
      title = CFSTR("Xenia Warning");
      flags = kCFUserNotificationCautionAlertLevel;
      break;
    case SimpleMessageBoxType::Error:
      title = CFSTR("Xenia Error");
      flags = kCFUserNotificationStopAlertLevel;
      break;
  }
  CFStringRef message_string = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(message.data()),
      CFIndex(message.size()), kCFStringEncodingUTF8, false);
  CFOptionFlags response_flags;
  CFUserNotificationDisplayAlert(0, flags, nullptr, nullptr, nullptr, title,
                                 message_string, nullptr, nullptr, nullptr,
                                 &response_flags);
  if (message_string) {
    CFRelease(message_string);
  }
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
