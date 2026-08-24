/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_STFS_XBOX_H_
#define XENIA_VFS_DEVICES_STFS_XBOX_H_

#include <time.h>

#include "xenia/base/byte_order.h"
#include "xenia/xbox.h"

namespace xe {
namespace vfs {

// Convert FAT timestamp to 100-nanosecond intervals since January 1, 1601 (UTC)
inline uint64_t decode_fat_timestamp(const uint32_t date, const uint32_t time) {
  struct tm tm = {0};
  // 80 is the difference between 1980 (FAT) and 1900 (tm);
  tm.tm_year = ((0xFE00 & date) >> 9) + 80;
  tm.tm_mon = ((0x01E0 & date) >> 5) - 1;
  tm.tm_mday = (0x001F & date) >> 0;
  tm.tm_hour = (0xF800 & time) >> 11;
  tm.tm_min = (0x07E0 & time) >> 5;
  tm.tm_sec = (0x001F & time) << 1;  // the value stored in 2-seconds intervals
  tm.tm_isdst = 0;

#if XE_PLATFORM_WIN32
  time_t timet = _mkgmtime(&tm);
#else
  time_t timet = timegm(&tm);
#endif

  if (timet == -1) {
    return 0;
  }
  // 11644473600LL is a difference between 1970 and 1601
  return (timet + 11644473600LL) * 10000000;
}

// Structs used for interchange between Xenia and actual Xbox360 kernel/XAM

inline uint32_t load_uint24_be(const uint8_t* p) {
  return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
}
inline uint32_t load_uint24_le(const uint8_t* p) {
  return (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}
inline void store_uint24_le(uint8_t* p, uint32_t value) {
  p[2] = uint8_t((value >> 16) & 0xFF);
  p[1] = uint8_t((value >> 8) & 0xFF);
  p[0] = uint8_t(value & 0xFF);
}

/* STFS structures */
#pragma pack(push, 1)
struct StfsVolumeDescriptor {
  uint8_t descriptor_length;
  uint8_t version;
  union {
    uint8_t as_byte;
    struct {
      uint8_t read_only_format : 1;  // if set, only uses a single backing-block
                                     // per hash table (no resiliency),
                                     // otherwise uses two
      uint8_t root_active_index : 1;  // if set, uses secondary backing-block
                                      // for the highest-level hash table

      uint8_t directory_overallocated : 1;
      uint8_t directory_index_bounds_valid : 1;
    } bits;
  } flags;
  uint16_t file_table_block_count;
  uint8_t file_table_block_number_raw[3];
  uint8_t top_hash_table_hash[0x14];
  be<uint32_t> total_block_count;
  be<uint32_t> free_block_count;

  uint32_t file_table_block_number() const {
    return load_uint24_le(file_table_block_number_raw);
  }

  void set_file_table_block_number(uint32_t value) {
    store_uint24_le(file_table_block_number_raw, value);
  }

  bool is_valid() const {
    return descriptor_length == sizeof(StfsVolumeDescriptor);
  }
};
static_assert_size(StfsVolumeDescriptor, 0x24);
#pragma pack(pop)

enum class StfsHashState : uint8_t {
  kFree = 0,   // unallocated but doesn't exist in package (needs to expand)?
  kFree2 = 1,  // unallocated but exists in package?
  kInUse = 2,
};

struct StfsHashEntry {
  uint8_t sha1[0x14];

  xe::be<uint32_t> info_raw;

  uint32_t level0_next_block() const { return info_raw & 0xFFFFFF; }
  void set_level0_next_block(uint32_t value) {
    info_raw = (info_raw & ~0xFFFFFF) | (value & 0xFFFFFF);
  }

  StfsHashState level0_allocation_state() const {
    return StfsHashState(uint8_t(((info_raw & 0xC0000000) >> 30) & 0xFF));
  }
  void set_level0_allocation_state(StfsHashState value) {
    info_raw = (info_raw & ~0xC0000000) | (uint32_t(value) << 30);
  }

  uint32_t levelN_num_blocks_free() const { return info_raw & 0x7FFF; }
  void set_levelN_num_blocks_free(uint32_t value) {
    info_raw = (info_raw & ~0x7FFF) | (value & 0x7FFF);
  }

  uint32_t levelN_num_blocks_unk() const {
    return ((info_raw & 0x3FFF8000) >> 15) & 0x7FFF;
  }
  void set_levelN_num_blocks_unk(uint32_t value) {
    info_raw = (info_raw & ~0x3FFF8000) | ((value & 0x7FFF) << 15);
  }

  bool levelN_active_index() const { return (info_raw & 0x40000000) != 0; }
  void set_levelN_active_index(bool value) {
    info_raw = (info_raw & ~0x40000000) | (value ? 0x40000000 : 0);
  }

  bool levelN_writeable() const { return (info_raw & 0x80000000) != 0; }
  void set_levelN_writeable(bool value) {
    info_raw = (info_raw & ~0x80000000) | (value ? 0x80000000 : 0);
  }
};
static_assert_size(StfsHashEntry, 0x18);

struct StfsHashTable {
  StfsHashEntry entries[170];
  xe::be<uint32_t> num_blocks;  // num L0 blocks covered by this table?
  uint8_t padding[12];
};
static_assert_size(StfsHashTable, 0x1000);

struct StfsDirectoryEntry {
  char name[40];

  struct {
    uint8_t name_length : 6;
    uint8_t contiguous : 1;
    uint8_t directory : 1;
  } flags;

  uint8_t valid_data_blocks_raw[3];
  uint8_t allocated_data_blocks_raw[3];
  uint8_t start_block_number_raw[3];

  be<uint16_t> directory_index;

  be<uint32_t> length;

  be<uint16_t> create_date;
  be<uint16_t> create_time;
  be<uint16_t> modified_date;
  be<uint16_t> modified_time;

  uint32_t valid_data_blocks() const {
    return load_uint24_le(valid_data_blocks_raw);
  }

  void set_valid_data_blocks(uint32_t value) {
    store_uint24_le(valid_data_blocks_raw, value);
  }

  uint32_t allocated_data_blocks() const {
    return load_uint24_le(allocated_data_blocks_raw);
  }

  void set_allocated_data_blocks(uint32_t value) {
    store_uint24_le(allocated_data_blocks_raw, value);
  }

  uint32_t start_block_number() const {
    return load_uint24_le(start_block_number_raw);
  }

  void set_start_block_number(uint32_t value) {
    store_uint24_le(start_block_number_raw, value);
  }
};
static_assert_size(StfsDirectoryEntry, 0x40);

struct StfsDirectoryBlock {
  StfsDirectoryEntry entries[0x40];
};
static_assert_size(StfsDirectoryBlock, 0x1000);

/* SVOD structures */
struct SvodDeviceDescriptor {
  uint8_t descriptor_length;
  uint8_t block_cache_element_count;
  uint8_t worker_thread_processor;
  uint8_t worker_thread_priority;
  uint8_t first_fragment_hash_entry[0x14];
  union {
    uint8_t as_byte;
    struct {
      uint8_t must_be_zero_for_future_usage : 6;
      uint8_t enhanced_gdf_layout : 1;
      uint8_t zero_for_downlevel_clients : 1;
    } bits;
  } features;
  uint8_t num_data_blocks_raw[3];
  uint8_t start_data_block_raw[3];
  uint8_t reserved[5];

  uint32_t num_data_blocks() const {
    return load_uint24_le(num_data_blocks_raw);
  }

  uint32_t start_data_block() const {
    return load_uint24_le(start_data_block_raw);
  }
};
static_assert_size(SvodDeviceDescriptor, 0x24);

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_STFS_XBOX_H_
