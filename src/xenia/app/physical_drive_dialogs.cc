/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/physical_drive_dialogs.h"
#include "xenia/app/emulator_window.h"

#include <algorithm>
#include <mutex>

#include "xenia/base/system.h"

namespace xe {
namespace app {

void ShowSecuritySectorSanityWarningOnce() {
  static std::once_flag shown_once;
  std::call_once(shown_once, []() {
    xe::ShowSimpleMessageBox(
        xe::SimpleMessageBoxType::Warning,
        "Security sector sanity check did not match expected Xbox game-disc "
        "layout.\n\nUsing game backup disks is piracy unless you own an "
        "official copy of the game.\n\nPress OK to acknowledge and "
        "continue.");
  });
}

void PhysicalDriveSelectionDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
  bool dialog_open = true;
  if (!ImGui::Begin("Select Optical Drive", &dialog_open,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    Close();
    return;
  }
  if (!dialog_open) {
    ImGui::End();
    Close();
    return;
  }

  ImGui::TextUnformatted(
      "Multiple suitable drives with inserted media were "
      "found. Select one:");
  ImGui::Separator();

  for (int i = 0; i < static_cast<int>(drives_.size()); ++i) {
    const auto& drive = drives_[i];
    const std::string label =
        fmt::format("{} - {}", drive.path.string(), drive.identity_summary);
    if (ImGui::Selectable(label.c_str(), selected_index_ == i)) {
      selected_index_ = i;
    }
  }

  ImGui::Separator();
  const bool can_confirm = selected_index_ >= 0 &&
                           selected_index_ < static_cast<int>(drives_.size());
  ImGui::BeginDisabled(!can_confirm);
  if (ImGui::Button("Open")) {
    emulator_window_->RunTitle(drives_[selected_index_].path);
    Close();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    Close();
  }

  ImGui::End();
}

void PhysicalDriveDiscEjectedDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_FirstUseEver);
  bool dialog_open = true;
  if (!ImGui::Begin(
          "Disc Ejected", &dialog_open,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted(
      "The physical disc appears to have been removed from the drive "
      "this title was launched from. Emulation has been paused.");
  ImGui::TextUnformatted(
      fmt::format("Drive: {}", drive_path_.string()).c_str());
  if (retry_failed_) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       "Disc still not detected.");
  }

  ImGui::Separator();
  if (ImGui::Button("Retry")) {
    const auto probes = vfs::DiscOmnidriveDevice::ProbeAllCandidateDrives();
    const bool present =
        std::any_of(probes.begin(), probes.end(), [&](const auto& info) {
          return info.path == drive_path_ && info.omnidrive_confirmed &&
                 info.media_inserted;
        });
    if (present) {
      emulator_window_->emulator()->Resume();
      Close();
    } else {
      retry_failed_ = true;
    }
  }
  ImGui::SameLine();
  ImGui::End();
}

}  // namespace app
}  // namespace xe
