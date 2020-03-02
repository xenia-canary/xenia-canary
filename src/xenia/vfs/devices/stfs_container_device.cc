/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/stfs_container_device.h"

#include <algorithm>
#include <queue>
#include <vector>

#include "third_party/crypto/TinySHA1.hpp"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/vfs/devices/stfs_container_entry.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#define timegm _mkgmtime
#endif

namespace xe {
namespace vfs {

uint32_t load_uint24_be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 16) |
         (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[2]);
}
uint32_t load_uint24_le(const uint8_t* p) {
  return (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
}

// Convert FAT timestamp to 100-nanosecond intervals since January 1, 1601 (UTC)
uint64_t decode_fat_timestamp(uint32_t date, uint32_t time) {
  struct tm tm = {0};
  // 80 is the difference between 1980 (FAT) and 1900 (tm);
  tm.tm_year = ((0xFE00 & date) >> 9) + 80;
  tm.tm_mon = (0x01E0 & date) >> 5;
  tm.tm_mday = (0x001F & date) >> 0;
  tm.tm_hour = (0xF800 & time) >> 11;
  tm.tm_min = (0x07E0 & time) >> 5;
  tm.tm_sec = (0x001F & time) << 1;  // the value stored in 2-seconds intervals
  tm.tm_isdst = 0;
  time_t timet = timegm(&tm);
  if (timet == -1) {
    return 0;
  }
  // 11644473600LL is a difference between 1970 and 1601
  return (timet + 11644473600LL) * 10000000;
}

StfsContainerDevice::StfsContainerDevice(const std::string& mount_path,
                                         const std::wstring& local_path)
    : Device(mount_path), local_path_(local_path) {}

StfsContainerDevice::~StfsContainerDevice() = default;

bool StfsContainerDevice::Initialize() {
  // Resolve a valid STFS file if a directory is given.
  if (filesystem::IsFolder(local_path_) && !ResolveFromFolder(local_path_)) {
    XELOGE("Could not resolve an STFS container given path %ls",
           local_path_.c_str());
    return false;
  }

  if (!filesystem::PathExists(local_path_)) {
    XELOGE("Path to STFS container does not exist: %ls", local_path_.c_str());
    return false;
  }

  // Map the data file(s)
  auto map_result = MapFiles();
  if (map_result != Error::kSuccess) {
    XELOGE("Failed to map STFS container: %d", map_result);
    return false;
  }

  switch (header_.metadata.volume_type) {
    case XContentVolumeType::kStfs:
      return ReadSTFS() == Error::kSuccess;
      break;
    case XContentVolumeType::kSvod:
      return ReadSVOD() == Error::kSuccess;
    default:
      XELOGE("Unknown XContent volume type");
      return false;
  }
}

StfsContainerDevice::Error StfsContainerDevice::MapFiles() {
  // Map the file containing the STFS Header and read it.
  XELOGI("Mapping STFS Header file: %ls", local_path_.c_str());
  auto header_map = MappedMemory::Open(local_path_, MappedMemory::Mode::kRead);
  if (!header_map) {
    XELOGE("Error mapping STFS Header file.");
    return Error::kErrorReadError;
  }

  auto header_result =
      ReadHeaderAndVerify(header_map->data(), header_map->size());
  if (header_result != Error::kSuccess) {
    XELOGE("Error reading STFS Header: %d", header_result);
    return header_result;
  }

  mmap_total_size_ += header_map->size();

  // If the STFS package is a single file, the header is self contained and
  // we don't need to map any extra files.
  // NOTE: data_file_count is 0 for STFS and 1 for SVOD
  if (header_.metadata.data_file_count <= 1) {
    XELOGI("STFS container is a single file.");
    mmap_.emplace(std::make_pair(0, std::move(header_map)));
    return Error::kSuccess;
  }

  // If the STFS package is multi-file, it is an SVOD system. We need to map
  // the files in the .data folder and can discard the header.
  auto data_fragment_path = local_path_ + L".data";
  if (!filesystem::PathExists(data_fragment_path)) {
    XELOGE("STFS container is multi-file, but path %ls does not exist.",
           xe::to_string(data_fragment_path).c_str());
    return Error::kErrorFileMismatch;
  }

  // Ensure data fragment files are sorted
  auto fragment_files = filesystem::ListFiles(data_fragment_path);
  std::sort(fragment_files.begin(), fragment_files.end(),
            [](filesystem::FileInfo& left, filesystem::FileInfo& right) {
              return left.name < right.name;
            });

  if (fragment_files.size() != header_.metadata.data_file_count) {
    XELOGE("SVOD expecting %d data fragments, but %d are present.",
           static_cast<uint32_t>(header_.metadata.data_file_count),
           fragment_files.size());
    return Error::kErrorFileMismatch;
  }

  for (size_t i = 0; i < fragment_files.size(); i++) {
    auto file = fragment_files.at(i);
    auto path = xe::join_paths(file.path, file.name);
    auto data = MappedMemory::Open(path, MappedMemory::Mode::kRead);
    if (!data) {
      XELOGI("Failed to map SVOD file %ls.", path.c_str());
      mmap_.clear();
      mmap_total_size_ = 0;
      return Error::kErrorReadError;
    }
    mmap_total_size_ += data->size();
    mmap_.emplace(std::make_pair(i, std::move(data)));
  }
  XELOGI("SVOD successfully mapped %d files.", fragment_files.size());
  return Error::kSuccess;
}

void StfsContainerDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* StfsContainerDevice::ResolvePath(const std::string& path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo

  XELOGFS("StfsContainerDevice::ResolvePath(%s)", path.c_str());

  // Walk the path, one separator at a time.
  auto entry = root_entry_.get();
  auto path_parts = xe::split_path(path);
  for (auto& part : path_parts) {
    entry = entry->GetChild(part);
    if (!entry) {
      // Not found.
      return nullptr;
    }
  }

  return entry;
}

StfsContainerDevice::Error StfsContainerDevice::ReadHeaderAndVerify(
    const uint8_t* map_ptr, size_t map_size) {
  // Copy header & check signature
  memcpy(&header_, map_ptr, sizeof(StfsHeader));
  if (header_.header.magic != XContentPackageType::kPackageTypeCon &&
      header_.header.magic != XContentPackageType::kPackageTypeLive &&
      header_.header.magic != XContentPackageType::kPackageTypePirs) {
    // Unexpected format.
    return Error::kErrorFileMismatch;
  }

  // Pre-calculate some values used in block number calculations
  blocks_per_hash_table_ = 1;
  block_step_[0] = 0xAB;
  block_step_[1] = 0x718F;

  // TODO: it seems if header_.header_size > 0xA000, this should make sure never
  // to follow the branch below? Since the header size would spill over into the
  // first hash tables primary block (@0xA000), that must mean it only uses a
  // single block for each table, right?
  // Need to verify with kernel if it actually bases anything on the header_size
  // field.
  if (!header_.metadata.stfs_volume_descriptor.flags.read_only_format) {
    blocks_per_hash_table_ = 2;
    block_step_[0] = 0xAC;
    block_step_[1] = 0x723A;
  }

  return Error::kSuccess;
}

StfsContainerDevice::Error StfsContainerDevice::ReadSVOD() {
  // SVOD Systems can have different layouts. The root block is
  // denoted by the magic "MICROSOFT*XBOX*MEDIA" and is always in
  // the first "actual" data fragment of the system.
  auto data = mmap_.at(0)->data();
  const char* MEDIA_MAGIC = "MICROSOFT*XBOX*MEDIA";

  // Check for EDGF layout
  bool has_egdf_layout =
      header_.metadata.svod_volume_descriptor.features.enhanced_gdf_layout;

  if (has_egdf_layout) {
    // The STFS header has specified that this SVOD system uses the EGDF layout.
    // We can expect the magic block to be located immediately after the hash
    // blocks. We also offset block address calculation by 0x1000 by shifting
    // block indices by +0x2.
    if (memcmp(data + 0x2000, MEDIA_MAGIC, 20) == 0) {
      base_offset_ = 0x0000;
      magic_offset_ = 0x2000;
      svod_layout_ = SvodLayoutType::kEnhancedGDF;
      XELOGI("SVOD uses an EGDF layout. Magic block present at 0x2000.");
    } else {
      XELOGE("SVOD uses an EGDF layout, but the magic block was not found.");
      return Error::kErrorFileMismatch;
    }
  } else if (memcmp(data + 0x12000, MEDIA_MAGIC, 20) == 0) {
    // If the SVOD's magic block is at 0x12000, it is likely using an XSF
    // layout. This is usually due to converting the game using a third-party
    // tool, as most of them use a nulled XSF as a template.

    base_offset_ = 0x10000;
    magic_offset_ = 0x12000;

    // Check for XSF Header
    const char* XSF_MAGIC = "XSF";
    if (memcmp(data + 0x2000, XSF_MAGIC, 3) == 0) {
      svod_layout_ = SvodLayoutType::kXSF;
      XELOGI("SVOD uses an XSF layout. Magic block present at 0x12000.");
      XELOGI("Game was likely converted using a third-party tool.");
    } else {
      svod_layout_ = SvodLayoutType::kUnknown;
      XELOGI("SVOD appears to use an XSF layout, but no header is present.");
      XELOGI("SVOD magic block found at 0x12000");
    }
  } else if (memcmp(data + 0xD000, MEDIA_MAGIC, 20) == 0) {
    // If the SVOD's magic block is at 0xD000, it most likely means that it is
    // a single-file system. The STFS Header is 0xB000 bytes , and the remaining
    // 0x2000 is from hash tables. In most cases, these will be STFS, not SVOD.

    base_offset_ = 0xB000;
    magic_offset_ = 0xD000;

    // Check for single file system
    if (header_.metadata.data_file_count == 1) {
      svod_layout_ = SvodLayoutType::kSingleFile;
      XELOGI("SVOD is a single file. Magic block present at 0xD000.");
    } else {
      svod_layout_ = SvodLayoutType::kUnknown;
      XELOGE(
          "SVOD is not a single file, but the magic block was found at "
          "0xD000.");
    }
  } else {
    XELOGE("Could not locate SVOD magic block.");
    return Error::kErrorReadError;
  }

  // Parse the root directory
  uint8_t* magic_block = data + magic_offset_;
  uint32_t root_block = xe::load<uint32_t>(magic_block + 0x14);
  uint32_t root_size = xe::load<uint32_t>(magic_block + 0x18);
  uint32_t root_creation_date = xe::load<uint32_t>(magic_block + 0x1C);
  uint32_t root_creation_time = xe::load<uint32_t>(magic_block + 0x20);
  uint64_t root_creation_timestamp =
      decode_fat_timestamp(root_creation_date, root_creation_time);

  auto root_entry = new StfsContainerEntry(this, nullptr, "", &mmap_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry->access_timestamp_ = root_creation_timestamp;
  root_entry->create_timestamp_ = root_creation_timestamp;
  root_entry->write_timestamp_ = root_creation_timestamp;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  // Traverse all child entries
  return ReadEntrySVOD(root_block, 0, root_entry);
}

StfsContainerDevice::Error StfsContainerDevice::ReadEntrySVOD(
    uint32_t block, uint32_t ordinal, StfsContainerEntry* parent) {
  // For games with a large amount of files, the ordinal offset can overrun
  // the current block and potentially hit a hash block.
  size_t ordinal_offset = ordinal * 0x4;
  size_t block_offset = ordinal_offset / 0x800;
  size_t true_ordinal_offset = ordinal_offset % 0x800;

  // Calculate the file & address of the block
  size_t entry_address, entry_file;
  BlockToOffsetSVOD(block + block_offset, &entry_address, &entry_file);
  entry_address += true_ordinal_offset;

  // Read block's descriptor
  auto data = mmap_.at(entry_file)->data() + entry_address;

  uint16_t node_l = xe::load<uint16_t>(data + 0x00);
  uint16_t node_r = xe::load<uint16_t>(data + 0x02);
  uint32_t data_block = xe::load<uint32_t>(data + 0x04);
  uint32_t length = xe::load<uint32_t>(data + 0x08);
  uint8_t attributes = xe::load<uint8_t>(data + 0x0C);
  uint8_t name_length = xe::load<uint8_t>(data + 0x0D);
  auto name = reinterpret_cast<const char*>(data + 0x0E);
  auto name_str = std::string(name, name_length);

  // Read the left node
  if (node_l) {
    auto node_result = ReadEntrySVOD(block, node_l, parent);
    if (node_result != Error::kSuccess) {
      return node_result;
    }
  }

  // Read file & address of block's data
  size_t data_address, data_file;
  BlockToOffsetSVOD(data_block, &data_address, &data_file);

  // Create the entry
  // NOTE: SVOD entries don't have timestamps for individual files, which can
  //       cause issues when decrypting games. Using the root entry's timestamp
  //       solves this issues.
  auto entry = StfsContainerEntry::Create(this, parent, name_str, &mmap_);
  if (attributes & kFileAttributeDirectory) {
    // Entry is a directory
    entry->attributes_ = kFileAttributeDirectory | kFileAttributeReadOnly;
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    entry->block_ = block;
    entry->access_timestamp_ = root_entry_->create_timestamp();
    entry->create_timestamp_ = root_entry_->create_timestamp();
    entry->write_timestamp_ = root_entry_->create_timestamp();

    if (length) {
      // If length is greater than 0, traverse the directory's children
      auto directory_result = ReadEntrySVOD(data_block, 0, entry.get());
      if (directory_result != Error::kSuccess) {
        return directory_result;
      }
    }
  } else {
    // Entry is a file
    entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
    entry->size_ = length;
    entry->allocation_size_ = xe::round_up(length, bytes_per_sector());
    entry->data_offset_ = data_address;
    entry->data_size_ = length;
    entry->block_ = data_block;
    entry->access_timestamp_ = root_entry_->create_timestamp();
    entry->create_timestamp_ = root_entry_->create_timestamp();
    entry->write_timestamp_ = root_entry_->create_timestamp();

    // Fill in all block records, sector by sector.
    if (entry->attributes() & X_FILE_ATTRIBUTE_NORMAL) {
      uint32_t block_index = data_block;
      size_t remaining_size = xe::round_up(length, 0x800);

      size_t last_record = -1;
      size_t last_offset = -1;
      while (remaining_size) {
        const size_t BLOCK_SIZE = 0x800;

        size_t offset, file_index;
        BlockToOffsetSVOD(block_index, &offset, &file_index);

        block_index++;
        remaining_size -= BLOCK_SIZE;

        if (offset - last_offset == 0x800) {
          // Consecutive, so append to last entry.
          entry->block_list_[last_record].length += BLOCK_SIZE;
          last_offset = offset;
          continue;
        }

        entry->block_list_.push_back({file_index, offset, BLOCK_SIZE});
        last_record = entry->block_list_.size() - 1;
        last_offset = offset;
      }
    }
  }

  parent->children_.emplace_back(std::move(entry));

  // Read the right node.
  if (node_r) {
    auto node_result = ReadEntrySVOD(block, node_r, parent);
    if (node_result != Error::kSuccess) {
      return node_result;
    }
  }

  return Error::kSuccess;
}

void StfsContainerDevice::BlockToOffsetSVOD(size_t block, size_t* out_address,
                                            size_t* out_file_index) {
  // SVOD Systems use hash blocks for integrity checks. These hash blocks
  // cause blocks to be discontinuous in memory, and must be accounted for.
  //  - Each data block is 0x800 bytes in length
  //  - Every group of 0x198 data blocks is preceded a Level0 hash table.
  //    Level0 tables contain 0xCC hashes, each representing two data blocks.
  //    The total size of each Level0 hash table is 0x1000 bytes in length.
  //  - Every 0xA1C4 Level0 hash tables is preceded by a Level1 hash table.
  //    Level1 tables contain 0xCB hashes, each representing two Level0 hashes.
  //    The total size of each Level1 hash table is 0x1000 bytes in length.
  //  - Files are split into fragments of 0xA290000 bytes in length,
  //    consisting of 0x14388 data blocks, 0xCB Level0 hash tables, and 0x1
  //    Level1 hash table.

  const size_t BLOCK_SIZE = 0x800;
  const size_t HASH_BLOCK_SIZE = 0x1000;
  const size_t BLOCKS_PER_L0_HASH = 0x198;
  const size_t HASHES_PER_L1_HASH = 0xA1C4;
  const size_t BLOCKS_PER_FILE = 0x14388;
  const size_t MAX_FILE_SIZE = 0xA290000;
  const size_t BLOCK_OFFSET =
      header_.metadata.svod_volume_descriptor.start_data_block();

  // Resolve the true block address and file index
  size_t true_block = block - (BLOCK_OFFSET * 2);
  if (svod_layout_ == SvodLayoutType::kEnhancedGDF) {
    // EGDF has an 0x1000 byte offset, which is two blocks
    true_block += 0x2;
  }

  size_t file_block = true_block % BLOCKS_PER_FILE;
  size_t file_index = true_block / BLOCKS_PER_FILE;
  size_t offset = 0;

  // Calculate offset caused by Level0 Hash Tables
  size_t level0_table_count = (file_block / BLOCKS_PER_L0_HASH) + 1;
  offset += level0_table_count * HASH_BLOCK_SIZE;

  // Calculate offset caused by Level1 Hash Tables
  size_t level1_table_count = (level0_table_count / HASHES_PER_L1_HASH) + 1;
  offset += level1_table_count * HASH_BLOCK_SIZE;

  // For single-file SVOD layouts, include the size of the header in the offset.
  if (svod_layout_ == SvodLayoutType::kSingleFile) {
    offset += base_offset_;
  }

  size_t block_address = (file_block * BLOCK_SIZE) + offset;

  // If the offset causes the block address to overrun the file, round it.
  if (block_address >= MAX_FILE_SIZE) {
    file_index += 1;
    block_address %= MAX_FILE_SIZE;
    block_address += 0x2000;
  }

  *out_address = block_address;
  *out_file_index = file_index;
}

StfsContainerDevice::Error StfsContainerDevice::ReadSTFS() {
  auto data = mmap_.at(0)->data();

  auto root_entry = new StfsContainerEntry(this, nullptr, "", &mmap_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  std::vector<StfsContainerEntry*> all_entries;

  // Load all listings.
  auto& volume_descriptor = header_.metadata.stfs_volume_descriptor;
  uint32_t table_block_index = volume_descriptor.directory_block_num();
  for (size_t n = 0; n < volume_descriptor.directory_block_count(); n++) {
    const uint8_t* p = data + STFSDataBlockToOffset(table_block_index);
    for (size_t m = 0; m < 0x1000 / 0x40; m++) {
      const uint8_t* filename = p;  // 0x28b
      if (filename[0] == 0) {
        // Done.
        break;
      }
      uint8_t filename_length_flags = xe::load_and_swap<uint8_t>(p + 0x28);
      // TODO(benvanik): use for allocation_size_?
      // uint32_t allocated_block_count = load_uint24_le(p + 0x29);
      uint32_t start_block_index = load_uint24_le(p + 0x2F);
      uint16_t path_indicator = xe::load_and_swap<uint16_t>(p + 0x32);
      uint32_t file_size = xe::load_and_swap<uint32_t>(p + 0x34);

      // both date and time parts of the timestamp are big endian
      uint16_t update_date = xe::load_and_swap<uint16_t>(p + 0x38);
      uint16_t update_time = xe::load_and_swap<uint16_t>(p + 0x3A);
      uint32_t access_date = xe::load_and_swap<uint16_t>(p + 0x3C);
      uint32_t access_time = xe::load_and_swap<uint16_t>(p + 0x3E);
      p += 0x40;

      StfsContainerEntry* parent_entry = nullptr;
      if (path_indicator == 0xFFFF) {
        parent_entry = root_entry;
      } else {
        parent_entry = all_entries[path_indicator];
      }

      std::string name_str(reinterpret_cast<const char*>(filename),
                           filename_length_flags & 0x3F);
      auto entry =
          StfsContainerEntry::Create(this, parent_entry, name_str, &mmap_);

      // bit 0x40 = consecutive blocks (not fragmented?)
      if (filename_length_flags & 0x80) {
        entry->attributes_ = kFileAttributeDirectory;
      } else {
        entry->attributes_ = kFileAttributeNormal | kFileAttributeReadOnly;
        entry->data_offset_ = STFSDataBlockToOffset(start_block_index);
        entry->data_size_ = file_size;
      }
      entry->size_ = file_size;
      entry->allocation_size_ = xe::round_up(file_size, bytes_per_sector());

      entry->create_timestamp_ = decode_fat_timestamp(update_date, update_time);
      entry->access_timestamp_ = decode_fat_timestamp(access_date, access_time);
      entry->write_timestamp_ = entry->create_timestamp_;

      all_entries.push_back(entry.get());

      // Fill in all block records.
      // It's easier to do this now and just look them up later, at the cost
      // of some memory. Nasty chain walk.
      if (entry->attributes() & X_FILE_ATTRIBUTE_NORMAL) {
        uint32_t block_index = start_block_index;
        size_t remaining_size = file_size;
        while (remaining_size && block_index) {
          assert_true(block_index != 0xffffff);

          size_t block_size =
              std::min(static_cast<size_t>(0x1000), remaining_size);
          size_t offset = STFSDataBlockToOffset(block_index);
          entry->block_list_.push_back({0, offset, block_size});
          remaining_size -= block_size;

          // If file entry has contiguous flag (0x40) set, skip reading next
          // block from hash table and just use block_index + 1 (but we'll only
          // do this if it's a read-only package, just in case the flag is in
          // error)
          if ((filename_length_flags & 0x40) &&
              header_.metadata.stfs_volume_descriptor.flags.read_only_format) {
            block_index++;
          } else {
            auto block_hash = STFSGetLevel0HashEntry(data, block_index);
            block_index = block_hash.level0_next_block();
          }
        }
      }

      parent_entry->children_.emplace_back(std::move(entry));
    }

    auto block_hash = STFSGetLevel0HashEntry(data, table_block_index);
    table_block_index = block_hash.level0_next_block();
  }

  // At this point we've read in all the data we need from the hash tables
  // Let's free some mem by clearing the cache we made.
  cached_tables_.clear();

  if (all_entries.size() > 0) {
    return Error::kSuccess;
  }

  // No entries found... return failure
  return Error::kErrorReadError;
}

uint64_t StfsContainerDevice::STFSDataBlockToBackingBlock(
    uint64_t block_index) {
  // For every level there is a hash table
  // Level 0: hash table of next 170 blocks
  // Level 1: hash table of next 170 hash tables
  // Level 2: hash table of next 170 level 1 hash tables
  // And so on...
  uint64_t block = block_index;
  for (uint32_t i = 0; i < 3; i++) {
    block += blocks_per_hash_table_ *
             ((block_index + kSTFSDataBlocksPerHashLevel[i]) /
              kSTFSDataBlocksPerHashLevel[i]);
    if (block_index < kSTFSDataBlocksPerHashLevel[i]) {
      break;
    }
  }

  return block;
}

uint64_t StfsContainerDevice::STFSDataBlockToBackingHashBlock(uint64_t block,
                                                              uint32_t level) {
  uint64_t backing_num = 0;
  switch (level) {
    case 0:
      backing_num = (block / kSTFSDataBlocksPerHashLevel[0]) * block_step_[0];
      if (block / kSTFSDataBlocksPerHashLevel[0] == 0) {
        return backing_num;
      }

      backing_num += ((block / kSTFSDataBlocksPerHashLevel[1]) + 1) *
                     blocks_per_hash_table_;
      if (block / kSTFSDataBlocksPerHashLevel[1] == 0) {
        return backing_num;
      }
      break;
    case 1:
      backing_num = (block / kSTFSDataBlocksPerHashLevel[1]) * block_step_[1];
      if (block / kSTFSDataBlocksPerHashLevel[1] == 0) {
        return backing_num + block_step_[0];
      }
      break;
    default:
      return block_step_[1];
  }

  return backing_num + blocks_per_hash_table_;
}

size_t StfsContainerDevice::STFSBackingBlockToOffset(uint64_t backing_block) {
  return xe::round_up(header_.header.header_size, 0x1000) +
         (backing_block * 0x1000);
}

size_t StfsContainerDevice::STFSDataBlockToOffset(uint64_t block) {
  return STFSBackingBlockToOffset(STFSDataBlockToBackingBlock(block));
}

size_t StfsContainerDevice::STFSDataBlockToBackingHashBlockOffset(
    uint64_t block, uint32_t level) {
  return STFSBackingBlockToOffset(
      STFSDataBlockToBackingHashBlock(block, level));
}

StfsHashEntry StfsContainerDevice::STFSGetLevelNHashEntry(
    const uint8_t* map_ptr, uint32_t block_index, uint32_t level,
    uint8_t* hash_in_out, bool secondary_block) {
  uint32_t record = block_index;
  for (uint32_t i = 0; i < level; i++) {
    record = record / kSTFSDataBlocksPerHashLevel[0];
  }
  record = record % kSTFSDataBlocksPerHashLevel[0];

  size_t hash_offset =
      STFSDataBlockToBackingHashBlockOffset(block_index, level);
  if (secondary_block &&
      !header_.metadata.stfs_volume_descriptor.flags.read_only_format) {
    hash_offset += bytes_per_sector();  // read from this tables secondary block
  }

  bool invalid_table = std::find(invalid_tables_.begin(), invalid_tables_.end(),
                                 hash_offset) != invalid_tables_.end();

  if (!cached_tables_.count(hash_offset)) {
    // Cache the table in memory, since it's likely to be needed again
    auto hash_data = (const StfsHashTable*)(map_ptr + hash_offset);
    cached_tables_[hash_offset] = *hash_data;

    // If hash is provided we'll try comparing it to the hash of this table
    if (hash_in_out && !invalid_table) {
      sha1::SHA1 sha;
      sha.processBytes(hash_data, 0x1000);

      uint8_t digest[0x14];
      sha.finalize(digest);
      if (memcmp(digest, hash_in_out, 0x14)) {
        XELOGW(
            "STFSGetLevelNHashEntry: level %d hash table at 0x%llX "
            "is corrupt (hash mismatch)!",
            level, hash_offset);
        invalid_table = true;
        invalid_tables_.push_back(hash_offset);
      }
    }
  }

  if (invalid_table) {
    // If table is corrupt there's no use reading invalid data, lets try
    // salvaging things by providing next block as block + 1, should work fine
    // for LIVE/PIRS packages hopefully.
    StfsHashEntry entry = {0};
    entry.level0_next_block(block_index + 1);
    return entry;
  }

  StfsHashTable& hash_table = cached_tables_[hash_offset];

  auto& entry = hash_table.entries[record];
  if (hash_in_out) {
    memcpy(hash_in_out, entry.sha1, 0x14);
  }
  return entry;
}

StfsHashEntry StfsContainerDevice::STFSGetLevel0HashEntry(
    const uint8_t* map_ptr, uint32_t block_index) {
  bool use_secondary_block = false;
  // Use secondary block for root table if RootActiveIndex flag is set
  if (header_.metadata.stfs_volume_descriptor.flags.root_active_index) {
    use_secondary_block = true;
  }

  // Copy our top hash table hash into the buffer...
  uint8_t hash[0x14];
  memcpy(hash, header_.metadata.stfs_volume_descriptor.root_hash, 0x14);

  // Check upper hash table levels to find which table (primary/secondary) to
  // use.

  // We used to always skip this if package is read-only, but it seems there's
  // a lot of LIVE/PIRS packages with corrupt hash tables out there.
  // Checking the hash table hashes is the only way to detect (and then
  // possibly salvage) these.
  auto num_blocks =
      header_.metadata.stfs_volume_descriptor.allocated_block_count;

  if (num_blocks >= kSTFSDataBlocksPerHashLevel[1]) {
    // Get the L2 entry for the block
    auto l2_entry = STFSGetLevelNHashEntry(map_ptr, block_index, 2, hash,
                                           use_secondary_block);
    use_secondary_block = false;
    if (l2_entry.levelN_activeindex()) {
      use_secondary_block = true;
    }
  }

  if (num_blocks >= kSTFSDataBlocksPerHashLevel[0]) {
    // Get the L1 entry for this block
    auto l1_entry = STFSGetLevelNHashEntry(map_ptr, block_index, 1, hash,
                                           use_secondary_block);
    use_secondary_block = false;
    if (l1_entry.levelN_activeindex()) {
      use_secondary_block = true;
    }
  }

  return STFSGetLevelNHashEntry(map_ptr, block_index, 0, hash,
                                use_secondary_block);
}

uint32_t StfsContainerDevice::ReadMagic(const std::wstring& path) {
  auto map = MappedMemory::Open(path, MappedMemory::Mode::kRead, 0, 4);
  return xe::load_and_swap<uint32_t>(map->data());
}

bool StfsContainerDevice::ResolveFromFolder(const std::wstring& path) {
  // Scan through folders until a file with magic is found
  std::queue<filesystem::FileInfo> queue;

  filesystem::FileInfo folder;
  filesystem::GetInfo(local_path_, &folder);
  queue.push(folder);

  while (!queue.empty()) {
    auto current_file = queue.front();
    queue.pop();

    if (current_file.type == filesystem::FileInfo::Type::kDirectory) {
      auto path = xe::join_paths(current_file.path, current_file.name);
      auto child_files = filesystem::ListFiles(path);
      for (auto file : child_files) {
        queue.push(file);
      }
    } else {
      // Try to read the file's magic
      auto path = xe::join_paths(current_file.path, current_file.name);
      auto magic = ReadMagic(path);

      if (magic == XContentPackageType::kPackageTypeCon ||
          magic == XContentPackageType::kPackageTypeLive ||
          magic == XContentPackageType::kPackageTypePirs) {
        local_path_ = xe::join_paths(current_file.path, current_file.name);
        XELOGI("STFS Package found: %s", xe::to_string(local_path_).c_str());
        return true;
      }
    }
  }

  if (local_path_ == path) {
    // Could not find a suitable container file
    return false;
  }
  return true;
}

uint32_t StfsContainerDevice::ExtractToFolder(const std::wstring& base_path) {
  XELOGD("Unpacking to %S", base_path.c_str());

  // Create path if it doesn't exist
  if (!filesystem::PathExists(base_path)) {
    filesystem::CreateFolder(base_path);
  }

  // Run through all the files, breadth-first style.
  std::queue<vfs::Entry*> queue;
  auto root = ResolvePath("/");
  queue.push(root);

  // Allocate a buffer when needed.
  size_t buffer_size = 0;
  uint8_t* buffer = nullptr;
  uint32_t extracted = 0;

  while (!queue.empty()) {
    auto entry = queue.front();
    queue.pop();
    for (auto& entry : entry->children()) {
      queue.push(entry.get());
    }

    XELOGD(" %s", entry->path().c_str());
    auto dest_name = xe::join_paths(base_path, xe::to_wstring(entry->path()));
    if (entry->attributes() & kFileAttributeDirectory) {
      xe::filesystem::CreateFolder(dest_name + L"\\");
      continue;
    }

    vfs::File* in_file = nullptr;
    if (entry->Open(FileAccess::kFileReadData, &in_file) != X_STATUS_SUCCESS) {
      continue;
    }

    auto file = xe::filesystem::OpenFile(dest_name, "wb");
    if (!file) {
      in_file->Destroy();
      continue;
    }

    if (entry->can_map()) {
      auto map = entry->OpenMapped(xe::MappedMemory::Mode::kRead);
      fwrite(map->data(), map->size(), 1, file);
      map->Close();
    } else {
      // Can't map the file into memory. Read it into a temporary buffer.
      if (!buffer || entry->size() > buffer_size) {
        // Resize the buffer.
        if (buffer) {
          delete[] buffer;
        }

        // Allocate a buffer rounded up to the nearest 512MB.
        buffer_size = xe::round_up(entry->size(), 512 * 1024 * 1024);
        buffer = new uint8_t[buffer_size];
      }

      size_t bytes_read = 0;
      in_file->ReadSync(buffer, entry->size(), 0, &bytes_read);
      fwrite(buffer, bytes_read, 1, file);
    }

    extracted++;

    fclose(file);
    in_file->Destroy();
  }

  if (buffer) {
    delete[] buffer;
  }

  return extracted;
}

}  // namespace vfs
}  // namespace xe