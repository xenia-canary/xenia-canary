/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/modal_dialog_ui.h"

#include <algorithm>
#include <cfloat>

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

bool XamModalDialog::BeginModal() {
  first_draw_ = !opened_;

  if (!opened_) {
    ImGui::OpenPopup(popup_id_.c_str());
    opened_ = true;
  }

  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(ImGui::GetFontSize() * 18.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));

  if (!ImGui::BeginPopupModal(popup_id_.c_str(), nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings)) {
    Close();
    return false;
  }

  return true;
}

void XamModalDialog::Dismiss() {
  ImGui::CloseCurrentPopup();
  Close();
}

float XamModalDialog::ContentWidth() {
  return std::min(ImGui::GetFontSize() * 28.0f,
                  ImGui::GetMainViewport()->WorkSize.x * 0.8f);
}

bool XamModalDialog::IsCancelPressed() {
  return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         !ImGui::IsAnyItemActive() &&
         (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
}

void XamModalDialog::DrawDescription(const std::string& description) {
  ImGui::Spacing();

  if (description.empty()) {
    return;
  }

  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ContentWidth());
  ImGui::TextUnformatted(description.c_str());
  ImGui::PopTextWrapPos();
}

int XamModalDialog::DrawButtons(const std::vector<std::string>& labels,
                                int focus_button) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float min_width = ImGui::GetFontSize() * 5.0f;
  const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;

  std::vector<float> widths;
  float row_width = -spacing;
  for (const std::string& label : labels) {
    widths.push_back(std::max(
        min_width,
        ImGui::CalcTextSize(label.c_str(), nullptr, true).x + padding));
    row_width += widths.back() + spacing;
  }

  const float offset = ImGui::GetContentRegionAvail().x - row_width;
  if (offset > 0.0f) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
  }

  int pressed = -1;

  for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
    if (i) {
      ImGui::SameLine();
    }

    ImGui::PushID(i);

    if (i == focus_button) {
      ImGui::SetKeyboardFocusHere();
    }

    if (ImGui::Button(labels[i].c_str(), ImVec2(widths[i], 0.0f))) {
      pressed = i;
    }

    ImGui::PopID();
  }

  return pressed;
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
