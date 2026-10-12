/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_device.h"

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path,
                               bool read_only)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      XELOGE("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  return true;
}

void HostPathDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("HostPathDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

bool HostPathDevice::AddDirectory(const std::string_view name,
                                  const std::filesystem::path& host_path) {
  auto global_lock = global_critical_region_.Acquire();
  if (!root_entry_ || read_only_ || name.empty() || name == "." ||
      name == ".." || name.find_first_of("/\\") != name.npos ||
      root_entry_->Entry::GetChild(name)) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(host_path, ec);
  if (ec) {
    return false;
  }
  auto info = xe::filesystem::GetInfo(host_path);
  if (!info || info->type != xe::filesystem::FileInfo::Type::kDirectory) {
    return false;
  }
  info->name = xe::to_path(name);
  auto root = static_cast<HostPathEntry*>(root_entry_.get());
  root->children_.emplace_back(
      HostPathEntry::Create(this, root, host_path, *info));
  return true;
}

}  // namespace vfs
}  // namespace xe
