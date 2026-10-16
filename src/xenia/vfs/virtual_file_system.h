/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
#define XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class VirtualFileSystem {
 public:
  VirtualFileSystem();
  ~VirtualFileSystem();

  void Clear();

  bool RegisterDevice(std::unique_ptr<Device> device);
  bool UnregisterDevice(const std::string_view path);

  // A fallback link stands in until something registers the same name: the
  // emulator's own drive mappings, which a title's ObCreateSymbolicLink (as
  // XMountUtilityDrive makes on a console) replaces rather than collides
  // with.
  bool RegisterSymbolicLink(const std::string_view path,
                            const std::string_view target,
                            bool fallback = false);
  bool UnregisterSymbolicLink(const std::string_view path);
  bool IsSymbolicLinkRegistered(const std::string_view path);
  bool FindSymbolicLink(const std::string_view path, std::string& target);

  Entry* ResolvePath(const std::string_view path);
  // The status a console's open of a name that does not resolve returns:
  // STATUS_OBJECT_PATH_NOT_FOUND when a directory on the way is missing,
  // otherwise STATUS_OBJECT_NAME_NOT_FOUND (the file, or the drive itself).
  X_STATUS NotFoundStatus(Entry* root_entry, const std::string_view path);
  // A disk partition named directly ("\Device\Harddisk0\Cache0"): the raw
  // partition, or with want_volume the root of the volume on it. nullptr for
  // any other name.
  Entry* ResolvePartition(const std::string_view path, bool want_volume);

  Entry* CreatePath(const std::string_view path, uint32_t attributes);
  bool DeletePath(const std::string_view path);

  X_STATUS OpenFile(Entry* root_entry, const std::string_view path,
                    FileDisposition creation_disposition,
                    uint32_t desired_access, bool is_directory,
                    bool is_non_directory, File** out_file,
                    FileAction* out_action);

  static X_STATUS ExtractContentFile(Entry* entry,
                                     std::filesystem::path base_path,
                                     uint64_t& progress,
                                     bool extract_to_root = false);
  static X_STATUS ExtractContentFiles(Device* device,
                                      std::filesystem::path base_path,
                                      uint64_t& progress);
  static void ExtractContentHeader(Device* device,
                                   std::filesystem::path base_path);

 private:
  xe::global_critical_region global_critical_region_;
  std::vector<std::unique_ptr<Device>> devices_;
  std::unordered_map<std::string, std::string> symlinks_;
  std::vector<std::string> fallback_symlinks_;

  bool ResolveSymbolicLink(const std::string_view path, std::string& result);
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_VIRTUAL_FILE_SYSTEM_H_
