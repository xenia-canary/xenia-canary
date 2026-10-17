/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/title_update_selector_dialog.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/content_manager.h"

DECLARE_bool(allow_incompatible_title_update);

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

TitleUpdateSelectorDialog::TitleUpdateSelectorDialog(
    xe::ui::ImGuiDrawer* imgui_drawer, ContentManager* content_manager,
    const xex2_opt_execution_info* exec_info,
    const std::vector<XCONTENT_DATA_INTERNAL>& title_updates,
    const XLanguage user_language, uint8_t* selected_entry_index)
    : xe::ui::ImGuiDialog(imgui_drawer),
      exec_info_(exec_info),
      user_language_(user_language),
      selected_entry_index_(selected_entry_index) {
  title_update_data_.reserve(title_updates.size());

  for (size_t i = 0; i < title_updates.size(); ++i) {
    const auto package = content_manager->OpenPackage(0, title_updates[i]);
    if (package) {
      title_update_data_.emplace_back(i, title_updates[i],
                                      *package->GetContainerHeader());
    }
  }
}

void TitleUpdateSelectorDialog::DrawEntry(ImGuiIO& io,
                                          const TitleUpdateData& entry) {
  const bool is_partially_incompatible =
      exec_info_->media_id !=
      entry.container_header_.content_metadata.execution_info.media_id;
  if (is_partially_incompatible) {
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                           IM_COL32(120, 90, 0, 120));
  }

  const auto start_position = ImGui::GetCursorPos();
  // First Column
  ImGui::TableSetColumnIndex(0);

  ImGui::NewLine();

  ImGui::BeginDisabled(!cvars::allow_incompatible_title_update &&
                       is_partially_incompatible);

  if (ImGui::RadioButton(fmt::format("##index{}", entry.index_).c_str(),
                         entry.index_ == *selected_entry_index_)) {
    *selected_entry_index_ = entry.index_;
  }
  ImGui::EndDisabled();

  // Second Column
  ImGui::TableNextColumn();

  ImGui::Text("%s",
              to_utf8(entry.container_header_.content_metadata.display_name(
                          user_language_))
                  .c_str());
  ImGui::Text("Game base version: %s",
              format_version(static_cast<xex2_version>(
                                 entry.container_header_.content_metadata
                                     .execution_info.base_version()))
                  .c_str());
  ImGui::Text("TU Version: %s",
              format_version(
                  static_cast<xex2_version>(
                      entry.container_header_.extra_fields.installer_version))
                  .c_str());

  // Third Column
  ImGui::TableNextColumn();
  const ImVec2 end_draw_position =
      ImVec2(ImGui::GetCursorPos().x - start_position.x,
             ImGui::GetCursorPos().y - start_position.y);

  ImGui::BeginDisabled(!cvars::allow_incompatible_title_update &&
                       is_partially_incompatible);
  ImGui::SetCursorPos(start_position);
  if (ImGui::Selectable(fmt::format("##{}Selectable", entry.index_).c_str(),
                        *selected_entry_index_ == entry.index_,
                        ImGuiSelectableFlags_NoAutoClosePopups |
                            ImGuiSelectableFlags_SpanAllColumns,
                        end_draw_position)) {
    *selected_entry_index_ = entry.index_;
  }

  ImGui::EndDisabled();

  if (is_partially_incompatible) {
    ImGui::SetItemTooltip(
        "This title update is for a different game version and may not work "
        "correctly. To use it, enable 'allow_incompatible_title_update' in "
        "the config.");
  }
}

void TitleUpdateSelectorDialog::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup("Select Title Update");
    has_opened_ = true;
  }

  bool dialog_open = true;
  if (!ImGui::BeginPopupModal("Select Title Update", &dialog_open,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    return;
  }

  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }

  if (ImGui::BeginTable("", 2, ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableNextRow(0, xe::ui::default_image_icon_size.y);
    for (const auto& entry : title_update_data_) {
      DrawEntry(io, entry);
    }

    ImGui::EndTable();
  }

  if (ImGui::Button("Cancel")) {
    *selected_entry_index_ = 0xFF;
    dialog_open = false;
  }

  ImGui::SameLine();

  if (ImGui::Button("Confirm")) {
    dialog_open = false;
  }

  if (!dialog_open) {
    ImGui::CloseCurrentPopup();
    Close();
    ImGui::EndPopup();
    return;
  }
  ImGui::EndPopup();
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
