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
#include "xenia/kernel/xam/xcontent/xcontent_package_container.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xobject.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/stfs_container_device.h"
#include "xenia/vfs/devices/xcontent_devices/svod_container_device.h"

namespace xe {
namespace kernel {
namespace xam {

ContentPackageContainer::ContentPackageContainer(
    vfs::VirtualFileSystem* file_system, const std::string_view device_path,
    const std::filesystem::path& package_path)
    : ContentPackage(file_system, device_path, package_path) {
  LoadAndVerifyHeader(package_path);
}

ContentPackageContainer::ContentPackageContainer(
    vfs::VirtualFileSystem* file_system, const std::string_view device_path,
    const std::filesystem::path& package_path,
    const XCONTENT_DATA_INTERNAL& metadata,
    const xex2_opt_execution_info* execution_info, const SpaInfo* spa_info)
    : ContentPackage(file_system, device_path, package_path, metadata,
                     execution_info, spa_info) {
  // No verification because there is nothing to be verified.
  CreatePackage(package_path, metadata);
}

ContentPackageContainer::~ContentPackageContainer() {
  // Unmount device first as it stores slice of memory mapped file.
  // TODO(Gliniak): Mark device as closed, so there is no way to write to it. At
  // this point device should be flushed completely and no further read/write be
  // possible to it.
  // Header data is still stored in parent class. In parent class we should
  // write header back to the file which is already closed with new data, but
  // old header.
  if (root_name_) {
    std::string root_name = std::string(root_name_.value().view());
    if (!root_name.ends_with(':')) {
      root_name.append(":");
    }
    file_system_->UnregisterSymbolicLink(root_name);
  }

  if (device_) {
    file_system_->UnregisterDevice(device_path_);
  }

  device_ = nullptr;
  root_name_ = std::nullopt;
}

bool ContentPackageContainer::LoadPackage(
    const std::filesystem::path& package_path, MappedMemory::Mode open_mode) {
  if (std::filesystem::file_size(package_path) <
      sizeof(XContentHeader) + sizeof(XContentMetadata)) {
    return false;
  }

  package_ = xe::MappedMemory::Open(package_path, open_mode);

  if (!package_) {
    return false;
  }

  const auto package_header =
      reinterpret_cast<XContentContainerHeader*>(package_->data());
  if (!package_header->content_header.is_magic_valid()) {
    package_->Close();
    return false;
  }

  header_ = *package_header;

  if (!header_.content_header.is_magic_valid()) {
    package_->Close();
    return false;
  }

  return true;
}

bool ContentPackageContainer::CreatePackage(
    const std::filesystem::path& package_path,
    const XCONTENT_DATA_INTERNAL& metadata) {
  // Create empty file
  if (!xe::filesystem::CreateEmptyFile(package_path)) {
    return false;
  }

  package_ =
      xe::MappedMemory::Open(package_path, MappedMemory::Mode::kReadWrite, 0,
                             sizeof(XContentHeader) + sizeof(XContentMetadata));
  if (!package_) {
    return false;
  }

  header_ = *reinterpret_cast<XContentContainerHeader*>(package_->data());

  // Todo(Gliniak): header write

  // No flush there. Flush should be only done on explicit flush request or
  // package close.

  return true;
}

bool ContentPackageContainer::LoadAndVerifyHeader(
    const std::filesystem::path& package_path) {
  if (!LoadPackage(package_path)) {
    return false;
  }

  // We need to remap data again in case header tells us to open package in
  // different mode.
  if (!header_.is_package_readonly()) {
    package_->Close();

    if (!LoadPackage(package_path, MappedMemory::Mode::kReadWrite)) {
      return false;
    }
  }

  return true;
}

std::unique_ptr<vfs::Device> ContentPackageContainer::MountPackage() {
  if (!package_ || !header_.content_header.is_magic_valid()) {
    return nullptr;
  }

  const size_t package_data_offset =
      xe::round_up(header_.content_header.header_size,
                   vfs::XContentContainerDevice::kBlockSize);

  std::unique_ptr<vfs::Device> device = nullptr;
  switch (header_.content_metadata.volume_type) {
    case XContentVolumeType::kStfs:
      device = std::make_unique<vfs::StfsContainerDevice>(
          device_path_, &header_.content_metadata.volume_descriptor.stfs,
          package_->Slice(package_data_offset,
                          package_->size() - package_data_offset));
      break;
    case XContentVolumeType::kSvod:
      device = std::make_unique<vfs::SvodContainerDevice>(
          device_path_, host_path_, header_.content_metadata.data_file_count,
          &header_.content_metadata.volume_descriptor.svod);
      break;
    default:
      break;
  }

  return device;
}

X_RESULT ContentPackageContainer::SetThumbnail(
    std::span<const uint8_t> thumbnail) {
  if (header_.is_package_readonly()) {
    return X_ERROR_NOT_SUPPORTED;
  }

  const uint32_t max_thumbnail_size =
      header_.content_metadata.metadata_version == 1
          ? header_.content_metadata.kThumbLengthV1
          : header_.content_metadata.kThumbLengthV2;

  if (thumbnail.size() > max_thumbnail_size) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  header_.content_metadata.thumbnail_size = thumbnail.size();
  memcpy(header_.content_metadata.thumbnail, thumbnail.data(),
         thumbnail.size());
  return X_ERROR_SUCCESS;
}

X_RESULT ContentPackageContainer::GetThumbnail(std::vector<uint8_t>& buffer) {
  if (!header_.content_metadata.thumbnail_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  const uint32_t max_thumbnail_size =
      header_.content_metadata.metadata_version == 1
          ? header_.content_metadata.kThumbLengthV1
          : header_.content_metadata.kThumbLengthV2;
  const uint32_t size = std::min<uint32_t>(
      header_.content_metadata.thumbnail_size, max_thumbnail_size);

  buffer.assign(header_.content_metadata.thumbnail,
                header_.content_metadata.thumbnail + size);
  return X_ERROR_SUCCESS;
}

size_t ContentPackageContainer::GetPackageSize() const {
  if (header_.content_metadata.content_size > 0) {
    return header_.content_metadata.content_size;
  }

  // Search for .data directory.
  if (header_.content_metadata.volume_type == XContentVolumeType::kSvod) {
    size_t size = std::filesystem::file_size(GetPackageHostPath());

    std::filesystem::path src_package_path = GetPackageHostPath();
    src_package_path += ".data";

    const auto files = xe::filesystem::ListFiles(src_package_path);
    for (const auto& file : files) {
      size += std::filesystem::file_size(file.path / file.name);
    }
    return size;
  }

  return std::filesystem::file_size(GetPackageHostPath());
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
