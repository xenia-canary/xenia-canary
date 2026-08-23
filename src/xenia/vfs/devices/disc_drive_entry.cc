/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_drive_entry.h"

#include "xenia/vfs/devices/disc_drive_file.h"

namespace xe {
namespace vfs {

DiscDriveEntry::DiscDriveEntry(Device* device, Entry* parent,
                               std::string_view path, size_t allocation_base,
                               size_t allocation_size)
    : Entry(device, parent, path),
      data_offset_(allocation_base),
      data_size_(allocation_size) {}

DiscDriveEntry::~DiscDriveEntry() = default;

std::unique_ptr<DiscDriveEntry> DiscDriveEntry::Create(Device* device,
                                                       Entry* parent,
                                                       std::string_view name,
                                                       size_t allocation_base,
                                                       size_t data_size) {
  auto path = xe::utf8::join_guest_paths(parent->path(), name);
  return std::make_unique<DiscDriveEntry>(device, parent, path, allocation_base,
                                          data_size);
}

X_STATUS DiscDriveEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new DiscDriveFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

bool DiscDriveEntry::DeleteEntryInternal(Entry* entry) { return false; }
}  // namespace vfs
}  // namespace xe
