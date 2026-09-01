/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/system.h"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  auto cmd = std::string("open \"") + std::string(url) + "\"";
  std::system(cmd.c_str());
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  auto cmd = std::string("open \"") + path.string() + "\"";
  std::system(cmd.c_str());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  std::fprintf(stderr, "[MESSAGE BOX] %.*s\n", static_cast<int>(message.size()), message.data());
}

bool SetProcessPriorityClass(const uint32_t priority_class) {
  return true;
}

bool IsUseNexusForGameBarEnabled() {
  return false;
}

}  // namespace xe
