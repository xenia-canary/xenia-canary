/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_DEVICES_DISC_SCSI_H_
#define XENIA_VFS_DEVICES_DISC_SCSI_H_

#include <cstdint>

namespace xe {
namespace vfs {

enum class OmniDriveDiscType : uint8_t;
enum class OmniDriveSubchannel : uint8_t;

typedef struct {
  uint8_t reserved[12];
} CDB12;

#if defined(CDB_INTERNAL_TYPES)
#if XE_PLATFORM_WIN32
#pragma pack(push, 1)
typedef struct CDB12_ReadOmniDrive {
#else
typedef struct __attribute__((__packed__)) CDB12_ReadOmniDrive {
#endif
  uint8_t operation_code;
  uint8_t byte1;
  uint8_t address[4];
  uint8_t transfer_length[4];
  uint8_t byte10;
  uint8_t control;
} CDB12_ReadOmniDrive;
#if XE_PLATFORM_WIN32
#pragma pack(pop)
#endif
static_assert(sizeof(CDB12_ReadOmniDrive) == 12,
              "CDB12_ReadOmniDrive must be 12 bytes");

#if XE_PLATFORM_WIN32
#pragma pack(push, 1)
typedef struct CDB12_Read12 {
#else
typedef struct __attribute__((__packed__)) CDB12_Read12 {
#endif
  uint8_t operation_code;
  uint8_t byte1;
  uint8_t logical_block_address[4];
  uint8_t transfer_length[4];
  uint8_t byte10;
  uint8_t control;
} CDB12_Read12;
#if XE_PLATFORM_WIN32
#pragma pack(pop)
#endif
static_assert(sizeof(CDB12_Read12) == 12, "CDB12_Read12 must be 12 bytes");
#endif  // !defined(CDB_INTERNAL_TYPES)

CDB12 BuildReadOmniDriveCdb(uint32_t address, uint32_t transfer_length,
                            OmniDriveDiscType disc_type, bool raw_addressing,
                            bool fua, bool descramble,
                            OmniDriveSubchannel subchannels, bool c2);

CDB12 BuildRead12Cdb(uint32_t lba, uint32_t transfer_length, bool fua);

namespace DeviceCommunication {
#if XE_PLATFORM_LINUX
bool RunScsiCommand(int fd, const char* command_name, const uint8_t* cdb,
                    uint8_t cdb_length, int data_direction, void* data_buffer,
                    uint32_t data_length, uint32_t timeout_ms = 8000,
                    uint8_t* out_sense_key = nullptr,
                    uint8_t* out_asc = nullptr, uint8_t* out_ascq = nullptr,
                    bool* out_size_related = nullptr);
#endif  // XE_PLATFORM_LINUX
#if XE_PLATFORM_WIN32
bool RunScsiCommand(HANDLE handle, const char* command_name, const uint8_t* cdb,
                    uint8_t cdb_length, UCHAR data_in, void* data_buffer,
                    uint32_t data_length, uint32_t timeout_seconds = 30,
                    uint8_t* out_sense_key = nullptr,
                    uint8_t* out_asc = nullptr, uint8_t* out_ascq = nullptr,
                    bool* out_size_related = nullptr);
#endif  // XE_PLATFORM_WIN32
}  // namespace DeviceCommunication
}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_DEVICES_DISC_SCSI_H_
