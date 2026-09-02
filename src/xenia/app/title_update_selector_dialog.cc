/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/title_update_selector_dialog.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/content_manager.h"

namespace xe {
namespace app {

TitleUpdateSelectorDialog::TitleUpdateSelectorDialog(
    ui::ImGuiDrawer* imgui_drawer, kernel::xam::ContentManager* content_manager,
    const std::vector<kernel::xam::XCONTENT_DATA_INTERNAL>& title_updates,
    uint8_t* selected_entry_index)
    : ui::ImGuiDialog(imgui_drawer),
      title_updates_(title_updates),
      selected_entry_index_(selected_entry_index) {
  title_updates_metadata_.reserve(title_updates_.size());

  for (const auto& entry : title_updates) {
    const auto package = content_manager->OpenPackage(0, entry);
    if (package) {
      title_updates_metadata_.push_back(*package->GetContainerHeader());
    }
  }
}

void TitleUpdateSelectorDialog::DrawEntry(ImGuiIO& io, uint8_t index) {
  const auto start_position = ImGui::GetCursorPos();
  // First Column
  ImGui::TableSetColumnIndex(0);

  if (ImGui::RadioButton(fmt::format("##index{}", index).c_str(),
                         index == *selected_entry_index_)) {
    *selected_entry_index_ = index;
  }

  // Second Column
  ImGui::TableNextColumn();

  ImGui::Text(
      "%s",
      xe::to_utf8(title_updates_metadata_[index].content_metadata.display_name(
                      xe::XLanguage::kEnglish))
          .c_str());
  ImGui::Text("Game base version: %s",
              format_version(
                  static_cast<xex2_version>(
                      title_updates_metadata_[index].extra_fields.base_version))
                  .c_str());
  ImGui::Text(
      "TU Version: %s",
      format_version(
          static_cast<xex2_version>(
              title_updates_metadata_[index].extra_fields.installer_version))
          .c_str());

  // Third Column
  ImGui::TableNextColumn();
  const ImVec2 end_draw_position =
      ImVec2(ImGui::GetCursorPos().x - start_position.x,
             ImGui::GetCursorPos().y - start_position.y);

  ImGui::SetCursorPos(start_position);
  if (ImGui::Selectable(fmt::format("##{}Selectable", index).c_str(),
                        *selected_entry_index_ == index,
                        ImGuiSelectableFlags_NoAutoClosePopups |
                            ImGuiSelectableFlags_SpanAllColumns,
                        end_draw_position)) {
    *selected_entry_index_ = index;
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
    ImGui::TableNextRow(0, ui::default_image_icon_size.y);
    for (uint8_t index = 0; index < title_updates_.size(); ++index) {
      DrawEntry(io, index);
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

}  // namespace app
}  // namespace xe
