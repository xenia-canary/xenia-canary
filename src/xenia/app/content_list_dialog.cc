/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <filesystem>

#include "xenia/app/emulator_window.h"
#include "xenia/base/png_utils.h"
#include "xenia/base/system.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/kernel/xam/xcontent/xcontent_package_directory.h"

#include "xenia/app/content_list_dialog.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace app {

bool isBootableType(XContentType type) {
  return type == XContentType::kArcadeTitle ||
         type == XContentType::kGameDemo ||
         type == XContentType::kInstalledGame ||
         type == XContentType::kXbox360Title;
}

void ContentListDialog::Initialize() {
  size_t current_index = 0;

  auto get_content = [=, this, &current_index](XContentType type) {
    const auto entries = content_manager_->ListContent(
        0, 0, kernel::kDashboardID, type, xe::kernel::xam::XContentFlag::kNone);

    for (const auto& entry : entries) {
      const auto package = content_manager_->OpenPackage(0, entry);
      if (!package) {
        continue;
      }

      ContentListEntry content_entry{
          .index = current_index++,
          .title_id = package->GetContentMetadata().title_id,
          .size = package->GetPackageSize(),
          .is_package = package->GetPackageType() ==
                        kernel::xam::ContentPackageType::Package,
          .name = entry.display_name(),
          .path = package->GetPackageHostPath(),
      };

      content_entry.title_name = package->GetContainerMetadata()->title_name();

      std::vector<uint8_t> buffer{};

      if (package->GetThumbnail(buffer) == X_ERROR_SUCCESS) {
        content_entry.icon = imgui_drawer()->LoadImGuiIcon(buffer);
      }

      enumerated_entries_[type].push_back(std::move(content_entry));
    }

    if (enumerated_entries_.contains(type) &&
        enumerated_entries_[type].size() > 1) {
      std::ranges::sort(enumerated_entries_[type], {},
                        &ContentListEntry::title_id);
    }
  };

  for (const auto& [_, types] : typesToEnumerate) {
    for (const auto& type : types) {
      get_content(type);
    }
  }
}

void ContentListDialog::OnCategoryDraw(
    ImGuiIO& io, const XContentType type,
    const std::vector<ContentListEntry>& entries) {
  // Add optional search?
  if (ImGui::BeginTable("", 2, ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableNextRow(0, xe::ui::default_image_icon_size.y);

    for (const auto& entry : entries) {
      OnEntryDraw(io, type, entry);
    }

    ImGui::EndTable();
  }
}

void ContentListDialog::OnEntryDraw(ImGuiIO& io, const XContentType type,
                                    const ContentListEntry& entry) {
  auto size_to_string = [](size_t bytes) -> std::string {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < xe::countof(units) - 1) {
      v /= 1024.0;
      ++i;
    }
    return i == 0 ? std::format("{} B", bytes)
                  : std::format("{:.1f} {}", v, units[i]);
  };

  const auto start_position = ImGui::GetCursorPos();
  // First Column
  ImGui::TableSetColumnIndex(0);
  if (entry.icon) {
    ImGui::Image(reinterpret_cast<ImTextureID>(entry.icon.get()),
                 ui::default_image_icon_size);
  } else {
    ImGui::Dummy(ui::default_image_icon_size);
  }

  // Second Column
  ImGui::TableNextColumn();
  if (entry.is_package) {
    ImGui::Text("Name: %s (Filename: %s)", xe::to_utf8(entry.name).c_str(),
                xe::path_to_utf8(entry.path.filename()).c_str());
  } else {
    ImGui::Text("Name: %s", xe::to_utf8(entry.name).c_str());
  }

  if (!entry.title_name.empty()) {
    ImGui::Text("Title Name: %s (Title ID: %s)",
                xe::to_utf8(entry.title_name).c_str(),
                fmt::format("{:08X}", entry.title_id).c_str());
  } else {
    ImGui::Text("Title ID: %s", fmt::format("{:08X}", entry.title_id).c_str());
  }

  ImGui::Text("Content Installation Type: %s",
              entry.is_package ? "Package" : "Directory");
  ImGui::Text("Installation Size: %s", size_to_string(entry.size).c_str());

  // Third Column
  ImGui::TableNextColumn();
  const ImVec2 end_draw_position =
      ImVec2(ImGui::GetCursorPos().x - start_position.x,
             ImGui::GetCursorPos().y - start_position.y);

  ImGui::SetCursorPos(start_position);
  if (ImGui::Selectable(fmt::format("##{:08X}Selectable", entry.index).c_str(),
                        selected_entry_ == entry.index,
                        ImGuiSelectableFlags_SpanAllColumns,
                        end_draw_position)) {
    selected_entry_ = entry.index;
    ImGui::OpenPopup(fmt::format("Popup-{}", xe::to_utf8(entry.name)).c_str());
  }

  if (ImGui::BeginPopupContextItem(
          fmt::format("Popup-{}", xe::to_utf8(entry.name)).c_str())) {
    selected_entry_ = entry.index;

    if (isBootableType(type)) {
      ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
      if (ImGui::MenuItem("Start title", nullptr, nullptr, true)) {
        pending_boot_ = {&entry};
      }

      ImGui::EndDisabled();
    }

    if (ImGui::MenuItem("Open location", nullptr, nullptr, true)) {
      if (entry.is_package) {
        std::thread path_open(LaunchFileExplorer, entry.path.parent_path());
        path_open.detach();
      } else {
        std::thread path_open(LaunchFileExplorer, entry.path);
        path_open.detach();
      }
    }
    if (entry.is_package) {
      if (ImGui::MenuItem("Extract", nullptr, nullptr, true)) {
        const auto path = entry.path;
        emulator_window_.window()->app_context().CallInUIThread(
            [emulator_window = &emulator_window_, path]() {
              emulator_window->ExtractContent(path);
            });
      }
    }

    ImGui::BeginDisabled(emulator_window_.emulator()->is_title_open());
    if (ImGui::BeginMenu("Delete")) {
      if (ImGui::MenuItem("Yes, delete it!")) {
        pending_deletion_ = {type, &entry};
      }
      ImGui::EndMenu();
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
  }
}

void ContentListDialog::OnDraw(ImGuiIO& io) {
  ExecutePendingStart();
  ExecutePendingDeletion();

  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  const auto viewport_size = ImGui::GetMainViewport()->Size;
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(viewport_size.x * 0.05f, viewport_size.y * 0.05f),
      ImVec2(viewport_size.x * 0.75f, viewport_size.y * 0.85f));

  ImGui::SetNextWindowBgAlpha(0.90f);
  bool dialog_open = true;
  if (!ImGui::Begin(
          "Content List", &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (ImGui::BeginTabBar("mainTab")) {
    for (const auto& [name, types] : typesToEnumerate) {
      if (ImGui::BeginTabItem(fmt::format("{}", name).c_str())) {
        bool is_everything_empty = true;
        for (const auto& type : types) {
          if (enumerated_entries_.contains(type)) {
            is_everything_empty = false;
            OnCategoryDraw(io, type, enumerated_entries_.at(type));
          }
        }
        if (is_everything_empty) {
          ImGui::Text("No %s available", name.c_str());
        }
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  if (!dialog_open) {
    Close();
    ImGui::End();
    emulator_window_.ToggleContentListDialog();
    return;
  }

  ImGui::End();
}

void ContentListDialog::ExecutePendingStart() {
  if (pending_boot_ && !emulator_window_.emulator()->is_title_open()) {
    // Path must be provided as a copy, not reference.
    const auto path = pending_boot_.value()->path;
    pending_boot_.reset();

    std::jthread(
        [path](EmulatorWindow* emulator_window) {
          emulator_window->RunTitle(path);
        },
        &emulator_window_)
        .detach();
  }
}

void ContentListDialog::ExecutePendingDeletion() {
  if (!pending_deletion_) {
    return;
  }

  const auto [type, entry] = pending_deletion_.value();
  pending_deletion_.reset();

  const auto file_to_delete = entry->path;
  const bool is_directory_package = !entry->is_package;

  if (std::erase_if(enumerated_entries_[type], [&](const ContentListEntry& e) {
        return &e == entry;
      }) == 0) {
    return;
  }

  if (enumerated_entries_[type].empty()) {
    enumerated_entries_.erase(type);
  }

  // Before removing anything ensure that path is located within content.
  // We REALLY don't want to yeet everything!
  const auto content_dir = emulator_window_.emulator()->content_root() /
                           kernel::xam::kCommonContentDirectory;
  if (!xe::path_to_utf8(file_to_delete)
           .starts_with(xe::path_to_utf8(content_dir))) {
    return;
  }

  std::filesystem::remove_all(file_to_delete);

  // Remove header file for "directory" type packages
  if (is_directory_package) {
    const auto header_path =
        file_to_delete.parent_path().parent_path() /
        kernel::xam::kGameContentHeaderDirName /
        fmt::format("{:08X}", static_cast<uint32_t>(type)) /
        (file_to_delete.filename().string() + ".header");
    std::error_code ec;
    std::filesystem::remove(header_path, ec);  // fine if it doesn't exist
  }

  for (auto path = file_to_delete.parent_path();
       path.filename() != kernel::xam::kCommonContentDirectory;
       path = path.parent_path()) {
    const auto dirs = xe::filesystem::ListDirectories(path);
    const bool header_dir =
        dirs.size() == 1 && xe::path_to_utf8(dirs[0].name.filename()) ==
                                kernel::xam::kGameContentHeaderDirName;

    if (!std::filesystem::is_empty(path) && !header_dir) {
      break;
    }
    std::filesystem::remove_all(path);
  }
}

}  // namespace app
}  // namespace xe
