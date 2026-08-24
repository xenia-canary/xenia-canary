/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xcontent/xcontent_package.h"
#include "xenia/kernel/xam/xcontent/xcontent_package_container.h"
#include "xenia/kernel/xam/xcontent/xcontent_package_directory.h"

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

DECLARE_int32(license_mask);

namespace xe {
namespace kernel {
namespace xam {

static std::string_view kSpaFilename = "spa.bin";

ContentManager::ContentManager(KernelState* kernel_state,
                               const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {}

ContentManager::~ContentManager() = default;

std::string ContentManager::GeneratePackageDevicePath(
    [[maybe_unused]] const std::filesystem::path& root_path) {
  return fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);
}

X_STATUS ContentManager::ExtractContentHeader(
    const std::filesystem::path& header_path,
    const XContentContainerHeader& container_header) {
  if (!std::filesystem::exists(header_path.parent_path())) {
    if (!std::filesystem::create_directories(header_path.parent_path())) {
      return X_STATUS_ACCESS_DENIED;
    }
  }

  xe::filesystem::CreateEmptyFile(header_path);
  if (!std::filesystem::exists(header_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }

  std::filesystem::resize_file(
      header_path, xe::round_up(sizeof(XContentContainerHeader), 0x1000));

  auto file = xe::filesystem::OpenFile(header_path, "r+b");
  if (!file) {
    return X_STATUS_UNSUCCESSFUL;
  }

  fwrite(&container_header, 1, sizeof(XContentContainerHeader), file);
  fclose(file);
  return X_STATUS_SUCCESS;
}

std::unique_ptr<ContentPackage> ContentManager::OpenPackage(
    const std::filesystem::path host_path) {
  if (std::filesystem::is_directory(host_path)) {
    return std::make_unique<ContentPackageDirectory>(
        kernel_state_->file_system(), GeneratePackageDevicePath(host_path),
        host_path);
  } else {
    auto package = std::make_unique<ContentPackageContainer>(
        kernel_state_->file_system(), GeneratePackageDevicePath(host_path),
        host_path);

    return package->IsValidPackage() ? std::move(package) : nullptr;
  }
}
std::unique_ptr<ContentPackage> ContentManager::OpenPackage(
    const uint64_t xuid, const XCONTENT_DATA_INTERNAL& data) {
  auto host_path = ResolvePackagePath(xuid, data);

  if (!std::filesystem::exists(host_path)) {
    return nullptr;
  }

  return OpenPackage(host_path);
}

std::unique_ptr<ContentPackage> ContentManager::CreatePackage(
    const std::filesystem::path host_path, const XCONTENT_DATA_INTERNAL& data) {
#if 0
  auto package = std::make_unique<ContentPackageContainer>(
      kernel_state_->file_system(), root_name, host_path, data);

  return package->GetContainerHeader() ? std::move(package) : nullptr;
#else
  return std::make_unique<ContentPackageDirectory>(
      kernel_state_->file_system(), GeneratePackageDevicePath(host_path),
      host_path, data, kernel_state_->GetExecutionInfo(),
      kernel_state_->xam_state()->spa_info());
#endif
}

ContentPackage* ContentManager::OpenAndMountPackage(
    const std::filesystem::path host_path, const std::string_view root_name) {
  auto global_lock = global_critical_region_.Acquire();
  return MountPackage(root_name, OpenPackage(host_path));
}

ContentPackage* ContentManager::CreateAndMountPackage(
    const std::filesystem::path host_path, const std::string_view root_name,
    const XCONTENT_DATA_INTERNAL& data) {
  auto global_lock = global_critical_region_.Acquire();

  return MountPackage(root_name, CreatePackage(host_path, data));
}

ContentPackage* ContentManager::FindPackage(
    const XCONTENT_DATA_INTERNAL& data) {
  auto global_lock = global_critical_region_.Acquire();

  auto it = std::ranges::find_if(
      std::as_const(mounted_packages_), [&data](const auto& e) {
        return e.second->GetContentMetadata() == data;
      });

  if (it == mounted_packages_.end()) {
    return nullptr;
  }
  return it->second.get();
}

ContentPackage* ContentManager::MountPackage(
    const std::string_view root_name, std::unique_ptr<ContentPackage> package) {
  if (!package || !package->MountPackageAndRegister(root_name)) {
    return nullptr;
  }

  auto [it, inserted] =
      mounted_packages_.emplace(package->GetDevicePath(), std::move(package));
  if (!inserted) {
    return nullptr;
  }

  return it->second.get();
}

std::unique_ptr<vfs::Device> ContentManager::MountPackageUnregistered(
    const std::string_view root_name, std::unique_ptr<ContentPackage> package) {
  if (!package) {
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();
  // No matter if package is returned in unregistered state it MUST be tracked.
  auto [it, inserted] =
      mounted_packages_.emplace(package->GetDevicePath(), std::move(package));
  if (!inserted) {
    return nullptr;
  }

  it->second->RegisterRootName(xe::string_key_insensitive::create(root_name));

  return it->second->MountPackage();
}

std::filesystem::path ContentManager::ResolvePackageRoot(
    uint64_t xuid, uint32_t title_id, XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str =
      fmt::format("{:08X}", static_cast<uint32_t>(content_type));

  // Package root path:
  // content_root/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(
    const uint64_t xuid, const XCONTENT_DATA_INTERNAL& data) {
  // Content path:
  // content_root/title_id/content_type/data_file_name/
  auto get_package_path = [&, data](const uint32_t title_id) {
    uint64_t used_xuid =
        (data.xuid != -1 && data.xuid != 0) ? data.xuid.get() : xuid;

    // All DLCs are stored in common directory, so we need to override xuid for
    // them and probably some other types.
    if (data.content_type == XContentType::kMarketplaceContent) {
      used_xuid = 0;
    }

    auto package_root =
        ResolvePackageRoot(used_xuid, title_id, data.content_type);
    std::string final_name = xe::string_util::trim(data.file_name());
    return package_root / xe::to_path(final_name);
  };

  if (data.content_type == XContentType::kPublisher) {
    const std::unordered_set<uint32_t> title_ids =
        FindPublisherTitleIds(xuid, data.title_id);

    for (const auto& title_id : title_ids) {
      auto package_path = get_package_path(title_id);

      if (!std::filesystem::exists(package_path)) {
        continue;
      }
      return package_path;
    }
  }

  // Default handling for current title
  return get_package_path(data.title_id);
}

std::unordered_set<uint32_t> ContentManager::FindPublisherTitleIds(
    const uint64_t xuid, uint32_t base_title_id) const {
  if (base_title_id == kCurrentlyRunningTitleId) {
    base_title_id = kernel_state_->title_id();
  }
  std::unordered_set<uint32_t> title_ids = {};

  std::string publisher_id_regex =
      fmt::format("^{:04X}.*", static_cast<uint16_t>(base_title_id >> 16));
  // Get all publisher entries
  auto publisher_entries = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(root_path_ /
                                      fmt::format("{:016X}", xuid)),
      std::regex(publisher_id_regex));

  for (const auto& entry : publisher_entries) {
    std::filesystem::path path_to_publisher_dir =
        entry.path / entry.name /
        fmt::format("{:08X}", static_cast<uint32_t>(XContentType::kPublisher));

    if (!std::filesystem::exists(path_to_publisher_dir)) {
      continue;
    }

    title_ids.insert(xe::string_util::from_string<uint32_t>(
        xe::path_to_utf8(entry.name), true));
  }

  // Always remove current title. It will be handled differently
  if (title_ids.count(base_title_id)) {
    title_ids.erase(base_title_id);
  }
  return title_ids;
}

std::vector<XCONTENT_DATA_INTERNAL> ContentManager::ListContent(
    const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type, const XContentFlag content_flags) {
  std::vector<XCONTENT_DATA_INTERNAL> result;

  std::unordered_set<uint32_t> title_ids = {title_id};
  std::set<uint64_t> xuids = {xuid};
  std::set<XContentType> content_types = {content_type};

  if (content_type == XContentType::kPublisher) {
    title_ids = FindPublisherTitleIds(xuid, title_id);
  }

  if (content_type == XContentType::kProfile) {
    const auto xuid_dir = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(root_path_),
        std::regex("[A-Fa-f0-9]{16}"));

    for (const auto& xuid : xuid_dir) {
      auto package_root =
          ResolvePackageRoot(xe::string_util::from_string<uint64_t>(
                                 xe::path_to_utf8(xuid.name), true),
                             kDashboardID, content_type);
      auto file_infos = xe::filesystem::ListFiles(package_root);

      for (const auto& file_info : file_infos) {
        if (std::unique_ptr<ContentPackage> package =
                OpenPackage(file_info.path / file_info.name)) {
          result.emplace_back(package->GetContentMetadata());
        }
      }
    }
    return result;
  }

  if (content_flags & XContentFlag::kEnumerateAllProfiles ||
      content_type == XContentType::kAll) {
    const auto xuid_dir = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(root_path_),
        std::regex("[A-Fa-f0-9]{16}"));

    for (const auto& xuid : xuid_dir) {
      xuids.emplace(xe::string_util::from_string<uint64_t>(
          xe::path_to_utf8(xuid.name), true));

      const auto titles = xe::filesystem::FilterByName(
          xe::filesystem::ListDirectories(xuid.path),
          std::regex("[A-Fa-f0-9]{8}"));

      for (const auto& title : titles) {
        title_ids.emplace(xe::string_util::from_string<uint32_t>(
            xe::path_to_utf8(title.name), true));
      }
    }

    // Used in 7xxx and later dashboards
    if (content_type == XContentType::kAll) {
      content_types.clear();
      content_types.insert(
          {XContentType::kSavedGame, XContentType::kMarketplaceContent,
           XContentType::kPublisher, XContentType::kInstalledGame,
           XContentType::kXbox360Title, XContentType::kProfile,
           XContentType::kGamerPicture, XContentType::kTheme,
           XContentType::kGameDemo, XContentType::kInstaller,
           XContentType::kArcadeTitle, XContentType::kGameTrailer});
    }
  }

  // Search for all titles. Used in 1888 dashboard
  if (title_id == kDashboardID && xuid == 0) {
    const auto common_titles = xe::filesystem::FilterByName(
        xe::filesystem::ListDirectories(root_path_ / kCommonContentDirectory),
        std::regex("[A-Fa-f0-9]{8}"));

    for (const auto& title : common_titles) {
      title_ids.emplace(xe::string_util::from_string<uint32_t>(
          xe::path_to_utf8(title.name), true));
    }
  }

  for (const uint64_t xuid : xuids) {
    for (const uint32_t& title_id : title_ids) {
      for (const XContentType content_type : content_types) {
        // Search path:
        // content_root/xuid/title_id/type_name/*
        auto package_root = ResolvePackageRoot(xuid, title_id, content_type);
        auto file_infos = xe::filesystem::ListFiles(package_root);

        for (const auto& file_info : file_infos) {
          if (std::unique_ptr<ContentPackage> package =
                  OpenPackage(file_info.path / file_info.name)) {
            if (!package->IsValidPackage()) {
              continue;
            }
            result.emplace_back(package->GetContentMetadata());
          }
        }
      }
    }
  }
  return result;
}

std::vector<XCONTENT_DATA_INTERNAL> ContentManager::ListContentODD(
    const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
    const XContentType content_type) const {
  std::vector<XCONTENT_DATA_INTERNAL> result;

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str =
      fmt::format("{:08X}", static_cast<uint32_t>(content_type));

  const std::filesystem::path game_content_path =
      std::filesystem::path(kDefaultGameSymbolicLink) / "content" / xuid_str /
      title_id_str / content_type_str;

  auto entry = kernel_state_->file_system()->ResolvePath(
      xe::path_to_utf8(game_content_path));

  if (!entry) {
    return {};
  }

  for (const auto& child : entry->children()) {
    XCONTENT_DATA_AGGREGATE content_data;

    content_data.device_id = device_id;
    content_data.content_type = content_type;
    content_data.set_display_name(xe::path_to_utf16(child->name()));
    content_data.set_file_name(xe::path_to_utf8(child->name()));
    content_data.title_id = title_id;
    content_data.xuid = xuid;
    result.emplace_back(std::move(content_data));
  }

  return result;
}

bool ContentManager::ContentExists(const uint64_t xuid,
                                   const XCONTENT_DATA_INTERNAL& data) {
  auto path = ResolvePackagePath(xuid, data);
  return std::filesystem::exists(path);
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name,
                                       const uint64_t xuid,
                                       const XCONTENT_DATA_INTERNAL& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(root_name)) {
    // Already content open with this root name.
    return X_ERROR_ALREADY_EXISTS;
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    // Exists, must not!
    return X_ERROR_ALREADY_EXISTS;
  }

  // Ensure that parent path exists
  std::filesystem::create_directories(package_path.parent_path());

  if (!CreateAndMountPackage(package_path, root_name, data)) {
    return X_ERROR_ACCESS_DENIED;
  }

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name,
                                     const uint64_t xuid,
                                     const XCONTENT_DATA_INTERNAL& data,
                                     uint32_t& content_license) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(root_name)) {
    // Already content open with this root name.
    return X_ERROR_ALREADY_EXISTS;
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    // Does not exist, must be created.
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Open package.
  auto package = OpenAndMountPackage(package_path, root_name);
  if (!package) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  content_license = package->GetContentMetadata().license_mask;
  if (static_cast<uint32_t>(cvars::license_mask) > 1) {
    content_license |= cvars::license_mask;
  }

  // Check for SPA file in package. Check it only for DLCs
  if (data.content_type == XContentType::kMarketplaceContent) {
    std::string spa_path = fmt::format("{}:\\{}", root_name, kSpaFilename);
    auto spa_update = kernel_state_->file_system()->ResolvePath(spa_path);
    if (spa_update) {
      UpdateSpaData(spa_update);
    }
  }

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  auto global_lock = global_critical_region_.Acquire();

  // 415607D6 - Uses XamContentCreate with name "save", but XamContentClose with
  // "SAVE".
  auto itr = std::ranges::find_if(
      std::as_const(mounted_packages_), [root_name](const auto& content) {
        if (const auto name = content.second->GetRootName(); name) {
          return name.value() == string_key_insensitive::create(root_name);
        }
        return false;
      });

  if (itr == mounted_packages_.cend()) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  CloseOpenedFilesFromContent(root_name);

  mounted_packages_.erase(itr);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContentByDeviceName(
    const std::string_view device_name) {
  auto global_lock = global_critical_region_.Acquire();

  auto it = std::ranges::find_if(
      std::as_const(mounted_packages_), [device_name](const auto& entry) {
        return entry.second->GetDevicePath() == device_name;
      });

  if (it == mounted_packages_.end()) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  mounted_packages_.erase(it);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(const uint64_t xuid,
                                             const XCONTENT_DATA_INTERNAL& data,
                                             std::vector<uint8_t>& buffer) {
  auto global_lock = global_critical_region_.Acquire();
  // Find pacakge based on provided data.
  if (auto itr = std::ranges::find_if(
          as_const(mounted_packages_),
          [data](const auto& content) {
            return data == content.second->GetContentMetadata();
          });
      itr != mounted_packages_.cend()) {
    return itr->second->GetThumbnail(buffer);
  }

  // Package is not mounted. Fallback to opening package.
  auto package = OpenPackage(xuid, data);
  if (!package) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  return package->GetThumbnail(buffer);
}

X_RESULT ContentManager::SetContentThumbnail(const uint64_t xuid,
                                             const XCONTENT_DATA_INTERNAL& data,
                                             std::span<const uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  if (auto itr = std::ranges::find_if(
          as_const(mounted_packages_),
          [data](const auto& content) {
            return data == content.second->GetContentMetadata();
          });
      itr != mounted_packages_.cend()) {
    return itr->second->SetThumbnail(buffer);
  }

  // Package is not mounted. Fallback to opening package.
  auto package = OpenPackage(xuid, data);
  if (!package) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  return package->SetThumbnail(buffer);
}

X_RESULT ContentManager::DeleteContent(const uint64_t xuid,
                                       const XCONTENT_DATA_INTERNAL& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(data)) {
    // TODO(Gliniak): Get real error code for this case.
    return X_ERROR_ACCESS_DENIED;
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::remove_all(package_path) > 0) {
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

bool ContentManager::IsContentOpen(const XCONTENT_DATA_INTERNAL& data) const {
  auto global_lock = global_critical_region_.Acquire();
  return std::ranges::any_of(mounted_packages_, [&data](const auto& content) {
    return data == content.second->GetContentMetadata();
  });
}

bool ContentManager::IsContentOpen(const std::string_view root_name) const {
  auto global_lock = global_critical_region_.Acquire();
  const auto key = string_key_insensitive::create(root_name);
  return std::ranges::any_of(mounted_packages_, [&key](const auto& content) {
    return content.second->GetRootName() == key;
  });
}

void ContentManager::CloseOpenedFilesFromContent(
    const std::string_view root_name) {
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(
          XObject::Type::File);

  std::string resolved_path = "";
  if (!kernel_state_->file_system()->FindSymbolicLink(
          std::string(root_name) + ':', resolved_path)) {
    // There is no symbolic link, so no files to close.
    return;
  }

  for (const object_ref<XFile>& file : all_files_handles) {
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content = utf8::starts_with(file_path, resolved_path);

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }
}

uint64_t ContentManager::GetContentTotalSpace() const {
  std::error_code ec;
  const auto drive_stats = std::filesystem::space(root_path_, ec);
  if (ec) {
    XELOGW("{}: {} (:08X)", __func__, ec.message(), ec.value());
    return 0;
  }

  return drive_stats.capacity;
}

uint64_t ContentManager::GetContentFreeSpace() const {
  std::error_code ec;
  const auto drive_stats = std::filesystem::space(root_path_, ec);
  if (ec) {
    XELOGW("{}: {} (:08X)", __func__, ec.message(), ec.value());
    return 0;
  }

  return drive_stats.free;
}

bool ContentManager::UpdateSpaData(vfs::Entry* spa_file_update) {
  vfs::File* file;
  if (spa_file_update->Open(vfs::FileAccess::kFileReadData, &file) !=
      X_STATUS_SUCCESS) {
    return false;
  }

  std::vector<uint8_t> data(spa_file_update->size());

  size_t read_bytes = 0;
  if (file->ReadSync(std::span<uint8_t>(data.data(), spa_file_update->size()),
                     0, &read_bytes) != X_STATUS_SUCCESS) {
    return false;
  }

  xam::SpaInfo new_spa_data(std::span<uint8_t>(data.data(), data.size()));
  kernel_state_->xam_state()->LoadSpaInfo(&new_spa_data);
  kernel_state_->emulator()->game_info_database()->Update(&new_spa_data);
  return true;
}

X_STATUS ContentManager::InstallContentPackage(
    const ContentPackage* package,
    const std::filesystem::path& installation_path,
    const std::filesystem::path& header_path, uint64_t& installation_progress,
    bool force_extract) {
  // Remove this path when STFS Writer is available.
  if (!package->IsReadOnly() || force_extract) {
    // Check if file with the same name exists. Directory and File cannot
    // coexist.
    if (const auto info = xe::filesystem::GetInfo(installation_path); info) {
      if (info->type != xe::filesystem::FileInfo::Type::kDirectory) {
        return X_ERROR_ALREADY_EXISTS;
      }
    }

    const std::error_code ec = xe::filesystem::CreateFolder(installation_path);
    if (ec) {
      return X_STATUS_UNSUCCESSFUL;
    }
    // No need to create header if we just want to extract some data outside of
    // content.
    if (!force_extract) {
      ExtractContentHeader(header_path, *package->GetContainerHeader());
    }
    return vfs::VirtualFileSystem::ExtractDeviceFiles(
        package->GetDevice(), installation_path, installation_progress);
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  // Copying package. For STFS it's easy as there is only one file to copy.
  // For SVOD we need to copy main header file + .data directory with files
  // inside.
  switch (package->GetContainerMetadata()->volume_type) {
    case XContentVolumeType::kStfs:
      if (xe::filesystem::Copy(package->GetPackageHostPath(), installation_path,
                               installation_progress)) {
        result = X_STATUS_SUCCESS;
      }
      break;
    case XContentVolumeType::kSvod: {
      const auto host_path = package->GetPackageHostPath();

      // Copy main file
      if (xe::filesystem::Copy(host_path, installation_path,
                               installation_progress)) {
        result = X_STATUS_SUCCESS;
      }

      // Copy .data directory with files.
      std::filesystem::path dest_data_fragment_path = installation_path;
      dest_data_fragment_path += ".data";

      xe::filesystem::CreateFolder(dest_data_fragment_path);

      std::filesystem::path src_package_path = host_path;
      src_package_path += ".data";

      auto fragment_files = filesystem::ListFiles(src_package_path);

      for (const auto& fragment : fragment_files) {
        const auto path = fragment.path / fragment.name;
        xe::filesystem::Copy(path, dest_data_fragment_path / fragment.name,
                             installation_progress);
      }

    } break;
    default:
      XELOGE("{}: Unsupported package type: {}", __func__,
             static_cast<uint32_t>(
                 package->GetContainerMetadata()->volume_type.get()));
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
