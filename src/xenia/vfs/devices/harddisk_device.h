/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_HARDDISK_DEVICE_H_
#define XENIA_VFS_DEVICES_HARDDISK_DEVICE_H_

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/file.h"

namespace xe {
namespace vfs {

class CacheVolumeDevice;

// A partition opened by its device name, without a trailing separator
// ("\Device\Harddisk0\Cache0"): its sectors. They are kept in a host image
// file; anything never written reads as zeros.
class RawPartitionEntry : public Entry {
 public:
  RawPartitionEntry(Device* device, Entry* parent, const std::string& path,
                    std::filesystem::path image_path, uint64_t offset,
                    uint64_t length);

  X_STATUS Open(uint32_t desired_access, File** out_file) override;
  bool can_map() const override { return false; }

  uint64_t partition_offset() const { return offset_; }
  uint64_t partition_length() const { return length_; }

  X_STATUS Read(std::span<uint8_t> buffer, uint64_t offset,
                size_t* out_bytes_read);
  X_STATUS Write(std::span<const uint8_t> buffer, uint64_t offset,
                 size_t* out_bytes_written);

  // The cache volume formatted onto this partition, told when a FATX
  // superblock is written.
  void set_volume(CacheVolumeDevice* volume) { volume_ = volume; }

 private:
  bool DeleteEntryInternal(Entry* entry) override { return false; }

  std::mutex lock_;
  std::filesystem::path image_path_;
  uint64_t offset_;
  uint64_t length_;
  CacheVolumeDevice* volume_ = nullptr;
};

class RawPartitionFile : public File {
 public:
  RawPartitionFile(uint32_t file_access, RawPartitionEntry* entry)
      : File(file_access, entry) {}

  void Destroy() override { delete this; }
  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override { return X_STATUS_SUCCESS; }
};

// The FATX volume of a cache partition ("\Device\Harddisk0\Cache0\..."): its
// files live in a host directory, and its geometry comes from the superblock
// XapiFormatFATVolumeEx wrote to the raw partition. A new superblock is a
// format, which empties the volume.
class CacheVolumeDevice : public HostPathDevice {
 public:
  CacheVolumeDevice(const std::string_view mount_path,
                    const std::filesystem::path& host_path,
                    RawPartitionEntry* raw);

  const std::string& name() const override { return name_; }
  uint32_t component_name_max_length() const override { return 42; }

  uint32_t total_allocation_units() const override;
  uint32_t available_allocation_units() const override;
  uint32_t sectors_per_allocation_unit() const override;
  uint32_t bytes_per_sector() const override { return 0x200; }
  uint32_t volume_serial_number() const override;

  void OnFormatted();

 private:
  struct Superblock {
    bool valid = false;
    uint32_t serial = 0;
    uint32_t sectors_per_cluster = 0;
  };
  Superblock ReadSuperblock() const;
  uint64_t cluster_size() const;

  std::string name_ = "FATX";
  RawPartitionEntry* raw_;
};

// "\Device\Harddisk0": the raw partitions a title can open by name
// (Partition0, the whole disk; Cache0 and Cache1, the utility-drive cache
// partitions) and the FATX volumes on the cache partitions. Names are
// matched without regard to case, as the console's object manager does.
class HarddiskDevice : public Device {
 public:
  HarddiskDevice(const std::string_view mount_path,
                 const std::filesystem::path& image_root,
                 const std::filesystem::path& cache_root);
  ~HarddiskDevice() override;

  bool Initialize() override;
  void Dump(StringBuffer* string_buffer) override;
  // A cache partition by name resolves to the root of its volume; see
  // ResolveRawPartition for the partition itself.
  Entry* ResolvePath(const std::string_view path) override;
  Entry* ResolveRawPartition(const std::string_view path);

  bool is_read_only() const override { return false; }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }
  uint32_t device_type() const override { return 7; }  // FILE_DEVICE_DISK

  // As the null device this replaces reported for raw partitions: STFC code in
  // titles requires the product of the two below to be 0x10000.
  uint32_t total_allocation_units() const override { return 0x10; }
  uint32_t available_allocation_units() const override { return 0x10; }
  uint32_t sectors_per_allocation_unit() const override { return 0x80; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  // The disk the partitions are on: a 120 GB retail drive.
  static constexpr uint64_t kDiskSectors = 0x0DF94BB0;

 private:
  struct Partition {
    std::string name;
    std::unique_ptr<RawPartitionEntry> raw;
    std::unique_ptr<CacheVolumeDevice> volume;
  };
  Partition* FindPartition(const std::string_view name);

  std::string name_ = "Harddisk";
  std::filesystem::path image_root_;
  std::filesystem::path cache_root_;
  std::unique_ptr<Entry> root_entry_;
  std::vector<Partition> partitions_;
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_HARDDISK_DEVICE_H_
