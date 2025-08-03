/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_RECENTLY_PLAYED_CONTENT_DIALOG_H_
#define XENIA_APP_RECENTLY_PLAYED_CONTENT_DIALOG_H_

#include <memory>
#include <string>

#include "xenia/app/main_menu_content.h"

namespace xe {
namespace app {

class EmulatorWindow;

class RecentlyPlayedContentDialog : public MainMenuContent {
 public:
  RecentlyPlayedContentDialog(ui::ImGuiDrawer* imgui_drawer,
                              EmulatorWindow* emulator_window);

  ~RecentlyPlayedContentDialog();

  void Draw() override;

 protected:
 private:
  void DrawTitleInfo(const RecentTitleEntry& entry);

  EmulatorWindow* emulator_window_;
  uint8_t selected_recently_title_ = 0;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_RECENTLY_PLAYED_CONTENT_DIALOG_H_
