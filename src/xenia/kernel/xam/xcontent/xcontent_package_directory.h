/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_DIRECTORY_H_
#define XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_DIRECTORY_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

static std::string_view kGameContentHeaderDirName = "Headers";

class SpaInfo;

class ContentPackageDirectory final : public ContentPackage {
 public:
  ContentPackageDirectory(vfs::VirtualFileSystem* file_system,
                          const std::string_view device_path,
                          const std::filesystem::path& package_path);
  ContentPackageDirectory(vfs::VirtualFileSystem* file_system,
                          const std::string_view device_path,
                          const std::filesystem::path& package_path,
                          const XCONTENT_DATA_INTERNAL& metadata,
                          const xex2_opt_execution_info* execution_info,
                          const SpaInfo* spa_info);
  ~ContentPackageDirectory();

  std::unique_ptr<vfs::Device> MountPackage() override;

  X_RESULT SetThumbnail(std::span<const uint8_t> thumbnail) override;
  X_RESULT GetThumbnail(std::vector<uint8_t>& buffer) override;

  bool IsReadOnly() const override { return false; }

  ContentPackageType GetPackageType() const override {
    return ContentPackageType::Directory;
  }
  size_t GetPackageSize() const override;

 private:
  X_RESULT ReadContentHeaderFile();
  X_RESULT WriteContentHeaderFile() const;
  void FillHeader(const XCONTENT_DATA_AGGREGATE& data);

  std::filesystem::path header_path_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XCONTENT_XCONTENT_PACKAGE_DIRECTORY_H_
