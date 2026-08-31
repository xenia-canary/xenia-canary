/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_CONTENT_MANAGER_H_
#define XENIA_KERNEL_XAM_CONTENT_MANAGER_H_

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_key.h"
#include "xenia/base/string_util.h"
#include "xenia/vfs/entry.h"
#include "xenia/xbox.h"

#include "xenia/kernel/xam/xcontent/xcontent.h"
#include "xenia/kernel/xam/xcontent/xcontent_package.h"

#include "xenia/vfs/device.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

static std::string_view kCommonContentDirectory = "0000000000000000";

class ContentManager {
 public:
  ContentManager(KernelState* kernel_state,
                 const std::filesystem::path& root_path);
  ~ContentManager();

  static X_STATUS ExtractContentHeader(
      const std::filesystem::path& header_path,
      const XContentContainerHeader& container_header);

  // Host specific methods
  std::unique_ptr<ContentPackage> OpenPackage(
      const std::filesystem::path host_path);
  std::unique_ptr<ContentPackage> OpenPackage(
      const uint64_t xuid, const XCONTENT_DATA_INTERNAL& data);
  std::unique_ptr<ContentPackage> CreatePackage(
      const std::filesystem::path host_path,
      const XCONTENT_DATA_INTERNAL& data);

  ContentPackage* OpenAndMountPackage(const std::filesystem::path host_path,
                                      const std::string_view root_name,
                                      const std::string_view root_path = "");
  ContentPackage* CreateAndMountPackage(const std::filesystem::path host_path,
                                        const std::string_view root_name,
                                        const XCONTENT_DATA_INTERNAL& data);
  ContentPackage* FindPackage(const XCONTENT_DATA_INTERNAL& data);

  // Used only in case you don't want device to be autoregistered in VFS.
  // Example: Device is initialized externally, not via ContentManager.
  std::unique_ptr<vfs::Device> MountPackageUnregistered(
      const std::string_view root_name,
      std::unique_ptr<ContentPackage> package);

  // Common methods
  std::vector<XCONTENT_DATA_INTERNAL> ListContent(
      const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
      const XContentType content_type, const XContentFlag content_flags);

  std::vector<XCONTENT_DATA_INTERNAL> ListContentODD(
      const uint32_t device_id, const uint64_t xuid, const uint32_t title_id,
      const XContentType content_type) const;

  bool ContentExists(const uint64_t xuid, const XCONTENT_DATA_INTERNAL& data);

  X_RESULT CreateContent(const std::string_view root_name, const uint64_t xuid,
                         const XCONTENT_DATA_INTERNAL& data);
  X_RESULT OpenContent(const std::string_view root_name, const uint64_t xuid,
                       const XCONTENT_DATA_INTERNAL& data,
                       uint32_t& content_license,
                       const uint32_t disc_number = -1);
  X_RESULT CloseContent(const std::string_view root_name);
  X_RESULT CloseContentByDeviceName(const std::string_view device_name);
  X_RESULT GetContentThumbnail(const uint64_t xuid,
                               const XCONTENT_DATA_INTERNAL& data,
                               std::vector<uint8_t>& buffer);
  X_RESULT SetContentThumbnail(const uint64_t xuid,
                               const XCONTENT_DATA_INTERNAL& data,
                               std::span<const uint8_t> buffer);
  X_RESULT DeleteContent(const uint64_t xuid,
                         const XCONTENT_DATA_INTERNAL& data);
  bool IsContentOpen(const XCONTENT_DATA_INTERNAL& data) const;
  bool IsContentOpen(const std::string_view root_name) const;

  void CloseOpenedFilesFromContent(const std::string_view root_name);

  uint64_t GetContentTotalSpace() const;
  uint64_t GetContentFreeSpace() const;

  X_STATUS InstallContentPackage(const ContentPackage* package,
                                 const std::filesystem::path& installation_path,
                                 const std::filesystem::path& header_path,
                                 uint64_t& installation_progress,
                                 bool force_extract = false);

 private:
  std::string GeneratePackageDevicePath(const std::filesystem::path& root_path);
  // Used in case of mounting package on guest (via XAM). Package is
  // autoregistered.
  ContentPackage* MountPackage(const std::string_view root_name,
                               std::unique_ptr<ContentPackage> package,
                               const std::string_view root_path = "");

  std::filesystem::path ResolvePackageRoot(
      const uint64_t xuid, const uint32_t title_id,
      const XContentType content_type) const;
  std::filesystem::path ResolvePackagePath(const uint64_t xuid,
                                           const XCONTENT_DATA_INTERNAL& data);
  std::unordered_set<uint32_t> FindPublisherTitleIds(
      const uint64_t xuid,
      uint32_t base_title_id = kCurrentlyRunningTitleId) const;

  bool UpdateSpaData(vfs::Entry* spa_file_update);

  uint32_t content_device_id_ = 0;

  KernelState* kernel_state_;
  std::filesystem::path root_path_;

  // TODO(benvanik): remove use of global lock, it's bad here!
  xe::global_critical_region global_critical_region_;
  std::unordered_map<std::string, std::unique_ptr<ContentPackage>>
      mounted_packages_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_CONTENT_MANAGER_H_
