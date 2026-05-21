/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_audio_dialog.h"

#include <cfloat>

#include "third_party/imgui/imgui.h"
#include "xenia/apu/apu_flags.h"

namespace xe {
namespace ui {

ImGuiAudioDialog::ImGuiAudioDialog(ImGuiDrawer* drawer,
                                   hid::InputSystem* input_system)
    : ImGuiGamepadDialog(drawer, input_system) {
  volume_percent_ = cvars::volume > 100 ? 100 : static_cast<int>(cvars::volume);
}

void ImGuiAudioDialog::OnClose() {
  if (on_close_callback_) {
    on_close_callback_();
  }
}

void ImGuiAudioDialog::OnDraw(ImGuiIO& io) {
  // Style - white background, black text, Xbox green accents
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, xbox_green);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

  // Center on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  // Ensure window has reasonable minimum width
  float title_width = ImGui::CalcTextSize("Audio").x;
  float min_width = (title_width + ImGui::GetStyle().WindowPadding.x * 2 +
                     ImGui::GetFrameHeight()) *
                    3.0f;
  ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0),
                                      ImVec2(FLT_MAX, FLT_MAX));

  bool is_open = true;
  if (ImGui::Begin("Audio", &is_open,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoScrollbar)) {
    // Handle keyboard escape or gamepad B/Back
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      Close();
    }

    bool changed = false;

    // Master volume.
    ImGui::Text("Volume");
    ImGui::SetNextItemWidth(-1);  // Use full available width
    if (ImGui::SliderInt("##volume", &volume_percent_, 0, 100, "%d%%",
                         ImGuiSliderFlags_None)) {
      cvars::volume = volume_percent_;
      changed = true;
    }
    // Reflect external changes when the user isn't dragging the slider.
    if (!ImGui::IsItemActive()) {
      volume_percent_ =
          cvars::volume > 100 ? 100 : static_cast<int>(cvars::volume);
    }

    if (changed && on_change_callback_) {
      on_change_callback_();
    }

    ImGui::End();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(12);

  if (!is_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xe
