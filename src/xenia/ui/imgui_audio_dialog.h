/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_AUDIO_DIALOG_H_
#define XENIA_UI_IMGUI_AUDIO_DIALOG_H_

#include <functional>

#include "xenia/ui/imgui_gamepad_dialog.h"

namespace xe {
namespace ui {

// In-game master volume slider. Adjusts the live "volume" cvar; changes apply
// immediately and are not persisted.
class ImGuiAudioDialog : public ImGuiGamepadDialog {
 public:
  ImGuiAudioDialog(ImGuiDrawer* drawer, hid::InputSystem* input_system);

  void CloseDialog() { Close(); }

  void SetOnCloseCallback(std::function<void()> callback) {
    on_close_callback_ = std::move(callback);
  }

  // Invoked whenever the volume changes.
  void SetOnChangeCallback(std::function<void()> callback) {
    on_change_callback_ = std::move(callback);
  }

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  std::function<void()> on_close_callback_;
  std::function<void()> on_change_callback_;
  int volume_percent_ = 100;  // 0-100 for slider display
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_AUDIO_DIALOG_H_
