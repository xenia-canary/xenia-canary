/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"

#include <ranges>

namespace xe {
namespace kernel {
namespace xam {

const std::vector<const X_XDBF_GPD_TITLE_PLAYED*>
GpdInfoProfile::GetTitlesInfo() const {
  std::vector<const X_XDBF_GPD_TITLE_PLAYED*> entries;

  auto titles = entries_ | std::views::filter([](const auto& entry) {
                  return !IsSyncEntry(&entry);
                }) |
                std::views::filter([](const auto& entry) {
                  return IsEntryOfSection(&entry, GpdSection::kTitle);
                });

  for (const auto& title : titles) {
    entries.push_back(
        reinterpret_cast<const X_XDBF_GPD_TITLE_PLAYED*>(title.data.data()));
  }
  return entries;
};

X_XDBF_GPD_TITLE_PLAYED* GpdInfoProfile::GetTitleInfo(const uint32_t title_id) {
  auto title = entries_ | std::views::filter([](const auto& entry) {
                 return !IsSyncEntry(&entry);
               }) |
               std::views::filter([](const auto& entry) {
                 return IsEntryOfSection(&entry, GpdSection::kTitle);
               }) |
               std::views::filter([title_id](const auto& entry) {
                 return static_cast<uint32_t>(entry.info.id) == title_id;
               });

  if (title.empty()) {
    return nullptr;
  }

  return reinterpret_cast<X_XDBF_GPD_TITLE_PLAYED*>(title.begin()->data.data());
}

std::u16string GpdInfoProfile::GetTitleName(const uint32_t title_id) const {
  const Entry* entry =
      GetEntry(static_cast<uint16_t>(GpdSection::kTitle), title_id);

  if (!entry) {
    return std::u16string();
  }

  return string_util::read_u16string_and_swap(reinterpret_cast<const char16_t*>(
      entry->data.data() + sizeof(X_XDBF_GPD_TITLE_PLAYED)));
}

void GpdInfoProfile::AddNewTitle(const SpaInfo* title_data) {
  AddNewTitle(title_data->title_id(), xe::to_utf16(title_data->title_name()),
              title_data->achievement_count(), title_data->total_gamerscore());
}

void GpdInfoProfile::AddNewTitle(uint32_t title_id,
                                 const std::u16string& title_name,
                                 uint32_t achievement_count,
                                 uint32_t total_gamerscore) {
  X_XDBF_GPD_TITLE_PLAYED title_gpd_data = {};
  title_gpd_data.title_id = title_id;
  title_gpd_data.achievements_count = achievement_count;
  title_gpd_data.gamerscore_total = total_gamerscore;

  const uint32_t entry_size =
      sizeof(X_XDBF_GPD_TITLE_PLAYED) +
      static_cast<uint32_t>(string_util::size_in_bytes(title_name));

  Entry entry(title_id, static_cast<uint16_t>(GpdSection::kTitle), entry_size);

  memcpy(entry.data.data(), &title_gpd_data, sizeof(X_XDBF_GPD_TITLE_PLAYED));

  string_util::copy_and_swap_truncating(
      reinterpret_cast<char16_t*>(entry.data.data() +
                                  sizeof(X_XDBF_GPD_TITLE_PLAYED)),
      title_name, title_name.size() + 1);

  UpsertEntry(&entry);
}

bool GpdInfoProfile::RemoveTitle(const uint32_t title_id) {
  // Find the entry by searching through all title entries and matching the
  // title_id in the data (not the entry ID, which is different!)
  const Entry* entry_to_delete = nullptr;

  for (const auto& e : entries_) {
    if (e.info.section.get() == static_cast<uint16_t>(GpdSection::kTitle)) {
      if (e.data.size() >= sizeof(X_XDBF_GPD_TITLE_PLAYED)) {
        auto* title_data =
            reinterpret_cast<const X_XDBF_GPD_TITLE_PLAYED*>(e.data.data());
        if (title_data->title_id.get() == title_id) {
          entry_to_delete = &e;
          break;
        }
      }
    }
  }

  if (!entry_to_delete) {
    return false;
  }

  DeleteEntry(entry_to_delete);
  return true;
}

void GpdInfoProfile::UpdateTitleInfo(const uint32_t title_id,
                                     X_XDBF_GPD_TITLE_PLAYED* title_data) {
  X_XDBF_GPD_TITLE_PLAYED* current_info = GetTitleInfo(title_id);
  if (!current_info) {
    return;
  }

  memcpy(current_info, title_data, sizeof(X_XDBF_GPD_TITLE_PLAYED));
}

// Xenia-specific: Use custom string IDs to store file paths and optional labels
// String IDs 0xFFFE0000 - 0xFFFEFFFF are reserved for Xenia extensions
// For multi-disc titles, we store paths (with optional labels) separated by
// newline Format: "label1::path1\npath2\nlabel3::path3"
// - If a line contains '::', split on first '::' as label::path
// - If no '::', the entire line is the path and label defaults to "Disc N"
// - Labels cannot contain '::' sequence
constexpr uint32_t kXeniaPathStringBase = 0xFFFE0000;
constexpr char kPathDelimiter = '\n';
constexpr std::string_view kLabelDelimiter = "::";

// Helper to strip label from path string (format: "label::path" -> "path")
static std::string StripLabel(const std::string& path_with_label) {
  size_t delim_pos = path_with_label.find(kLabelDelimiter);
  if (delim_pos != std::string::npos) {
    return path_with_label.substr(delim_pos + kLabelDelimiter.size());
  }
  return path_with_label;
}

// Helper to parse label and path from string (format: "label::path" or just
// "path") Returns pair of (label, path). If no label present, label will be
// empty string.
static std::pair<std::string, std::string> ParseLabelAndPath(
    const std::string& line) {
  size_t delim_pos = line.find(kLabelDelimiter);
  if (delim_pos != std::string::npos) {
    return {line.substr(0, delim_pos),
            line.substr(delim_pos + kLabelDelimiter.size())};
  }
  return {"", line};
}

std::optional<std::filesystem::path> GpdInfoProfile::GetTitlePath(
    uint32_t title_id) const {
  const uint32_t string_id = kXeniaPathStringBase + title_id;
  const std::u16string path_u16 = GetString(string_id);

  if (path_u16.empty()) {
    return std::nullopt;
  }

  // Get the first path (before any newline)
  std::string paths_utf8 = xe::to_utf8(path_u16);
  size_t delimiter_pos = paths_utf8.find(kPathDelimiter);
  if (delimiter_pos != std::string::npos) {
    paths_utf8 = paths_utf8.substr(0, delimiter_pos);
  }

  // Strip label if present (format: "label::path")
  paths_utf8 = StripLabel(paths_utf8);

  return xe::to_path(paths_utf8);
}

std::vector<GpdInfoProfile::DiscInfo> GpdInfoProfile::GetTitleDiscs(
    uint32_t title_id) const {
  std::vector<DiscInfo> discs;

  const uint32_t string_id = kXeniaPathStringBase + title_id;
  const std::u16string data_u16 = GetString(string_id);

  if (data_u16.empty()) {
    return discs;
  }

  // Split by newline delimiter
  std::string data_utf8 = xe::to_utf8(data_u16);
  size_t start = 0;
  size_t end = 0;
  size_t disc_num = 1;

  while ((end = data_utf8.find(kPathDelimiter, start)) != std::string::npos) {
    std::string line = data_utf8.substr(start, end - start);
    if (!line.empty()) {
      DiscInfo disc;
      auto [label, path_str] = ParseLabelAndPath(line);
      disc.path = xe::to_path(path_str);
      disc.label = label;  // Keep empty if no label in storage
      discs.push_back(disc);
      disc_num++;
    }
    start = end + 1;
  }

  // Add the last entry (or only entry if no delimiter)
  if (start < data_utf8.length()) {
    std::string line = data_utf8.substr(start);
    if (!line.empty()) {
      DiscInfo disc;
      auto [label, path_str] = ParseLabelAndPath(line);
      disc.path = xe::to_path(path_str);
      disc.label = label;  // Keep empty if no label in storage
      discs.push_back(disc);
    }
  }

  return discs;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
