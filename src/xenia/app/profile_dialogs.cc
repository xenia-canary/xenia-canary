/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>

#include "xenia/app/emulator_window.h"
#include "xenia/app/profile_dialogs.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/system.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/ui/file_picker.h"

namespace xe {
namespace app {

void NoProfileDialog::OnDraw(ImGuiIO& io) {
  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();

  if (profile_manager->GetAccountCount()) {
    delete this;
    return;
  }

  const auto window_position =
      ImVec2(GetIO().DisplaySize.x * 0.35f, GetIO().DisplaySize.y * 0.4f);

  ImGui::SetNextWindowPos(window_position, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(1.0f);

  bool dialog_open = true;
  if (!ImGui::Begin("No Profiles Found", &dialog_open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    delete this;
    return;
  }

  const std::string message =
      "There is no profile available! You will not be able to save without "
      "one.\n\nWould you like to create one?";

  ImGui::TextUnformatted(message.c_str());

  ImGui::Separator();
  ImGui::NewLine();

  const auto content_files = xe::filesystem::ListDirectories(
      emulator_window_->emulator()->content_root());

  if (content_files.empty()) {
    if (ImGui::Button("Create Profile")) {
      new kernel::xam::CreateProfileDialog(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator());
    }
  } else {
    if (ImGui::Button("Create profile & migrate data")) {
      new kernel::xam::CreateProfileDialog(emulator_window_->imgui_drawer(),
                                           emulator_window_->emulator(), true);
    }
  }

  ImGui::SameLine();
  if (ImGui::Button("Open profile menu")) {
    emulator_window_->ToggleProfilesConfigDialog();
  }

  ImGui::SameLine();
  if (ImGui::Button("Close") || !dialog_open) {
    emulator_window_->SetHotkeysState(true);
    ImGui::End();
    delete this;
    return;
  }
  ImGui::End();
}

void ProfileConfigDialog::LoadProfileIcon() {
  if (!emulator_window_) {
    return;
  }

  for (uint8_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager()
                             ->GetProfile(user_index);

    if (!profile) {
      continue;
    }
    LoadProfileIcon(profile->xuid());
  }
}

void ProfileConfigDialog::LoadProfileIcon(const uint64_t xuid) {
  if (!emulator_window_) {
    return;
  }

  const auto profile_manager = emulator_window_->emulator()
                                   ->kernel_state()
                                   ->xam_state()
                                   ->profile_manager();
  if (!profile_manager) {
    return;
  }

  const auto profile = profile_manager->GetProfile(xuid);

  if (!profile) {
    if (profile_icon_.contains(xuid)) {
      profile_icon_[xuid].release();
    }
    return;
  }

  const auto profile_icon =
      profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
  if (profile_icon.empty()) {
    return;
  }

  profile_icon_[xuid].release();
  profile_icon_[xuid] = imgui_drawer()->LoadImGuiIcon(profile_icon);
}

void ProfileConfigDialog::OnDraw(ImGuiIO& io) {
  if (!emulator_window_->emulator() ||
      !emulator_window_->emulator()->kernel_state() ||
      !emulator_window_->emulator()->kernel_state()->xam_state()) {
    return;
  }

  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();
  if (!profile_manager) {
    return;
  }

  auto profiles = profile_manager->GetAccounts();

  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.8f);

  bool dialog_open = true;
  if (!ImGui::Begin("Profiles Menu", &dialog_open,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    return;
  }

  if (profiles->empty()) {
    ImGui::TextUnformatted("No profiles found!");
    ImGui::Spacing();
    ImGui::Separator();
  }

  const ImVec2 next_window_position =
      ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x + 20.f,
             ImGui::GetWindowPos().y);

  for (auto& [xuid, account] : *profiles) {
    ImGui::PushID(static_cast<int>(xuid));

    const uint8_t user_index =
        profile_manager->GetUserIndexAssignedToProfile(xuid);

    const auto profile_icon = profile_icon_.find(xuid) != profile_icon_.cend()
                                  ? profile_icon_[xuid].get()
                                  : nullptr;

    auto context_menu_fun = [=, this]() -> bool {
      if (ImGui::BeginPopupContextItem("Profile Menu")) {
        //*selected_xuid = xuid;
        if (user_index == XUserIndexAny) {
          if (ImGui::MenuItem("Login")) {
            profile_manager->Login(xuid);
            if (!profile_manager->GetProfile(xuid)
                     ->GetProfileIcon(kernel::xam::XTileType::kGamerTile)
                     .empty()) {
              LoadProfileIcon(xuid);
            }
          }
          if (ImGui::BeginMenu("Login to slot:")) {
            for (uint8_t i = 1; i <= XUserMaxUserCount; i++) {
              if (ImGui::MenuItem(fmt::format("slot {}", i).c_str())) {
                profile_manager->Login(xuid, i - 1);
              }
            }
            ImGui::EndMenu();
          }
        } else {
          if (ImGui::MenuItem("Logout")) {
            profile_manager->Logout(user_index);
            LoadProfileIcon(xuid);
          }
        }

        const bool is_signedin = profile_manager->GetProfile(xuid) != nullptr;
        ImGui::BeginDisabled(emulator_window_->emulator()->is_title_open());
        if (ImGui::BeginMenu("Modify")) {
          if (ImGui::MenuItem("Gamertag")) {
            new GamertagModifyDialog(emulator_window_,
                                     emulator_window_->imgui_drawer(), xuid);
          }

          ImGui::BeginDisabled(!is_signedin);
          if (ImGui::MenuItem("Profile Icon")) {
            new ProfileIconModifyDialog(emulator_window_,
                                        emulator_window_->imgui_drawer(), xuid);
          }
          ImGui::EndDisabled();
          ImGui::EndMenu();
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!is_signedin);
        if (ImGui::MenuItem("Show Played Titles")) {
          new kernel::xam::GamesInfoDialog(
              emulator_window_->imgui_drawer(), next_window_position,
              profile_manager->GetProfile(user_index));
        }
        ImGui::EndDisabled();

        if (ImGui::MenuItem("Show Content Directory")) {
          const auto path = profile_manager->GetProfileContentPath(
              xuid, emulator_window_->emulator()->kernel_state()->title_id());

          if (!std::filesystem::exists(path)) {
            std::filesystem::create_directories(path);
          }

          std::thread path_open(LaunchFileExplorer, path);
          path_open.detach();
        }

        if (!emulator_window_->emulator()->is_title_open()) {
          ImGui::Separator();
          if (ImGui::BeginMenu("Delete Profile")) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                fmt::format(
                    "You're about to delete profile: {} (XUID: {:016X}). "
                    "This will remove all data assigned to this profile "
                    "including savefiles. Are you sure?",
                    account.GetGamertagString(), xuid)
                    .c_str());
            ImGui::EndTooltip();

            if (ImGui::MenuItem("Yes, delete it!")) {
              profile_manager->DeleteProfile(xuid);
              ImGui::EndMenu();
              ImGui::EndPopup();
              return false;
            }

            ImGui::EndMenu();
          }
        }
        ImGui::EndPopup();
      }
      return true;
    };

    if (!kernel::xam::xeDrawProfileContent(
            imgui_drawer(), xuid, user_index, &account, profile_icon,
            context_menu_fun, [=, this]() { LoadProfileIcon(xuid); },
            &selected_xuid_)) {
      ImGui::PopID();
      ImGui::End();
      return;
    }

    ImGui::PopID();
    ImGui::Separator();
  }

  ImGui::Spacing();

  if (ImGui::Button("Create Profile")) {
    new kernel::xam::CreateProfileDialog(emulator_window_->imgui_drawer(),
                                         emulator_window_->emulator());
  }

  ImGui::End();

  if (!dialog_open) {
    emulator_window_->ToggleProfilesConfigDialog();
    return;
  }
}

void GamertagModifyDialog::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup("Modify Gamertag");
    has_opened_ = true;
  }

  bool dialog_open = true;
  if (!ImGui::BeginPopupModal("Modify Gamertag", &dialog_open,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    return;
  }

  auto profile_manager = emulator_window_->emulator()
                             ->kernel_state()
                             ->xam_state()
                             ->profile_manager();

  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) {
    ImGui::SetKeyboardFocusHere(0);
  }

  ImGui::TextUnformatted("New gamertag:");
  ImGui::InputText("##Gamertag", gamertag_, sizeof(gamertag_));

  const std::string gamertag_string = std::string(gamertag_);
  bool valid = profile_manager->IsGamertagValid(gamertag_string);

  ImGui::BeginDisabled(!valid);
  if (ImGui::Button("Update")) {
    profile_manager->ModifyGamertag(xuid_, gamertag_string);
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
    dialog_open = false;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  if (ImGui::Button("Cancel")) {
    std::fill(std::begin(gamertag_), std::end(gamertag_), '\0');
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

void ProfileIconModifyDialog::Initialize() {
  const auto icon = emulator_window_->emulator()
                        ->kernel_state()
                        ->xam_state()
                        ->profile_manager()
                        ->GetProfile(xuid_)
                        ->GetProfileIcon(kernel::xam::XTileType::kGamerTile);

  if (!icon.empty()) {
    current_icon_ = imgui_drawer()->LoadImGuiIcon(icon);
  }
}

void ProfileIconModifyDialog::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup("Modify Profile Icon");
    has_opened_ = true;
  }

  bool dialog_open = true;
  if (!ImGui::BeginPopupModal("Modify Profile Icon", &dialog_open,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_HorizontalScrollbar)) {
    Close();
    return;
  }

  auto user_tracker =
      emulator_window_->emulator()->kernel_state()->xam_state()->user_tracker();

  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0)) {
    ImGui::SetKeyboardFocusHere(0);
  }

  ImGui::Text("Provide a PNG image with a resolution of 64x64.");
  ImGui::Text("Icon will refresh after relog.");
  ImGui::Dummy(ImVec2(10.f, 10.f));

  if (ImGui::BeginTable("icons_table", 2)) {
    ImGui::TableNextColumn();
    ImGui::Text("Current Icon:");
    if (current_icon_) {
      ImGui::Image(reinterpret_cast<ImTextureID>(current_icon_.get()),
                   ui::default_image_icon_size);
    } else {
      ImGui::Dummy(ui::default_image_icon_size);
    }

    ImGui::TableNextColumn();
    ImGui::Text("New Icon:");

    if (loaded_icon_) {
      ImGui::Image(reinterpret_cast<ImTextureID>(loaded_icon_.get()),
                   ui::default_image_icon_size);
    } else {
      ImGui::Dummy(ui::default_image_icon_size);
    }

    ImGui::EndTable();
  }

  if (!loading_status_.empty()) {
    ImGui::Text("Status: %s", loading_status_.c_str());
  }

  if (ImGui::Button("Load Icon")) {
    std::filesystem::path path;

    auto file_picker = xe::ui::FilePicker::Create();
    file_picker->set_mode(ui::FilePicker::Mode::kOpen);
    file_picker->set_type(ui::FilePicker::Type::kFile);
    file_picker->set_multi_selection(false);
    file_picker->set_title("Select PNG Image");
    file_picker->set_extensions({{"PNG Image", "*.png"}});

    if (file_picker->Show(emulator_window_->window())) {
      auto selected_files = file_picker->selected_files();
      if (!selected_files.empty()) {
        path = selected_files[0];
      }

      loading_status_.clear();
      loaded_icon_data_.clear();
      loaded_icon_.reset();

      if (IsFilePngImage(path)) {
        const auto res = GetImageResolution(path);

        if (res == kernel::xam::kProfileIconSizeSmall ||
            res == kernel::xam::kProfileIconSize) {
          loaded_icon_data_ = ReadPngFromFile(path);
          loaded_icon_ = imgui_drawer()->LoadImGuiIcon(loaded_icon_data_);
        } else {
          loading_status_ =
              fmt::format("Invalid file resolution. Provided size: {}x{}",
                          res.first, res.second);
        }
      } else {
        loading_status_ = "Invalid file loaded.";
      }
    }
  }

  ImGui::SameLine();
  ImGui::BeginDisabled(loaded_icon_data_.empty());
  if (ImGui::Button("Update Profile Icon")) {
    user_tracker->UpdateUserIcon(xuid_, loaded_icon_data_);
    dialog_open = false;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();

  if (ImGui::Button("Cancel")) {
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