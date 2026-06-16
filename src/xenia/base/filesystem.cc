/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/filesystem.h"

#include "xenia/base/string_util.h"

namespace xe {
namespace filesystem {

bool CreateParentFolder(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    auto parent_path = path.parent_path();
    if (!std::filesystem::exists(parent_path)) {
      return std::filesystem::create_directories(parent_path);
    }
  }
  return true;
}

std::error_code CreateFolder(const std::filesystem::path& path) {
  if (std::filesystem::exists(path)) {
    return {};
  }

  std::error_code ec;
  if (std::filesystem::create_directories(path, ec)) {
    return {};
  }

  return ec;
}

std::vector<FileInfo> ListDirectories(const std::filesystem::path& path) {
  std::vector<FileInfo> files = ListFiles(path);
  std::vector<FileInfo> directories = {};

  std::copy_if(files.cbegin(), files.cend(), std::back_inserter(directories),
               [](const FileInfo& file) {
                 return file.type == FileInfo::Type::kDirectory;
               });

  return directories;
}

std::vector<FileInfo> FilterByName(const std::vector<FileInfo>& files,
                                   const std::regex pattern) {
  std::vector<FileInfo> filtered_entries = {};

  std::copy_if(
      files.cbegin(), files.cend(), std::back_inserter(filtered_entries),
      [pattern](const FileInfo& file) {
        return std::regex_match(file.name.filename().string(), pattern);
      });
  return filtered_entries;
}

std::vector<FileInfo> FindFileWithName(const std::filesystem::path& path,
                                       std::string_view name, bool recursive) {
  if (!std::filesystem::exists(path)) {
    return {};
  }

  if (!std::filesystem::is_directory(path)) {
    return {};
  }

  if (!recursive) {
    return FilterByName(ListFiles(path), std::regex(std::string(name)));
  }

  const std::string file_name = xe::utf8::lower_ascii(name);

  std::vector<FileInfo> filtered_entries = {};
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(path)) {
    if (entry.is_regular_file() && xe::utf8::lower_ascii(xe::path_to_utf8(
                                       entry.path().filename())) == file_name) {
      auto file_info = GetInfo(entry.path());
      if (file_info) {
        filtered_entries.push_back(std::move(file_info.value()));
      }
    }
  }
  return filtered_entries;
}

}  // namespace filesystem
}  // namespace xe
