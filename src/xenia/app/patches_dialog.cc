/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/patches_dialog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/imgui/imgui.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/system.h"
#include "xenia/emulator.h"

#ifdef XE_PLATFORM_WIN32
#include <shellapi.h>
#include <windows.h>
#endif

namespace xe {
namespace app {

PatchesDialog::PatchesDialog(ui::ImGuiDrawer* imgui_drawer,
                             EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  auto emulator = emulator_window_->emulator();
  if (emulator) {
    patches_directory_ = emulator->storage_root() / "patches";
    patch_db_ = std::make_unique<patcher::PatchDB>(emulator->storage_root());
    LoadPatchFiles();
  }
}

void PatchesDialog::LoadPatchFiles() {
  title_patches_.clear();

  if (!std::filesystem::exists(patches_directory_)) {
    std::filesystem::create_directories(patches_directory_);
    XELOGI("Created patches directory");
  }

  auto& all_patches = patch_db_->GetAllPatches();

  for (const auto& patch_file : all_patches) {
    TitlePatchData data;
    data.title_id = patch_file.title_id;
    data.title_name = patch_file.title_name;
    data.filename = patch_file.filename;
    data.hashes = patch_file.hashes;
    data.is_expanded = false;

    for (size_t i = 0; i < patch_file.patch_info.size(); ++i) {
      PatchInfo info;
      info.id = static_cast<uint32_t>(i);
      info.name = patch_file.patch_info[i].patch_name;
      info.description = patch_file.patch_info[i].patch_desc;
      info.author = patch_file.patch_info[i].patch_author;
      info.is_enabled = patch_file.patch_info[i].is_enabled;
      data.patches.push_back(std::move(info));
    }

    title_patches_.push_back(std::move(data));
  }

  // Sort patches by title name
  std::sort(title_patches_.begin(), title_patches_.end(),
            [](const TitlePatchData& a, const TitlePatchData& b) {
              return a.title_name < b.title_name;
            });
}

bool PatchesDialog::UpdateSinglePatchEnabledLine(
    std::vector<std::string>& lines, size_t patch_index, bool new_value) {
  // Regex patterns for matching TOML structures
  static const std::regex patch_header_regex(R"(^\s*\[\[patch\]\]\s*$)");
  static const std::regex section_header_regex(R"(^\s*\[([^\[\]]+)\]\s*$)");
  static const std::regex is_enabled_regex(R"(^\s*is_enabled\s*=\s*(.+))");
  static const std::regex comment_line_regex(R"(^\s*#.*)");

  // Find all is_enabled lines in patch blocks
  std::vector<size_t> enabled_lines;
  bool in_patch_block = false;

  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];

    // Skip comment lines
    if (std::regex_match(line, comment_line_regex)) {
      continue;
    }

    // Check for patch section marker
    if (std::regex_match(line, patch_header_regex)) {
      in_patch_block = true;
      continue;
    }

    // Check if we've hit another top-level section (not an array element)
    if (std::regex_match(line, section_header_regex)) {
      in_patch_block = false;
      continue;
    }

    // Look for is_enabled field within patch blocks
    if (in_patch_block) {
      if (std::regex_search(line, is_enabled_regex)) {
        enabled_lines.push_back(i);
      }
    }
  }

  // Check if the patch index is valid
  if (patch_index >= enabled_lines.size()) {
    XELOGE("Patch index {} out of range (found {} is_enabled fields)",
           patch_index, enabled_lines.size());
    return false;
  }

  // Update only the specific is_enabled line for this patch
  size_t line_idx = enabled_lines[patch_index];
  std::string& enabled_line = lines[line_idx];

  // Use regex to replace the value while preserving formatting
  static const std::regex value_regex(
      R"((^\s*is_enabled\s*=\s*)(true|false)(.*)$)");
  std::smatch match;

  if (std::regex_match(enabled_line, match, value_regex)) {
    // match[1] = everything before the value
    // match[2] = the current value (true/false)
    // match[3] = everything after the value (comments)
    lines[line_idx] =
        match[1].str() + (new_value ? "true" : "false") + match[3].str();
  } else {
    XELOGE("Failed to parse is_enabled line for patch #{}", patch_index + 1);
  }

  return true;
}

void PatchesDialog::SavePatchToggle(const std::string& filename,
                                    size_t patch_index, bool new_value) {
  std::filesystem::path patch_file_path = patches_directory_ / filename;

  if (!std::filesystem::exists(patch_file_path)) {
    XELOGE("Patch file does not exist: {}", filename);
    return;
  }

  try {
    // Read the original file content to preserve comments and formatting
    std::ifstream infile(patch_file_path);
    if (!infile.is_open()) {
      XELOGE("Failed to open patch file for reading: {}", filename);
      return;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(infile, line)) {
      lines.push_back(line);
    }
    infile.close();

    // Update only the specific patch's is_enabled value
    if (!UpdateSinglePatchEnabledLine(lines, patch_index, new_value)) {
      XELOGE("Failed to update patch #{} enabled state in {}", patch_index + 1,
             filename);
      return;
    }

    // Write the modified content back to file
    std::ofstream outfile(patch_file_path, std::ios::out | std::ios::trunc);
    if (!outfile.is_open()) {
      XELOGE("Failed to open patch file for writing: {}", filename);
      return;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
      outfile << lines[i];
      if (i < lines.size() - 1) {
        outfile << "\n";
      }
    }
    outfile.close();

    XELOGI("Updated patch #{} in {}", patch_index + 1, filename);

  } catch (const std::exception& e) {
    XELOGE("Failed to save patch toggle: {}", e.what());
  }

  needs_reload_ = true;
}

void PatchesDialog::OnDraw(ImGuiIO& io) {
  if (!emulator_window_->emulator()) {
    ImGui::Text("Emulator not initialized");
    return;
  }

  // Set window size
  ImVec2 window_size = ImVec2(800, 600);
  ImGui::SetNextWindowSize(window_size, ImGuiCond_FirstUseEver);

  // Center the window - calculate position based on viewport
  ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
  ImVec2 window_pos = ImVec2((viewport_size.x - window_size.x) * 0.5f,
                             (viewport_size.y - window_size.y) * 0.5f);
  ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);

  bool window_open = true;
  if (!ImGui::Begin("Patch Manager", &window_open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoResize)) {
    ImGui::End();
    return;
  }

  if (!window_open) {
    ImGui::End();
    Close();
    return;
  }

  ImGui::TextWrapped(
      "Manage game patches. Enable or disable patches for "
      "individual games. Changes will take effect on next game "
      "launch.");
  ImGui::Separator();

  if (filter_text_.size() < 256) {
    filter_text_.resize(256);
  }

  // Set focus to filter input when dialog first opens
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }

  ImGui::SetNextItemWidth(-1);  // Use full width
  ImGui::InputTextWithHint(
      "##patch_filter",
      "Filter by title, media ID, or patch name... (Esc to clear)",
      filter_text_.data(), filter_text_.size(),
      ImGuiInputTextFlags_EscapeClearsAll);

  if (ImGui::Button("Open Patches Directory")) {
    if (!std::filesystem::exists(patches_directory_)) {
      std::filesystem::create_directories(patches_directory_);
    }
    LaunchFileExplorer(patches_directory_);
  }

  ImGui::Separator();

  if (title_patches_.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "No patch files found in: %s",
                       patches_directory_.string().c_str());
    ImGui::TextWrapped(
        "Place .patch.toml files in the patches directory. "
        "Patch files should be named with the title ID "
        "(e.g., 4D530910.patch.toml)");
  } else {
    ImGui::BeginChild("PatchList", ImVec2(0, -30), true);

    for (auto& patch_data : title_patches_) {
      // Parse filter into tokens for fuzzy search
      std::string filter_lower(filter_text_.data());
      std::transform(filter_lower.begin(), filter_lower.end(),
                     filter_lower.begin(), ::tolower);

      std::vector<std::string> filter_tokens;
      if (!filter_lower.empty()) {
        std::string current_token;
        for (char c : filter_lower) {
          if (c == ' ' && !current_token.empty()) {
            filter_tokens.push_back(current_token);
            current_token.clear();
          } else if (c != ' ') {
            current_token += c;
          }
        }
        if (!current_token.empty()) {
          filter_tokens.push_back(current_token);
        }
      }

      // Check if any patches match the filter
      if (!filter_tokens.empty()) {
        bool has_visible_patches = false;
        // Create base searchable text for this game (title + filename)
        std::string base_search_text =
            patch_data.title_name + " " + patch_data.filename;
        std::transform(base_search_text.begin(), base_search_text.end(),
                       base_search_text.begin(), ::tolower);

        for (const auto& patch : patch_data.patches) {
          // Create combined searchable text for each patch
          std::string patch_search_text = base_search_text + " " + patch.name;
          std::transform(patch_search_text.begin(), patch_search_text.end(),
                         patch_search_text.begin(), ::tolower);

          // Check if all filter tokens match
          bool all_tokens_match = true;
          for (const auto& token : filter_tokens) {
            if (patch_search_text.find(token) == std::string::npos) {
              all_tokens_match = false;
              break;
            }
          }

          if (all_tokens_match) {
            has_visible_patches = true;
            break;
          }
        }

        // Skip this game if no patches match
        if (!has_visible_patches) {
          continue;
        }
      }

      // Create unique ID from title_id and hashes
      std::string unique_id = fmt::format("{:08X}", patch_data.title_id);
      for (const auto& hash : patch_data.hashes) {
        unique_id += fmt::format("_{:016X}", hash);
      }
      ImGui::PushID(fmt::format("patch_game_{}", unique_id).c_str());

      // Auto-expand if filter is active and there are matches
      bool should_expand = !filter_lower.empty();
      ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_None;
      if (should_expand) {
        node_flags |= ImGuiTreeNodeFlags_DefaultOpen;
      }

      // Remove .patch.toml extension for display
      std::string display_name = patch_data.filename;
      const std::string patch_toml_ext = ".patch.toml";
      const std::string toml_ext = ".toml";

      if (display_name.size() > patch_toml_ext.size() &&
          display_name.substr(display_name.size() - patch_toml_ext.size()) ==
              patch_toml_ext) {
        display_name.resize(display_name.size() - patch_toml_ext.size());
      } else if (display_name.size() > toml_ext.size() &&
                 display_name.substr(display_name.size() - toml_ext.size()) ==
                     toml_ext) {
        display_name.resize(display_name.size() - toml_ext.size());
      }

      if (ImGui::TreeNodeEx(display_name.c_str(), node_flags)) {
        if (!patch_data.patches.empty()) {
          int visible_patches = 0;

          // Prepare base searchable text if filtering
          std::string base_search_text;
          if (!filter_tokens.empty()) {
            base_search_text =
                patch_data.title_name + " " + patch_data.filename;
            std::transform(base_search_text.begin(), base_search_text.end(),
                           base_search_text.begin(), ::tolower);
          }

          for (auto& patch : patch_data.patches) {
            // Filter individual patches if there's an active filter
            if (!filter_tokens.empty()) {
              // Create combined searchable text for this patch
              std::string patch_search_text =
                  base_search_text + " " + patch.name;
              std::transform(patch_search_text.begin(), patch_search_text.end(),
                             patch_search_text.begin(), ::tolower);

              // Check if all filter tokens match
              bool all_tokens_match = true;
              for (const auto& token : filter_tokens) {
                if (patch_search_text.find(token) == std::string::npos) {
                  all_tokens_match = false;
                  break;
                }
              }

              if (!all_tokens_match) {
                continue;  // Skip this patch if not all tokens match
              }
            }

            visible_patches++;
            ImGui::PushID(fmt::format("patch_item_{}", patch.id).c_str());

            if (ImGui::Checkbox(patch.name.c_str(), &patch.is_enabled)) {
              XELOGI("\"{}\" {}", patch.name,
                     patch.is_enabled ? "enabled" : "disabled");
              SavePatchToggle(patch_data.filename, patch.id, patch.is_enabled);
            }

            if (!patch.description.empty()) {
              ImGui::SameLine();
              ImGui::TextDisabled("(?)");
              if (ImGui::IsItemHovered()) {
                // Calculate optimal width based on text length
                float text_width =
                    ImGui::CalcTextSize(patch.description.c_str()).x;
                float author_width =
                    patch.author.empty()
                        ? 0.0f
                        : ImGui::CalcTextSize(
                              fmt::format("Author: {}", patch.author).c_str())
                              .x;
                float max_line_width = std::max(text_width, author_width);
                float tooltip_width =
                    std::min(400.0f, std::max(200.0f, max_line_width + 20.0f));

                ImGui::SetNextWindowContentSize(ImVec2(tooltip_width, 0.0f));
                ImGui::BeginTooltip();
                ImGui::TextWrapped("%s", patch.description.c_str());
                if (!patch.author.empty()) {
                  ImGui::Text("Author: %s", patch.author.c_str());
                }
                ImGui::EndTooltip();
              }
            }

            ImGui::PopID();
          }

          if (visible_patches == 0) {
            ImGui::TextDisabled("No patches match the filter");
          }
        } else {
          ImGui::TextDisabled("No patches defined in this file");
        }

        ImGui::TreePop();
      }

      ImGui::PopID();
    }

    ImGui::EndChild();
  }

  if (needs_reload_) {
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "Changes will take effect on next game launch");
  }

  ImGui::End();
}

}  // namespace app
}  // namespace xe
