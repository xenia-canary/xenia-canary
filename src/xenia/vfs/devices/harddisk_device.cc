/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/harddisk_device.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/devices/null_entry.h"

namespace xe {
namespace vfs {

namespace {
// The retail drive layout: the cache partitions are 2 GB each, after a
// 512 KB system area.
constexpr uint64_t kCache0Offset = 0x80000;
constexpr uint64_t kCacheLength = 0x80000000;
constexpr uint64_t kCache1Offset = kCache0Offset + kCacheLength;

constexpr uint32_t kFatxMagic = 0x58544146;  // "XTAF"
// XapiFormatFATVolumeEx aligns the superblock and the FAT to this.
constexpr uint64_t kFatxAlignment = 0x1000;
}  // namespace

RawPartitionEntry::RawPartitionEntry(Device* device, Entry* parent,
                                     const std::string& path,
                                     std::filesystem::path image_path,
                                     uint64_t offset, uint64_t length)
    : Entry(device, parent, path),
      image_path_(std::move(image_path)),
      offset_(offset),
      length_(length) {
  attributes_ = kFileAttributeNormal;
  size_ = static_cast<size_t>(length);
  allocation_size_ = static_cast<size_t>(length);
}

X_STATUS RawPartitionEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new RawPartitionFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

X_STATUS RawPartitionEntry::Read(std::span<uint8_t> buffer, uint64_t offset,
                                 size_t* out_bytes_read) {
  if (offset >= length_) {
    return X_STATUS_END_OF_FILE;
  }
  size_t count = static_cast<size_t>(
      std::min<uint64_t>(buffer.size(), length_ - offset));
  std::fill_n(buffer.begin(), count, uint8_t(0));

  std::lock_guard<std::mutex> lock(lock_);
  std::ifstream image(image_path_, std::ios::binary);
  if (image) {
    image.seekg(0, std::ios::end);
    uint64_t image_size = static_cast<uint64_t>(image.tellg());
    if (offset < image_size) {
      size_t stored = static_cast<size_t>(
          std::min<uint64_t>(count, image_size - offset));
      image.seekg(static_cast<std::streamoff>(offset));
      image.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(stored));
    }
  }
  if (out_bytes_read) {
    *out_bytes_read = count;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS RawPartitionEntry::Write(std::span<const uint8_t> buffer,
                                  uint64_t offset, size_t* out_bytes_written) {
  if (offset >= length_) {
    return X_STATUS_END_OF_FILE;
  }
  size_t count = static_cast<size_t>(
      std::min<uint64_t>(buffer.size(), length_ - offset));
  {
    std::lock_guard<std::mutex> lock(lock_);
    std::error_code ec;
    std::filesystem::create_directories(image_path_.parent_path(), ec);
    if (!std::filesystem::exists(image_path_)) {
      std::ofstream create(image_path_, std::ios::binary);
    }
    std::fstream image(image_path_,
                       std::ios::binary | std::ios::in | std::ios::out);
    if (!image) {
      XELOGE("RawPartitionEntry: cannot open {}", xe::path_to_utf8(image_path_));
      return X_STATUS_UNSUCCESSFUL;
    }
    image.seekp(static_cast<std::streamoff>(offset));
    image.write(reinterpret_cast<const char*>(buffer.data()),
                static_cast<std::streamsize>(count));
    if (!image) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  if (out_bytes_written) {
    *out_bytes_written = count;
  }
  // XapiFormatFATVolumeEx writes the superblock last, at the start of the
  // partition: the volume on it is now a new, empty one.
  if (volume_ && offset == 0 && count >= 4 &&
      xe::load_and_swap<uint32_t>(buffer.data()) == kFatxMagic) {
    volume_->OnFormatted();
  }
  return X_STATUS_SUCCESS;
}

X_STATUS RawPartitionFile::ReadSync(std::span<uint8_t> buffer,
                                    size_t byte_offset,
                                    size_t* out_bytes_read) {
  if (!(file_access_ & FileAccess::kFileReadData)) {
    return X_STATUS_ACCESS_DENIED;
  }
  return static_cast<RawPartitionEntry*>(entry_)->Read(buffer, byte_offset,
                                                       out_bytes_read);
}

X_STATUS RawPartitionFile::WriteSync(std::span<const uint8_t> buffer,
                                     size_t byte_offset,
                                     size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }
  return static_cast<RawPartitionEntry*>(entry_)->Write(buffer, byte_offset,
                                                        out_bytes_written);
}

CacheVolumeDevice::CacheVolumeDevice(const std::string_view mount_path,
                                     const std::filesystem::path& host_path,
                                     RawPartitionEntry* raw)
    : HostPathDevice(mount_path, host_path, false), raw_(raw) {}

CacheVolumeDevice::Superblock CacheVolumeDevice::ReadSuperblock() const {
  Superblock sb;
  uint8_t data[12] = {};
  size_t read = 0;
  if (XSUCCEEDED(raw_->Read(data, 0, &read)) &&
      xe::load_and_swap<uint32_t>(data) == kFatxMagic) {
    sb.serial = xe::load_and_swap<uint32_t>(data + 4);
    sb.sectors_per_cluster = xe::load_and_swap<uint32_t>(data + 8);
    sb.valid = sb.sectors_per_cluster != 0;
  }
  return sb;
}

uint64_t CacheVolumeDevice::cluster_size() const {
  auto sb = ReadSuperblock();
  return sb.valid ? uint64_t(sb.sectors_per_cluster) * bytes_per_sector() : 0;
}

uint32_t CacheVolumeDevice::sectors_per_allocation_unit() const {
  auto sb = ReadSuperblock();
  return sb.valid ? sb.sectors_per_cluster : 0;
}

uint32_t CacheVolumeDevice::volume_serial_number() const {
  return ReadSuperblock().serial;
}

uint32_t CacheVolumeDevice::total_allocation_units() const {
  // The data area XapiFormatFATVolumeEx leaves: the partition less the
  // superblock and the FAT (16-bit entries below 65520 clusters, else 32).
  uint64_t cluster = cluster_size();
  uint64_t length = raw_->partition_length();
  if (!cluster || length <= kFatxAlignment) {
    return 0;
  }
  uint64_t entries = length / cluster + 1;
  uint64_t fat = entries * (entries < 65520 ? 2 : 4);
  fat = (fat + kFatxAlignment - 1) & ~(kFatxAlignment - 1);
  if (length < kFatxAlignment + fat) {
    return 0;
  }
  return static_cast<uint32_t>((length - kFatxAlignment - fat) / cluster);
}

uint32_t CacheVolumeDevice::available_allocation_units() const {
  uint64_t cluster = cluster_size();
  uint32_t total = total_allocation_units();
  if (!cluster || !total) {
    return 0;
  }
  // The root directory takes a cluster; so does every other directory, and
  // a file takes the clusters its data fills.
  uint64_t used = 1;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(host_path(), ec);
       !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_directory(ec)) {
      used += 1;
    } else {
      used += (it->file_size(ec) + cluster - 1) / cluster;
    }
  }
  return used >= total ? 0 : static_cast<uint32_t>(total - used);
}

void CacheVolumeDevice::OnFormatted() {
  XELOGI("{}: formatted, emptying {}", mount_path(),
         xe::path_to_utf8(host_path()));
  std::error_code ec;
  for (auto& child : std::filesystem::directory_iterator(host_path(), ec)) {
    std::filesystem::remove_all(child.path(), ec);
  }
  Initialize();
}

HarddiskDevice::HarddiskDevice(const std::string_view mount_path,
                               const std::filesystem::path& image_root,
                               const std::filesystem::path& cache_root)
    : Device(mount_path), image_root_(image_root), cache_root_(cache_root) {}

HarddiskDevice::~HarddiskDevice() = default;

bool HarddiskDevice::Initialize() {
  root_entry_ = std::unique_ptr<Entry>(NullEntry::Create(this, nullptr, ""));

  struct Layout {
    const char* name;
    uint64_t offset;
    uint64_t length;
    const char* cache_dir;
  };
  const Layout layout[] = {
      {"Partition0", 0, kDiskSectors * 0x200, nullptr},
      {"Cache0", kCache0Offset, kCacheLength, "cache0"},
      {"Cache1", kCache1Offset, kCacheLength, "cache1"},
  };
  for (const auto& l : layout) {
    Partition p;
    p.name = l.name;
    p.raw = std::make_unique<RawPartitionEntry>(
        this, root_entry_.get(), std::string("\\") + l.name,
        image_root_ / (std::string(l.name) + ".img"), l.offset, l.length);
    if (l.cache_dir) {
      p.volume = std::make_unique<CacheVolumeDevice>(
          xe::utf8::join_guest_paths(mount_path_, l.name),
          cache_root_ / l.cache_dir, p.raw.get());
      if (!p.volume->Initialize()) {
        return false;
      }
      p.raw->set_volume(p.volume.get());
    }
    partitions_.push_back(std::move(p));
  }
  return true;
}

void HarddiskDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

HarddiskDevice::Partition* HarddiskDevice::FindPartition(
    const std::string_view name) {
  for (auto& p : partitions_) {
    if (xe::utf8::equal_case(p.name, name)) {
      return &p;
    }
  }
  return nullptr;
}

// path is relative to the mount path: "", "\Cache0", "\Cache0\dir\file".
static std::pair<std::string_view, std::string_view> SplitPartitionPath(
    std::string_view path) {
  while (!path.empty() && path.front() == '\\') {
    path.remove_prefix(1);
  }
  auto separator = path.find('\\');
  if (separator == std::string_view::npos) {
    return {path, std::string_view()};
  }
  return {path.substr(0, separator), path.substr(separator + 1)};
}

Entry* HarddiskDevice::ResolvePath(const std::string_view path) {
  auto [name, rest] = SplitPartitionPath(path);
  if (name.empty()) {
    return root_entry_.get();
  }
  auto partition = FindPartition(name);
  if (!partition) {
    return nullptr;
  }
  if (!partition->volume) {
    return rest.empty() ? partition->raw.get() : nullptr;
  }
  // As on a console, a partition with no FATX volume on it has no files to
  // open; the caller formats it.
  if (!partition->volume->sectors_per_allocation_unit()) {
    return nullptr;
  }
  return partition->volume->ResolvePath(rest);
}

Entry* HarddiskDevice::ResolveRawPartition(const std::string_view path) {
  auto [name, rest] = SplitPartitionPath(path);
  if (name.empty() || !rest.empty()) {
    return nullptr;
  }
  auto partition = FindPartition(name);
  return partition ? partition->raw.get() : nullptr;
}

}  // namespace vfs
}  // namespace xe
