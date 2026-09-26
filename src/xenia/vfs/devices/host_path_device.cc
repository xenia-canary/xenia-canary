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
  PopulateEntry(root_entry);

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

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  std::vector<std::filesystem::path> ancestors;
  PopulateEntry(parent_entry, ancestors);
}

void HostPathDevice::PopulateEntry(
    HostPathEntry* parent_entry,
    std::vector<std::filesystem::path>& ancestors) {
  // Directory symlinks are walked now that the type comes from the stat, so a
  // link back into a directory already on this path would recurse until the
  // stack runs out. A link to a sibling subtree still gets walked: it
  // duplicates entries but ends, so only genuine self-containment is cut.
  std::error_code ec;
  auto canonical_self =
      std::filesystem::canonical(parent_entry->host_path(), ec);
  if (!ec) {
    for (const auto& seen : ancestors) {
      if (seen == canonical_self) {
        XELOGW(
            "HostPathDevice: {} is already on the path being walked; not "
            "following it again",
            xe::path_to_utf8(parent_entry->host_path()));
        return;
      }
    }
    ancestors.push_back(canonical_self);
  }

  auto child_infos = xe::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    auto child = HostPathEntry::Create(
        this, parent_entry, parent_entry->host_path() / child_info.name,
        child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == xe::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child, ancestors);
    }
  }

  if (!ec) {
    ancestors.pop_back();
  }
}

}  // namespace vfs
}  // namespace xe
