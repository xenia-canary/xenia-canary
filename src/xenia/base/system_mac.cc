/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <cstring>
#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/system.h"

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  CFStringRef url_cf = CFStringCreateWithCString(
      kCFAllocatorDefault, url.data(), kCFStringEncodingUTF8);
  if (!url_cf) return;

  CFURLRef url_ref =
      CFURLCreateWithString(kCFAllocatorDefault, url_cf, nullptr);
  if (url_ref) {
    LSOpenCFURLRef(url_ref, nullptr);
    CFRelease(url_ref);
  }

  CFRelease(url_cf);
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  std::string path_str = path.string();
  CFStringRef path_cf = CFStringCreateWithCString(
      kCFAllocatorDefault, path_str.c_str(), kCFStringEncodingUTF8);
  if (!path_cf) return;

  CFURLRef url_ref = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, path_cf,
                                                   kCFURLPOSIXPathStyle, false);
  if (url_ref) {
    LSOpenCFURLRef(url_ref, nullptr);
    CFRelease(url_ref);
  }

  CFRelease(path_cf);
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  CFStringRef keys[] = {kCFUserNotificationAlertHeaderKey,
                        kCFUserNotificationDefaultButtonTitleKey};

  CFStringRef message_cf = CFStringCreateWithCString(
      kCFAllocatorDefault, message.data(), kCFStringEncodingUTF8);
  CFStringRef button_cf = CFSTR("OK");

  CFTypeRef values[] = {message_cf, button_cf};

  CFDictionaryRef dict = CFDictionaryCreate(
      kCFAllocatorDefault, reinterpret_cast<const void**>(keys),
      reinterpret_cast<const void**>(values), 2, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);

  if (dict) {
    CFOptionFlags responseFlags;
    CFUserNotificationDisplayAlert(0,           // timeout
                                   0,           // flags
                                   nullptr,     // icon URL
                                   nullptr,     // sound URL
                                   nullptr,     // localization URL
                                   CFSTR(""),   // default title (empty)
                                   message_cf,  // message
                                   button_cf,   // default button
                                   nullptr,     // alternate button
                                   nullptr,     // other button
                                   &responseFlags);

    CFRelease(dict);
  }

  if (message_cf) CFRelease(message_cf);
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
