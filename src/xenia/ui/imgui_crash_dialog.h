/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_CRASH_DIALOG_H_
#define XENIA_UI_IMGUI_CRASH_DIALOG_H_

#include <string>
#include <utility>
#include <vector>

#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace ui {

struct CrashReport {
  std::string heading;
  std::string description;
  std::vector<std::pair<std::string, std::string>> details;
  std::string dump;
};

ImGuiDialog* ShowCrashDialog(ImGuiDrawer* imgui_drawer, CrashReport report);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_CRASH_DIALOG_H_
