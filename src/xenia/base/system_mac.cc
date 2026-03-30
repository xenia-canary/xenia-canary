/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>

#include "xenia/base/system.h"

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  auto cmd = std::string("open ");
  cmd.append(url);
  system(cmd.c_str());
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  auto cmd = std::string("open ");
  cmd.append(path.string());
  system(cmd.c_str());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* icon;
  switch (type) {
    case SimpleMessageBoxType::Help:
      icon = "note";
      break;
    case SimpleMessageBoxType::Warning:
      icon = "caution";
      break;
    default:
    case SimpleMessageBoxType::Error:
      icon = "stop";
      break;
  }
  // Use osascript to display a native macOS dialog.
  auto cmd = std::string("osascript -e 'display dialog \"");
  // Escape double quotes in the message.
  for (char c : message) {
    if (c == '"') {
      cmd += "\\\"";
    } else if (c == '\\') {
      cmd += "\\\\";
    } else {
      cmd += c;
    }
  }
  cmd += "\" with icon ";
  cmd += icon;
  cmd += " buttons {\"OK\"} default button \"OK\" with title \"Xenia\"'";
  system(cmd.c_str());
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
