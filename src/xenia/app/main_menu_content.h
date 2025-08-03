/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_MAIN_MENU_CONTENT_H_
#define XENIA_APP_MAIN_MENU_CONTENT_H_

#include "xenia/ui/imgui_drawer.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class EmulatorWindow;

class MainMenuContent {
 public:
  virtual ~MainMenuContent() = 0;

  virtual void Draw() = 0;

 protected:
  MainMenuContent(ui::ImGuiDrawer* imgui_drawer,
                  EmulatorWindow* emulator_window);

  void ToggleInvisibility() { is_visible_ ^= true; }

 protected:
  bool was_cursor_set_ = false;

 private:
  ui::ImGuiDrawer* imgui_drawer_;
  EmulatorWindow* emulator_window_;
  uint8_t controller_index_ = 0;
  bool has_opened_ = false;
  bool is_visible_ = true;

  std::vector<std::unique_ptr<ui::ImmediateTexture>> profile_icons_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_MAIN_MENU_CONTENT_H_
