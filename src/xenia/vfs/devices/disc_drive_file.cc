/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_drive_file.h"

#include <algorithm>

#include "xenia/vfs/devices/disc_drive_entry.h"

namespace xe {
namespace vfs {

DiscDriveFile::DiscDriveFile(uint32_t file_access, DiscDriveEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscDriveFile::~DiscDriveFile() = default;

void DiscDriveFile::Destroy() { delete this; }

X_STATUS DiscDriveFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  if (byte_offset >= entry_->data_size()) {
    return X_STATUS_END_OF_FILE;
  }

  const size_t read_length =
      std::min(buffer.size(), entry_->data_size() - byte_offset);
  if (read_length == 0) {
    if (out_bytes_read) {
      *out_bytes_read = 0;
    }
    return X_STATUS_SUCCESS;
  }

  auto* physical_device = entry_->physical_device();
  if (!physical_device) {
    return X_STATUS_UNSUCCESSFUL;
  }

  constexpr size_t kPhysicalReadChunkSize = 512 * 1024;
  const size_t real_offset = entry_->data_offset() + byte_offset;
  size_t progressed = 0;
  while (progressed < read_length) {
    const size_t chunk_length =
        std::min(read_length - progressed, kPhysicalReadChunkSize);
    if (!physical_device->ReadFileBytes(real_offset + progressed,
                                        buffer.subspan(progressed,
                                                       chunk_length))) {
      return X_STATUS_UNSUCCESSFUL;
    }
    progressed += chunk_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = read_length;
  }
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
