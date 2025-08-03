/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_MAIN_MENU_WINDOW_H_
#define XENIA_APP_MAIN_MENU_WINDOW_H_

#include <memory>
#include <string>

#include "xenia/app/emulator_window.h"
#include "xenia/app/main_menu_content.h"
#include "xenia/app/profile_dialogs.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/menu_item.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

enum class MainMenuUiContent : uint8_t { recent, profiles, settings, about };

class MainMenuWindow final : public ui::ImGuiDialog {
 public:
  MainMenuWindow(ui::ImGuiDrawer* imgui_drawer,
                 EmulatorWindow* emulator_window);
  ~MainMenuWindow();

 protected:
  void OnDraw(ImGuiIO& io) override;

  void DrawSidebarMenu(const float width);
  void DrawContentView(const float width);

  void DrawCurrentlySignedProfile();

  // void DrawRecentMenu();
  void DrawProfilesMenu();
  void DrawSettings();
  void DrawAbout();

  void ToggleInvisibility() { is_visible_ ^= true; }

 private:
  void DrawXeniaInfo();
  void OnGuideButtonPress(uint8_t controller_index);

  EmulatorWindow* emulator_window_;

  MainMenuUiContent current_content_ = MainMenuUiContent::recent;
  uint8_t selected_recently_title_ = 0;
  uint8_t controller_index_ = 0;

  std::unique_ptr<MainMenuContent> currently_selected_submenu_ = nullptr;

  bool has_opened_ = false;
  bool was_cursor_set_ = false;
  bool is_visible_ = true;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_MAIN_MENU_WINDOW_H_
