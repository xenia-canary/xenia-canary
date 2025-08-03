/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/main_menu_content.h"

#include "xenia/app/emulator_window.h"

namespace xe {
namespace app {

MainMenuContent::MainMenuContent(ui::ImGuiDrawer* imgui_drawer,
                                 EmulatorWindow* emulator_window)
    : imgui_drawer_(imgui_drawer), emulator_window_(emulator_window) {};

MainMenuContent::~MainMenuContent() {}

void MainMenuContent::Draw() {}

}  // namespace app
}  // namespace xe
