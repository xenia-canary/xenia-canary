/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_CONTAINER_H_
#define XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_CONTAINER_H_

#include <memory>
#include <string>
#include <unordered_map>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/string_key.h"
#include "xenia/base/string_util.h"
#include "xenia/xbox.h"

#include "xenia/kernel/xam/xcontent/xcontent.h"
#include "xenia/kernel/xam/xcontent/xcontent_package.h"

namespace xe {
namespace kernel {
namespace xam {

class ContentPackageContainer final : public ContentPackage {
 public:
  // Ctor for reading already exisitng packages.
  ContentPackageContainer(vfs::VirtualFileSystem* file_system,
                          const std::string_view device_path,
                          const std::filesystem::path& package_path);
  // Ctor for creating new packages.
  ContentPackageContainer(vfs::VirtualFileSystem* file_system,
                          const std::string_view device_path,
                          const std::filesystem::path& package_path,
                          const XCONTENT_DATA_INTERNAL& metadata,
                          const xex2_opt_execution_info* execution_info,
                          const SpaInfo* spa_info);

  ~ContentPackageContainer();

  bool IsValidPackage() const {
    return header_.content_header.is_magic_valid();
  }

  std::unique_ptr<vfs::Device> MountPackage() override;

  X_RESULT SetThumbnail(std::span<const uint8_t> thumbnail) override;
  X_RESULT GetThumbnail(std::vector<uint8_t>& buffer) override;

  ContentPackageType GetPackageType() const override {
    return ContentPackageType::Package;
  }

  bool IsReadOnly() const override { return header_.is_package_readonly(); }
  size_t GetPackageSize() const override;

 private:
  bool LoadPackage(const std::filesystem::path& package_path,
                   MappedMemory::Mode open_mode = MappedMemory::Mode::kRead);
  bool CreatePackage(const std::filesystem::path& package_path,
                     const XCONTENT_DATA_INTERNAL& metadata);
  bool LoadAndVerifyHeader(const std::filesystem::path& package_path);

  std::unique_ptr<MappedMemory> package_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_CONTAINER_H_
