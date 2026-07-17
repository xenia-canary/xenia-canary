/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_H_
#define XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_H_

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_key.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/xam/xcontent/xcontent.h"
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

class SpaInfo;

enum class ContentPackageType { Package = 0, Directory = 1 };

class ContentPackage {
 public:
  ContentPackage(vfs::VirtualFileSystem* file_system,
                 const std::string_view device_path,
                 const std::filesystem::path& package_path);
  ContentPackage(vfs::VirtualFileSystem* file_system,
                 const std::string_view device_path,
                 const std::filesystem::path& package_path,
                 const XCONTENT_DATA_INTERNAL& metadata,
                 const xex2_opt_execution_info* execution_info,
                 const SpaInfo* spa_info);

  virtual ~ContentPackage();

  // Allows to read package content. Creates dedicated device.
  virtual std::unique_ptr<vfs::Device> MountPackage() = 0;
  bool MountPackageAndRegister(const std::string_view root_name);

  virtual X_RESULT SetThumbnail(std::span<const uint8_t> thumbnail) = 0;
  virtual X_RESULT GetThumbnail(std::vector<uint8_t>& buffer) = 0;

  virtual size_t GetPackageSize() const = 0;
  virtual ContentPackageType GetPackageType() const = 0;

  void SetDevice(vfs::Device* device) { device_ = device; }

  const std::string_view GetDevicePath() const { return device_path_; }
  vfs::Device* GetDevice() const { return device_; }

  XCONTENT_DATA_INTERNAL GetContentMetadata() const;
  const XContentContainerHeader* const GetContainerHeader() const {
    return &header_;
  }
  const XContentMetadata* const GetContainerMetadata() const {
    return &header_.content_metadata;
  }
  uint32_t GetPackageLicense() const { return license_mask_; }
  bool IsValidPackage() const;

  virtual bool IsReadOnly() const = 0;

  std::filesystem::path GetPackageHostPath() const { return host_path_; }

  bool RegisterRootName(string_key_insensitive root_name);
  std::optional<string_key_insensitive> GetRootName() const {
    return root_name_;
  }

 protected:
  vfs::VirtualFileSystem* file_system_;
  // NtPath for device. Example: \\DEVICE\\<Something>
  std::string device_path_;
  // Path to host location where package is.
  std::filesystem::path host_path_;
  // Stores for example C: That allows quick access to device without usage of
  // device_path_.
  std::optional<string_key_insensitive> root_name_;

  XContentContainerHeader header_;
  // Device assigned to this package via MountPackage. Package might exist
  // without device to only read metadata.
  vfs::Device* device_;
  // Separate entry for license for backward compatibility
  uint32_t license_mask_ = 0;

 private:
  void InitializePackageHeader(const XCONTENT_DATA_INTERNAL& metadata,
                               const xex2_opt_execution_info* execution_info,
                               const SpaInfo* spa_info);
  void ComputePackageLicenseMask();
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_H_
