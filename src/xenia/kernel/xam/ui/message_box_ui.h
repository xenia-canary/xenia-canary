/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_MESSAGE_BOX_UI_H_
#define XENIA_KERNEL_XAM_UI_MESSAGE_BOX_UI_H_

#include "xenia/kernel/xam/ui/modal_dialog_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

class MessageBoxDialog : public XamModalDialog {
 public:
  MessageBoxDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                   std::string& description, std::vector<std::string> buttons,
                   uint32_t default_button)
      : XamModalDialog(imgui_drawer),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    set_title(title.empty() ? "Message Box" : title, "XamMessageBox");
  }

  uint32_t chosen_button() const { return chosen_button_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;
  virtual ~MessageBoxDialog() {}

 private:
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
  bool cancelled_ = false;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
