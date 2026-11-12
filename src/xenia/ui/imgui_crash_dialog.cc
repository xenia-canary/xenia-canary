/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_crash_dialog.h"

#include <algorithm>
#include <cfloat>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace ui {

namespace {

constexpr ImVec4 kHeadingColor = ImVec4(0.95f, 0.36f, 0.33f, 1.0f);
constexpr double kStatusDuration = 3.0;

class CrashDialog final : public ImGuiDialog {
 public:
  CrashDialog(ImGuiDrawer* imgui_drawer, CrashReport report)
      : ImGuiDialog(imgui_drawer), report_(std::move(report)) {
    popup_id_ = fmt::format("Crash###xe_crash_dialog_{}", GetWindowId());
    dump_buffer_ = report_.dump;
  }

  void OnDraw(ImGuiIO& io) override {
    const bool first_draw = !opened_;

    if (!opened_) {
      ImGui::OpenPopup(popup_id_.c_str());
      opened_ = true;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, viewport->WorkSize.y * 0.9f));

    if (!ImGui::BeginPopupModal(popup_id_.c_str(), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings)) {
      Close();
      return;
    }

    const float font_size = ImGui::GetFontSize();
    const float width =
        std::min(font_size * 36.0f, viewport->WorkSize.x * 0.85f);

    DrawHeader(width);
    DrawDetails(width);
    DrawDump(width, font_size);
    DrawFooter(first_draw);

    if (IsCancelPressed()) {
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImGui::EndPopup();
  }

 private:
  void DrawHeader(float width) {
    ImGui::PushFont(nullptr, imgui_drawer()->GetTitleFontSize());
    ImGui::PushStyleColor(ImGuiCol_Text, kHeadingColor);
    ImGui::TextUnformatted(report_.heading.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Spacing();

    // Fixes the dialog's width; everything below wraps or fills to it.
    ImGui::Dummy(ImVec2(width, 0.0f));

    if (!report_.description.empty()) {
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
      ImGui::TextUnformatted(report_.description.c_str());
      ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
  }

  void DrawDetails(float width) {
    if (report_.details.empty()) {
      return;
    }

    // Pad rows evenly so text doesn't sit against the row borders.
    ImGui::PushStyleVar(
        ImGuiStyleVar_CellPadding,
        ImVec2(ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize() * 0.3f));

    if (!ImGui::BeginTable("##details", 2,
                           ImGuiTableFlags_BordersInnerH |
                               ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_SizingFixedFit,
                           ImVec2(width, 0.0f))) {
      ImGui::PopStyleVar();
      return;
    }

    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

    const ImVec4 label_color = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];

    for (const auto& [label, value] : report_.details) {
      ImGui::TableNextRow();

      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, label_color);
      ImGui::TextUnformatted(label.c_str());
      ImGui::PopStyleColor();

      ImGui::TableNextColumn();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(value.c_str());
      ImGui::PopTextWrapPos();
    }

    ImGui::EndTable();
    ImGui::PopStyleVar();

    ImGui::Spacing();
  }

  void DrawDump(float width, float font_size) {
    if (report_.dump.empty()) {
      return;
    }

    if (!ImGui::CollapsingHeader("Crash dump")) {
      return;
    }

    ImGui::InputTextMultiline(
        "##dump", dump_buffer_.data(), dump_buffer_.size() + 1,
        ImVec2(width, font_size * 16.0f), ImGuiInputTextFlags_ReadOnly);
  }

  void DrawFooter(bool first_draw) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (status_until_ > ImGui::GetTime()) {
      // Drawn rather than laid out so the button row isn't moved by it.
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      const float offset_y =
          (DialogButtonSize().y - ImGui::GetTextLineHeight()) * 0.5f;

      ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, pos.y + offset_y),
                                          ImGui::GetColorU32(ImGuiCol_Text),
                                          status_text_);
    }

    RightAlignDialogButtons(2);

    if (DialogButton("Copy", false)) {
      ImGui::SetClipboardText(BuildReportText().c_str());
      ShowStatus("Copied to the clipboard.");
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Copy the summary and crash dump for a bug report.");
    }

    ImGui::SameLine();

    if (first_draw) {
      ImGui::SetKeyboardFocusHere();
    }

    if (DialogButton("Close", true)) {
      ImGui::CloseCurrentPopup();
      Close();
    }
  }

  void ShowStatus(const char* text) {
    status_text_ = text;
    status_until_ = ImGui::GetTime() + kStatusDuration;
  }

  std::string BuildReportText() const {
    std::string text = report_.heading;
    text.push_back('\n');

    for (const auto& [label, value] : report_.details) {
      text.append(fmt::format("{}: {}\n", label, value));
    }

    if (!report_.dump.empty()) {
      text.push_back('\n');
      text.append(report_.dump);
    }

    return text;
  }

  static bool IsCancelPressed() {
    return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
           !ImGui::IsAnyItemActive() &&
           (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false));
  }

  CrashReport report_;
  std::string popup_id_;

  // Read-only, but ImGui wants a mutable buffer.
  std::string dump_buffer_;

  bool opened_ = false;

  const char* status_text_ = "";
  double status_until_ = 0.0;
};

}  // namespace

ImGuiDialog* ShowCrashDialog(ImGuiDrawer* imgui_drawer, CrashReport report) {
  return new CrashDialog(imgui_drawer, std::move(report));
}

}  // namespace ui
}  // namespace xe
