/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_XCONTENT_CONTAINER_DEVICE_H_
#define XENIA_VFS_DEVICES_XCONTENT_CONTAINER_DEVICE_H_

#include <filesystem>
#include <map>
#include <string_view>

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/stfs_xbox.h"

namespace xe {
namespace vfs {

constexpr fourcc_t kLIVESignature = make_fourcc("LIVE");
constexpr fourcc_t kCONSignature = make_fourcc("CON ");
constexpr fourcc_t kPIRSSignature = make_fourcc("PIRS");

class XContentContainerDevice : public Device {
 public:
  constexpr static uint32_t kBlockSize = 0x1000;

  ~XContentContainerDevice() override;

  bool Initialize() override;

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }

  uint32_t sectors_per_allocation_unit() const override { return 8; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 protected:
  XContentContainerDevice(const std::string_view mount_path,
                          const size_t data_file_count = 1);

  enum class Result {
    kSuccess = 0,
    kOutOfMemory = -1,
    kReadError = -10,
    kFileMismatch = -30,
    kDamagedFile = -31,
    kTooSmall = -32,
  };

  virtual Result Read() = 0;
  // Load all host files. Usually STFS is only 1 file, meanwhile SVOD is usually
  // multiple file.
  virtual Result LoadHostFiles(const size_t data_file_count) = 0;

  Entry* ResolvePath(const std::string_view path) override;
  void Dump(StringBuffer* string_buffer) override;

  void SetName(std::string name) { name_ = name; }
  const std::string& GetName() const { return name_; }

  void SetFilesSize(uint64_t files_size) { files_total_size_ = files_size; }
  const uint64_t GetFilesSize() const { return files_total_size_; }

  const std::filesystem::path& GetHostPath() const { return host_path_; }

  std::string name_;
  std::filesystem::path host_path_;

  const size_t data_file_count_;
  size_t files_total_size_;
  std::unique_ptr<Entry> root_entry_;
};

}  // namespace vfs
}  // namespace xe

#endif
