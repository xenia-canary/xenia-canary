/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_DRIVE_ENTRY_H_
#define XENIA_VFS_DEVICES_DISC_DRIVE_ENTRY_H_

#include <string>
#include <string_view>
#include <vector>

#include "xenia/vfs/entry.h"
#include "xenia/vfs/physical_device.h"

namespace xe {
namespace vfs {

class Device;

class DiscDriveEntry : public Entry {
 public:
  DiscDriveEntry(Device* device, Entry* parent, std::string_view path,
                 size_t allocation_base, size_t allocation_size);
  ~DiscDriveEntry() override;

  static std::unique_ptr<DiscDriveEntry> Create(Device* device, Entry* parent,
                                                std::string_view name,
                                                size_t allocation_base,
                                                size_t data_size);

  size_t data_offset() const { return data_offset_; }
  size_t data_size() const { return data_size_; }

  void SetMetadata(uint32_t attributes, size_t size, size_t allocation_size,
                   uint64_t create_timestamp, uint64_t access_timestamp,
                   uint64_t write_timestamp) {
    attributes_ = attributes;
    size_ = size;
    allocation_size_ = allocation_size;
    create_timestamp_ = create_timestamp;
    access_timestamp_ = access_timestamp;
    write_timestamp_ = write_timestamp;
  }

  void AddChild(std::unique_ptr<DiscDriveEntry> child) {
    children_.push_back(std::move(child));
  }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

  bool can_map() const override { return false; }
  std::unique_ptr<MappedMemory> OpenMapped(MappedMemory::Mode mode,
                                           size_t offset,
                                           size_t length) override {
    return nullptr;
  }

  PhysicalDevice* physical_device() const {
    return static_cast<PhysicalDevice*>(device_);
  }

 private:
  friend class PhysicalDevice;

  bool DeleteEntryInternal(Entry* entry) override;

  size_t data_offset_;
  size_t data_size_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_DRIVE_ENTRY_H_
