/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_DRIVE_FILE_H_
#define XENIA_VFS_DEVICES_DISC_DRIVE_FILE_H_

#include <span>

#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class DiscDriveEntry;

class DiscDriveFile : public File {
 public:
  DiscDriveFile(uint32_t file_access, DiscDriveEntry* entry);
  ~DiscDriveFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override {
    return X_STATUS_ACCESS_DENIED;
  }

  //TODO: Async Reads/Writes.

  X_STATUS SetLength(size_t length) override { return X_STATUS_ACCESS_DENIED; }

 private:
  DiscDriveEntry* entry_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_DRIVE_FILE_H_
