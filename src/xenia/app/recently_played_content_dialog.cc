/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <ctime>

#include "third_party/fmt/include/fmt/std.h"
#include "xenia/app/emulator_window.h"

#include "xenia/app/recently_played_content_dialog.h"

namespace xe {
namespace app {

RecentlyPlayedContentDialog::RecentlyPlayedContentDialog(
    ui::ImGuiDrawer* imgui_drawer, EmulatorWindow* emulator_window)
    : MainMenuContent(imgui_drawer, emulator_window),
      emulator_window_(emulator_window) {}

RecentlyPlayedContentDialog::~RecentlyPlayedContentDialog() {}

void RecentlyPlayedContentDialog::DrawTitleInfo(const RecentTitleEntry& entry) {
  if (ImGui::BeginTable("TitleInfoTable", 1)) {
    // Row 0 - Title Name
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    const auto title_name =
        entry.title_name.empty() ? "[Unknown Title]" : entry.title_name;

    ImGui::PushFont(emulator_window_->imgui_drawer()->GetTitleFont());
    ImGui::Text(fmt::format("{}", title_name).c_str());
    ImGui::PopFont();

    // Row 1 - Path to file
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    ImGui::Text(fmt::format("{}", entry.path_to_file.parent_path()).c_str());

    // Row 2 - Time
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    // Prepare time
    std::tm tm =
        *std::localtime(static_cast<const time_t*>(&entry.last_run_time));
    ImGui::Text(fmt::format("{:%Y-%m-%d %H:%M:%S}", tm).c_str());
  }

  ImGui::EndTable();
}

void RecentlyPlayedContentDialog::Draw() {
  ImGui::Dummy(ImVec2(50.f, 50.f));

  ImGui::PushFont(emulator_window_->imgui_drawer()->GetTitleFont());
  ImGui::Text("Recently played titles");
  ImGui::PopFont();
  ImGui::Separator();

  const auto recent_titles = emulator_window_->GetRecentlyPlayedTitles();
  const auto default_row_height = 75.f;

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 4));

  if (ImGui::BeginTable("RecentTable", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("Image",
                            ImGuiTableColumnFlags_::ImGuiTableColumnFlags_None,
                            default_row_height);
    ImGui::TableSetupColumn(
        "Title Details",
        ImGuiTableColumnFlags_::ImGuiTableColumnFlags_WidthStretch, 325.f);

    if (!was_cursor_set_) {
      ImGui::SetKeyboardFocusHere();
      ImGui::SetItemDefaultFocus();
      was_cursor_set_ = true;
    }

    for (const auto& entry : *recent_titles) {
      ImGui::TableNextRow(ImGuiTableRowFlags_None, default_row_height);
      ImGui::TableSetColumnIndex(0);

      if (ImGui::Selectable(
              fmt::format("###{}_Selectable", entry.title_name).c_str(),
              selected_recently_title_, ImGuiSelectableFlags_SpanAllColumns,
              ImVec2(0, default_row_height))) {
        emulator_window_->RunTitle(entry.path_to_file);
      }

      ImGui::SameLine();
      ImGui::Image(
          reinterpret_cast<ImTextureID>(
              emulator_window_->imgui_drawer()->GetNotificationIcon(0)),
          ui::default_image_icon_size);
      ImGui::SameLine();
      ImGui::Dummy(ImVec2(250.f, 75.f));
      ImGui::TableNextColumn();
      DrawTitleInfo(entry);
    }

    ImGui::EndTable();
  }

  ImGui::PopStyleVar();
}

}  // namespace app
}  // namespace xe
