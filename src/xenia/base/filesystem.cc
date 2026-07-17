/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/filesystem.h"

#include <utility>

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

  std::ranges::copy_if(std::as_const(files), std::back_inserter(directories),
                       [](const FileInfo& file) {
                         return file.type == FileInfo::Type::kDirectory;
                       });

  return directories;
}

std::vector<FileInfo> FilterByName(const std::vector<FileInfo>& files,
                                   const std::regex pattern) {
  std::vector<FileInfo> filtered_entries = {};

  std::ranges::copy_if(files, std::back_inserter(filtered_entries),
                       [pattern](const FileInfo& file) {
                         return std::regex_match(file.name.filename().string(),
                                                 pattern);
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

bool Copy(const std::filesystem::path& source,
          const std::filesystem::path& destination, uint64_t& progress,
          size_t copy_chunk) {
  auto source_file = xe::filesystem::OpenFile(source, "rb");
  if (!source_file) {
    return false;
  }

  auto destination_file = xe::filesystem::OpenFile(destination, "wb");
  if (!destination_file) {
    fclose(source_file);
    return false;
  }

  std::vector<uint8_t> buffer;
  buffer.resize(copy_chunk);

  const size_t file_size = std::filesystem::file_size(source);
  size_t offset = 0;

  while (offset < file_size) {
    const auto read_count = fread(buffer.data(), 1, copy_chunk, source_file);
    if (read_count == 0) {
      break;
    }

    const auto write_count =
        fwrite(buffer.data(), 1, read_count, destination_file);
    if (write_count == 0) {
      break;
    }
    offset += write_count;
    progress += read_count;
  }

  fclose(source_file);
  fclose(destination_file);
  return file_size == std::filesystem::file_size(destination);
}

}  // namespace filesystem
}  // namespace xe
