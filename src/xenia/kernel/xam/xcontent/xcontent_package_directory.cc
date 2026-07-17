/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/content_manager.h"

#include <array>
#include <string>
#include <unordered_set>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xobject.h"
#include "xenia/vfs/devices/host_path_device.h"

#include "xenia/kernel/xam/xcontent/xcontent_package_directory.h"

DECLARE_int32(license_mask);

namespace xe {
namespace kernel {
namespace xam {

static std::string_view kThumbnailFileName = "__thumbnail.png";

std::filesystem::path ComputeHeaderPath(
    const std::filesystem::path& package_path) {
  const auto package_name =
      xe::path_to_utf8(package_path.filename()) + ".header";
  const auto content_type = package_path.parent_path().filename();

  return package_path.parent_path().parent_path() / kGameContentHeaderDirName /
         content_type / package_name;
}

ContentPackageDirectory::ContentPackageDirectory(
    vfs::VirtualFileSystem* file_system, const std::string_view device_path,
    const std::filesystem::path& package_path)
    : ContentPackage(file_system, device_path, package_path) {
  // Compute header path based on package_path
  // We assume that package_path will always be at least 2-3 levels deep.
  header_path_ = ComputeHeaderPath(package_path);
  ReadContentHeaderFile();
}

ContentPackageDirectory::ContentPackageDirectory(
    vfs::VirtualFileSystem* file_system, const std::string_view device_path,
    const std::filesystem::path& package_path,
    const XCONTENT_DATA_INTERNAL& metadata,
    const xex2_opt_execution_info* execution_info, const SpaInfo* spa_info)
    : ContentPackage(file_system, device_path, package_path, metadata,
                     execution_info, spa_info) {
  header_path_ = ComputeHeaderPath(package_path);
}

ContentPackageDirectory::~ContentPackageDirectory() {
  // Only update header file if package was fully mounted.
  if (device_) {
    WriteContentHeaderFile();
  }
}

std::unique_ptr<vfs::Device> ContentPackageDirectory::MountPackage() {
  return std::make_unique<vfs::HostPathDevice>(device_path_, host_path_, false);
}

X_RESULT ContentPackageDirectory::SetThumbnail(
    std::span<const uint8_t> thumbnail) {
  const size_t max_thumbnail_size =
      header_.content_metadata.metadata_version == 1
          ? header_.content_metadata.kThumbLengthV1
          : header_.content_metadata.kThumbLengthV2;

  if (thumbnail.size() > max_thumbnail_size) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  memcpy(header_.content_metadata.thumbnail, thumbnail.data(),
         std::min(thumbnail.size(), max_thumbnail_size));

  header_.content_metadata.thumbnail_size = thumbnail.size();
  return X_ERROR_SUCCESS;
}

X_RESULT ContentPackageDirectory::GetThumbnail(std::vector<uint8_t>& buffer) {
  auto thumb_path = host_path_ / kThumbnailFileName;

  if (auto file = xe::filesystem::OpenFile(thumb_path, "rb"); file) {
    size_t file_len = std::filesystem::file_size(thumb_path);

    buffer.resize(file_len);
    const size_t read_bytes =
        fread(buffer.data(), 1, std::min(buffer.size(), file_len), file);
    fclose(file);

    const size_t max_thumbnail_size =
        header_.content_metadata.metadata_version == 1
            ? header_.content_metadata.kThumbLengthV1
            : header_.content_metadata.kThumbLengthV2;
    const size_t thumbnail_size = std::min(read_bytes, max_thumbnail_size);
    // Update package adhoc to include proper thumbnail on save.
    memcpy(header_.content_metadata.thumbnail, buffer.data(), thumbnail_size);
    header_.content_metadata.thumbnail_size =
        static_cast<uint32_t>(thumbnail_size);
    std::filesystem::remove(thumb_path);
    return X_ERROR_SUCCESS;
  }

  if (header_.content_metadata.thumbnail_size > 0) {
    buffer.resize(header_.content_metadata.thumbnail_size);
    std::copy(header_.content_metadata.thumbnail,
              header_.content_metadata.thumbnail +
                  header_.content_metadata.thumbnail_size,
              buffer.begin());
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

X_RESULT ContentPackageDirectory::ReadContentHeaderFile() {
  if (!std::filesystem::exists(header_path_)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file = xe::filesystem::OpenFile(header_path_, "rb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file_size = std::filesystem::file_size(header_path_);
  if (file_size < sizeof(XCONTENT_DATA_AGGREGATE)) {
    fclose(file);
    return X_STATUS_END_OF_FILE;
  }

  if (file_size < sizeof(XContentHeader) + sizeof(XContentMetadata)) {
    // Assume that we're in old header
    XCONTENT_DATA_AGGREGATE data{};
    const size_t result =
        fread(&data, 1, sizeof(XCONTENT_DATA_AGGREGATE), file);

    if (result != sizeof(XCONTENT_DATA_AGGREGATE)) {
      fclose(file);
      return X_STATUS_END_OF_FILE;
    }

    FillHeader(data);

    // Read License mask. Based on previous read we should be in proper offset.
    fread(&license_mask_, 1, sizeof(uint32_t), file);
    // Some function to map this to header_
    // ...
  } else if (file_size == sizeof(XContentHeader) + sizeof(XContentMetadata) ||
             file_size ==
                 xe::round_up(sizeof(XContentContainerHeader), 0x1000)) {
    fread(&header_, 1, sizeof(XContentContainerHeader), file);
  } else {
    fclose(file);
    return X_ERROR_FILE_NOT_FOUND;
  }

  fclose(file);
  return X_STATUS_SUCCESS;
}

X_RESULT ContentPackageDirectory::WriteContentHeaderFile() const {
  return ContentManager::ExtractContentHeader(header_path_, header_);
}

size_t ContentPackageDirectory::GetPackageSize() const {
  size_t total = 0;
  for (auto& entry : std::filesystem::recursive_directory_iterator(
           GetPackageHostPath(),
           std::filesystem::directory_options::skip_permission_denied)) {
    if (entry.is_regular_file()) {
      total += entry.file_size();
    }
  }
  return total;
}

void ContentPackageDirectory::FillHeader(const XCONTENT_DATA_AGGREGATE& data) {
  header_.content_header.header_size =
      sizeof(XContentHeader) + sizeof(XContentMetadata);
  header_.content_header.magic = XContentPackageType::kCon;

  header_.content_metadata.content_type = data.content_type;
  header_.content_metadata.data_file_count = 1;
  header_.content_metadata.metadata_version = 2;  // ?
  header_.content_metadata.volume_type = XContentVolumeType::kStfs;

  header_.content_metadata.execution_info.title_id = data.title_id;
  header_.content_metadata.set_display_name(XLanguage::kEnglish,
                                            data.display_name());
  // Custom field in header. Used only in Xenia.
  header_.set_file_name(data.file_name());
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
