/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#include <cerrno>
#include <cstdint>

#if XE_PLATFORM_LINUX
#include <fcntl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif  // XE_PLATFORM_LINUX

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"

#include <winioctl.h>  //The next newline fixes a linter bug. Removing it will break compilation.

#include <ntddcdrm.h>
#include <ntddscsi.h>
#endif  // XE_PLATFORM_WIN32

#define CDB_INTERNAL_TYPES
#include "xenia/vfs/devices/disc_scsi.h"
#undef CDB_INTERNAL_TYPES

namespace xe::vfs {

void PackBE32(uint32_t value, uint8_t out[4]) {
  out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(value & 0xFF);
}

CDB12 BuildReadOmniDriveCdb(uint32_t address, uint32_t transfer_length,
                            OmniDriveDiscType disc_type, bool raw_addressing,
                            bool fua, bool descramble,
                            OmniDriveSubchannel subchannels, bool c2) {
  CDB12_ReadOmniDrive cdb{};
  cdb.operation_code = 0xC0;

  cdb.byte1 = static_cast<uint8_t>(static_cast<uint8_t>(disc_type) & 0x03);
  if (raw_addressing) {
    cdb.byte1 |= (1u << 2);
  }
  if (fua) {
    cdb.byte1 |= (1u << 3);
  }
  if (descramble) {
    cdb.byte1 |= (1u << 4);
  }

  PackBE32(static_cast<uint32_t>(static_cast<int32_t>(address)), cdb.address);
  PackBE32(transfer_length, cdb.transfer_length);

  cdb.byte10 = static_cast<uint8_t>(static_cast<uint8_t>(subchannels) & 0x03);
  if (c2) {
    cdb.byte10 |= (1u << 2);
  }

  cdb.control = 0;
  return reinterpret_cast<CDB12*>(&cdb)[0];
}

CDB12 BuildRead12Cdb(uint32_t lba, uint32_t transfer_length, bool fua) {
  CDB12_Read12 cdb{};
  cdb.operation_code = 0xA8;
  cdb.byte1 = fua ? static_cast<uint8_t>(1u << 3) : 0;
  PackBE32(lba, cdb.logical_block_address);
  PackBE32(transfer_length, cdb.transfer_length);
  cdb.byte10 = 0;
  cdb.control = 0;
  return reinterpret_cast<CDB12*>(&cdb)[0];
}

namespace DeviceCommunication {
#if XE_PLATFORM_LINUX
bool RunScsiCommand(int fd, const char* command_name, const uint8_t* cdb,
                    uint8_t cdb_length, int data_direction, void* data_buffer,
                    uint32_t data_length, uint32_t timeout_ms,
                    uint8_t* out_sense_key, uint8_t* out_asc, uint8_t* out_ascq,
                    bool* out_size_related) {
  if (out_size_related) {
    *out_size_related = false;
  }
  uint8_t sense_buffer[32] = {0};
  sg_io_hdr_t io_hdr = {};
  io_hdr.interface_id = 'S';
  io_hdr.dxfer_direction = data_direction;
  io_hdr.cmd_len = cdb_length;
  io_hdr.mx_sb_len = sizeof(sense_buffer);
  io_hdr.dxfer_len = data_length;
  io_hdr.dxferp = data_buffer;
  io_hdr.cmdp = const_cast<uint8_t*>(cdb);
  io_hdr.sbp = sense_buffer;
  io_hdr.timeout = timeout_ms;

  if (ioctl(fd, SG_IO, &io_hdr) != 0) {
    const int ioctl_errno = errno;
    const bool size_related = ioctl_errno == EINVAL || ioctl_errno == ENOMEM;
    if (out_size_related) {
      *out_size_related = size_related;
    }
    if (!size_related) {
      XELOGW("SCSI: {} failed with SG_IO error: {}", command_name,
             std::strerror(ioctl_errno));
    }
    return false;
  }

  if (io_hdr.sb_len_wr >= 14 &&
      (sense_buffer[0] & 0x7F) == 0x70 /* current fixed-format sense */) {
    if (out_sense_key) {
      *out_sense_key = sense_buffer[2] & 0x0F;
    }
    if (out_asc) {
      *out_asc = sense_buffer[12];
    }
    if (out_ascq) {
      *out_ascq = sense_buffer[13];
    }
  }

  if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK || io_hdr.status != 0 ||
      io_hdr.host_status != 0 || io_hdr.driver_status != 0) {
    XELOGW(
        "SCSI: {} status=0x{:02X} host=0x{:04X} "
        "driver=0x{:04X} sense_len={}",
        command_name, io_hdr.status, io_hdr.host_status, io_hdr.driver_status,
        io_hdr.sb_len_wr);
    return false;
  }

  return true;
}
#endif  // XE_PLATFORM_LINUX
#if XE_PLATFORM_WIN32
struct SptdWithSense {
  SCSI_PASS_THROUGH_DIRECT sptd;
  ULONG filler;
  UCHAR sense_buffer[32];
};
bool RunScsiCommand(HANDLE handle, const char* command_name, const uint8_t* cdb,
                    uint8_t cdb_length, UCHAR data_in, void* data_buffer,
                    uint32_t data_length, uint32_t timeout_seconds,
                    uint8_t* out_sense_key, uint8_t* out_asc, uint8_t* out_ascq,
                    bool* out_size_related) {
  if (out_size_related) {
    *out_size_related = false;
  }
  SptdWithSense sptd = {};
  sptd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
  sptd.sptd.CdbLength = cdb_length;
  sptd.sptd.SenseInfoLength = sizeof(sptd.sense_buffer);
  sptd.sptd.DataIn = data_in;
  sptd.sptd.DataTransferLength = data_length;
  sptd.sptd.TimeOutValue = timeout_seconds;
  sptd.sptd.DataBuffer = data_buffer;
  sptd.sptd.SenseInfoOffset = offsetof(SptdWithSense, sense_buffer);
  std::memcpy(sptd.sptd.Cdb, cdb, cdb_length);

  DWORD returned = 0;
  if (!DeviceIoControl(handle, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                       sizeof(sptd), &sptd, sizeof(sptd), &returned, nullptr)) {
    const DWORD last_error = GetLastError();
    const bool size_related = last_error == ERROR_INVALID_PARAMETER ||
                              last_error == ERROR_INSUFFICIENT_BUFFER ||
                              last_error == ERROR_NOT_ENOUGH_MEMORY;
    if (out_size_related) {
      *out_size_related = size_related;
    }
    if (!size_related) {
      XELOGW("DiscOmnidriveDevice: {} failed for {} with error {}",
             command_name, "SPTI", last_error);
    }
    return false;
  }

  if (out_sense_key || out_asc || out_ascq) {
    const uint8_t sense_key = sptd.sense_buffer[2] & 0x0F;
    const uint8_t asc = sptd.sense_buffer[12];
    const uint8_t ascq = sptd.sense_buffer[13];
    if (out_sense_key) {
      *out_sense_key = sense_key;
    }
    if (out_asc) {
      *out_asc = asc;
    }
    if (out_ascq) {
      *out_ascq = ascq;
    }
  }

  if (sptd.sptd.ScsiStatus != 0) {
    XELOGW("DiscOmnidriveDevice: {} SPTI status=0x{:02X}", command_name,
           sptd.sptd.ScsiStatus);
    return false;
  }

  return true;
}
#endif  // XE_PLATFORM_WIN32
}  // namespace DeviceCommunication

}  // namespace xe::vfs
