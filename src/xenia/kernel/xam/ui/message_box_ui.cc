/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/message_box_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

void MessageBoxDialog::OnDraw(ImGuiIO& io) {
  if (!BeginModal()) {
    return;
  }

  if (!first_draw() && IsCancelPressed()) {
    cancelled_ = true;
    Dismiss();
    ImGui::EndPopup();
    return;
  }

  DrawDescription(description_);

  int focus_button = -1;
  if (first_draw()) {
    focus_button = default_button_ < buttons_.size()
                       ? static_cast<int>(default_button_)
                       : 0;
  }

  const int pressed = DrawButtons(buttons_, focus_button);
  if (pressed >= 0) {
    chosen_button_ = static_cast<uint32_t>(pressed);
    Dismiss();
  }

  ImGui::EndPopup();
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
