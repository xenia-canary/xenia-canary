/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_
#define XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_

#include <cstdint>
#include <string_view>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {

enum DeviceType : uint32_t {
  Invalid = 0x00000000,
  HDD = 0x00000001,
  MU = 0x00000002,
  System = 0x00000003,
  ODD = 0x00000004,
  TransferCable = 0x00000008,
  Sapphire = 0x0000000F,
  USBMASS = 0x00000010,  // Added in 8955
};

enum DeviceTypeExtended : uint32_t {
  // ex) device_type == MU | MU0
  MU0 = 0x00010000,                  // "\\Device\\Mu0"
  MU1 = 0x00020000,                  // "\\Device\\Mu1"
  MUSFC = 0x00040000,                // "\\Device\\BuiltInMuSfc"
  MUUSB = 0x00080000,                // "\\Device\\BuiltInMuUsb"
  MUMMC = 0x00200000,                // "\\Device\\BuiltInMuMmc"
  NetworkStorageCache = 0x00400000,  // "\\Device\\NetworkStorageCache"
  MUunk = 0x00100000,                // ???
  // ex) device_type == ?? | Unk
  unk1 = 0x02000000,  // // device_info->unk == 7
  unk2 = 0x04000000,  // device_info->unk == 6
  Unk3 = 0x80000000   // device_info->unk == 4
};

enum class DummyDeviceId : uint32_t {
  HDD = 1,
  ODD = 2,
  MU = 3,
};

struct DummyDeviceInfo {
  const std::string_view volume_path;
  const std::u16string_view name;  // TODO: Allow name Change
  DummyDeviceId device_id;
  DeviceType device_type;
  uint64_t total_bytes;
  uint64_t free_bytes;
  uint32_t unk1;  // ranges from 3-7
  uint32_t unk2;  // pointer
  uint32_t unk3;  // bool?
  const std::string_view device_serial_number;
};

const DummyDeviceInfo* GetDummyDeviceInfo(uint32_t device_id);
std::vector<const DummyDeviceInfo*> ListStorageDevices(
    bool include_readonly = false);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_
