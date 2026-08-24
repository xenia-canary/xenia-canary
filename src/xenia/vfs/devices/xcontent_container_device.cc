/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/base/logging.h"

namespace xe {
namespace vfs {

XContentContainerDevice::XContentContainerDevice(
    const std::string_view mount_path, const size_t data_file_count)
    : Device(mount_path),
      name_("XContent"),
      data_file_count_(data_file_count),
      files_total_size_(0) {}

XContentContainerDevice::~XContentContainerDevice() {}

bool XContentContainerDevice::Initialize() {
  if (LoadHostFiles(data_file_count_) != Result::kSuccess) {
    XELOGE("Error loading XContent host files.");
    return false;
  }

  if (Read() != Result::kSuccess) {
    return false;
  }

  return true;
}

Entry* XContentContainerDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("StfsContainerDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

void XContentContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

}  // namespace vfs
}  // namespace xe
