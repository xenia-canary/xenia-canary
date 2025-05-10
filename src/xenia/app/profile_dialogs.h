/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_PROFILE_DIALOGS_H_
#define XENIA_APP_PROFILE_DIALOGS_H_

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class EmulatorWindow;

class NoProfileDialog final : public ui::ImGuiDialog {
 public:
  NoProfileDialog(ui::ImGuiDrawer* imgui_drawer,
                  EmulatorWindow* emulator_window)
      : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

  EmulatorWindow* emulator_window_;
};

class ProfileConfigDialog final : public ui::ImGuiDialog {
 public:
  ProfileConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                      EmulatorWindow* emulator_window)
      : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
    LoadProfileIcon();
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void LoadProfileIcon();
  void LoadProfileIcon(const uint64_t xuid);

  std::map<uint64_t, std::unique_ptr<ui::ImmediateTexture>> profile_icon_;

  uint64_t selected_xuid_ = 0;
  EmulatorWindow* emulator_window_;
};

class GamertagModifyDialog final : public ui::ImGuiDialog {
 public:
  GamertagModifyDialog(EmulatorWindow* emulator_window,
                       ui::ImGuiDrawer* imgui_drawer, uint64_t xuid)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        xuid_(xuid) {
    memset(gamertag_, 0, sizeof(gamertag_));
  }

 private:
  void OnDraw(ImGuiIO& io) override;

  bool has_opened_ = false;
  char gamertag_[16] = "";
  const uint64_t xuid_;
  EmulatorWindow* emulator_window_;
};

class ProfileIconModifyDialog final : public ui::ImGuiDialog {
 public:
  ProfileIconModifyDialog(EmulatorWindow* emulator_window,
                          ui::ImGuiDrawer* imgui_drawer, uint64_t xuid)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        xuid_(xuid) {
    Initialize();
  }

 private:
  void Initialize();
  void OnDraw(ImGuiIO& io) override;

  bool has_opened_ = false;
  const uint64_t xuid_;

  std::vector<uint8_t> loaded_icon_data_;
  std::string loading_status_ = "";
  std::unique_ptr<ui::ImmediateTexture> loaded_icon_;
  std::unique_ptr<ui::ImmediateTexture> current_icon_;
  EmulatorWindow* emulator_window_;
};

}  // namespace app
}  // namespace xe

#endif
