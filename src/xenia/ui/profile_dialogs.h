/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_PROFILE_DIALOGS_H_
#define XENIA_UI_PROFILE_DIALOGS_H_

#include <functional>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_gamepad_dialog.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class EmulatorWindow;

class ProfileConfigDialog final : public ui::ImGuiGamepadDialog {
 public:
  ProfileConfigDialog(ui::ImGuiDrawer* imgui_drawer,
                      EmulatorWindow* emulator_window,
                      hid::InputSystem* input_system)
      : ui::ImGuiGamepadDialog(imgui_drawer, input_system),
        emulator_window_(emulator_window) {
    LoadProfileIcon();
  }

  void CloseDialog() { Close(); }

  void SetOnCloseCallback(std::function<void()> callback) {
    on_close_callback_ = std::move(callback);
  }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override {
    if (on_close_callback_) {
      on_close_callback_();
    }
  }

 private:
  void LoadProfileIcon();
  void LoadProfileIcon(const uint64_t xuid);

  std::map<uint64_t, std::unique_ptr<ui::ImmediateTexture>> profile_icon_;

  uint64_t selected_xuid_ = 0;
  bool context_menu_open_ = false;
  EmulatorWindow* emulator_window_;
  std::function<void()> on_close_callback_;
};

}  // namespace app
}  // namespace xe

#endif
