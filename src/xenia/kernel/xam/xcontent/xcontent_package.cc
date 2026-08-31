/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xcontent/xcontent_package.h"
#include "xenia/kernel/xam/content_manager.h"

#include <array>
#include <string>
#include <unordered_set>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xdbf/spa_info.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xobject.h"
#include "xenia/vfs/devices/host_path_device.h"

namespace xe {
namespace kernel {
namespace xam {

ContentPackage::ContentPackage(vfs::VirtualFileSystem* file_system,
                               const std::string_view device_path,
                               const std::filesystem::path& package_path)
    : file_system_(file_system),
      device_path_(device_path),
      host_path_(package_path),
      root_name_(std::nullopt),
      header_(),
      device_(nullptr),
      license_mask_(0) {}

ContentPackage::ContentPackage(vfs::VirtualFileSystem* file_system,
                               const std::string_view device_path,
                               const std::filesystem::path& package_path,
                               const XCONTENT_DATA_INTERNAL& metadata,
                               const xex2_opt_execution_info* execution_info,
                               const SpaInfo* spa_info)
    : file_system_(file_system),
      device_path_(device_path),
      host_path_(package_path),
      root_name_(std::nullopt),
      header_(),
      device_(nullptr),
      license_mask_(0) {
  InitializePackageHeader(metadata, execution_info, spa_info);
}

ContentPackage::~ContentPackage() {
  XELOGI("{}: Closing package: {} (Device: {}) Device mounted: {}", __func__,
         root_name_ ? root_name_.value().view() : "", device_path_,
         device_ ? "Yes" : "No");

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
}

bool ContentPackage::MountPackageAndRegister(const std::string_view root_name,
                                             const std::string_view root_path) {
  auto device = MountPackage();

  if (!device || !device->Initialize()) {
    return false;
  }

  if (!root_name.empty()) {
    std::string name = std::string(root_name);
    // We don't want to include : in root name
    if (!name.ends_with(":")) {
      name.append(":");
    }
    if (!file_system_->RegisterSymbolicLink(
            name, xe::utf8::join_guest_paths(GetDevicePath(), root_path))) {
      XELOGE(
          "{}: Cannot register package with mount path: {}. Mount path already "
          "in-use.",
          __func__, root_name);
      return false;
    }
    root_name_ = string_key_insensitive::create(root_name);
  }
  // Assign device to package. Device ownership is on VFS side, but package have
  // direct access to it.
  device_ = file_system_->RegisterDevice(std::move(device));
  return device_;
}

bool ContentPackage::RegisterRootName(string_key_insensitive root_name) {
  if (device_) {
    // Pacakge is already mounted, so it should have root_name provided.
    return false;
  }

  root_name_ = root_name;
  return true;
}

XCONTENT_DATA_INTERNAL ContentPackage::GetContentMetadata() const {
  XCONTENT_DATA_INTERNAL metadata{};

  metadata.device_id = 1;
  metadata.title_id = header_.content_metadata.execution_info.title_id;
  metadata.content_type = header_.content_metadata.content_type;
  metadata.content_size = header_.content_metadata.content_size;
  if (!metadata.content_size) {
    metadata.content_size = GetPackageSize();
  }

  auto name = header_.content_metadata.display_name(XLanguage::kEnglish);
  if (name.empty()) {
    // Find first filled language and use it. It might be incorrect, but meh
    // until stfs support is done.
    for (uint8_t i = 0; i < header_.content_metadata.kNumLanguagesV2; i++) {
      name = header_.content_metadata.display_name(static_cast<XLanguage>(i));
      if (!name.empty()) {
        break;
      }
    }
  }

  metadata.set_display_name(name);
  metadata.set_file_name(xe::path_to_utf8(host_path_.filename()));

  if (metadata.content_type == XContentType::kSavedGame) {
    const auto filename = header_.file_name();
    if (!filename.empty()) {
      metadata.set_file_name(filename);
    }
  }
  metadata.set_title_name(header_.content_metadata.title_name());

  metadata.license_mask = license_mask_;
  metadata.xuid = header_.content_metadata.profile_id;
  metadata.title_id = header_.content_metadata.execution_info.title_id;

  return metadata;
}

void ContentPackage::ComputePackageLicenseMask() {
  uint32_t final_license = 0;
  for (uint8_t i = 0; i < license_count; i++) {
    if (header_.content_header.licenses[i].license_flags) {
      final_license |= header_.content_header.licenses[i].license_bits;
    }
  }

  license_mask_ = final_license;
}

void ContentPackage::InitializePackageHeader(
    const XCONTENT_DATA_INTERNAL& metadata,
    const xex2_opt_execution_info* execution_info, const SpaInfo* spa_info) {
  header_.content_header.header_size =
      sizeof(XContentHeader) + sizeof(XContentMetadata);
  header_.content_header.magic = XContentPackageType::kCon;

  header_.content_metadata.content_type = metadata.content_type;
  header_.content_metadata.data_file_count = 1;
  header_.content_metadata.metadata_version = 2;  // ?
  header_.content_metadata.volume_type = XContentVolumeType::kStfs;
  if (execution_info) {
    header_.content_metadata.execution_info = *execution_info;
  }
  header_.content_metadata.profile_id = metadata.xuid;

  // Specific entries
  header_.content_metadata.execution_info.title_id = metadata.title_id;

  // Icon
  if (spa_info) {
    const auto icon = spa_info->title_icon();
    if (!icon.empty()) {
      header_.content_metadata.title_thumbnail_size = icon.size();
      memcpy(header_.content_metadata.title_thumbnail, icon.data(),
             std::min(icon.size(),
                      xe::countof(header_.content_metadata.title_thumbnail)));

      header_.content_metadata.thumbnail_size = icon.size();
      memcpy(header_.content_metadata.thumbnail, icon.data(),
             std::min(icon.size(),
                      xe::countof(header_.content_metadata.thumbnail)));
    }

    header_.content_metadata.set_display_name(spa_info->default_language(),
                                              metadata.display_name());
    header_.content_metadata.set_title_name(
        xe::to_utf16(spa_info->title_name()));
  } else {
    header_.content_metadata.set_display_name(XLanguage::kEnglish,
                                              metadata.display_name());
  }

  if (metadata.content_type == XContentType::kSavedGame) {
    header_.set_file_name(metadata.file_name());
  }
}

bool ContentPackage::IsValidPackage() const {
  if (!header_.content_header.is_magic_valid()) {
    return false;
  }

  if (header_.content_metadata.content_type == XContentType::kInvalid) {
    return false;
  }

  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
