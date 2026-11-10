/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_MODAL_DIALOG_UI_H_
#define XENIA_KERNEL_XAM_UI_MODAL_DIALOG_UI_H_

#include <string>
#include <string_view>
#include <vector>

#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

class XamModalDialog : public XamDialog {
 protected:
  XamModalDialog(xe::ui::ImGuiDrawer* imgui_drawer) : XamDialog(imgui_drawer) {}

  void set_title(std::string_view title, std::string_view id) {
    popup_id_ = std::string(title) + "###" + std::string(id);
  }

  // Must be followed by ImGui::EndPopup() when it returns true.
  bool BeginModal();
  bool first_draw() const { return first_draw_; }
  void Dismiss();

  static float ContentWidth();
  static bool IsCancelPressed();
  static void DrawDescription(const std::string& description);
  static int DrawButtons(const std::vector<std::string>& labels,
                         int focus_button = -1);

 private:
  std::string popup_id_;
  bool opened_ = false;
  bool first_draw_ = false;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
