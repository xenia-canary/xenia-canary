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

enum class DeviceType : uint32_t {
  Invalid = 0x00000000,
  HDD = 0x00000001,
  MU = 0x00000002,  // support removed in 9199
  System = 0x00000003,
  ODD = 0x00000004,
  TransferCable = 0x00000008,
  Sapphire = 0x0000000F,
  USBMASS = 0x00000010,  // Added in 8955
  // ex) device_type == MU | MU0
  MU0 = 0x00010000,
  MU1 = 0x00020000,
  MUSFC = 0x00040000,
  MUUSB = 0x00080000,
  MUMMC = 0x00200000,
  NetworkStorageCache = 0x00400000,
  MUunk = 0x00100000,
  // ex) device_type == ?? | Unk
  unk1 = 0x02000000,
  unk2 = 0x04000000,
  Unk3 = 0x80000000
};

enum class DummyDeviceId : uint32_t {
  HDD = 1,
  ODD = 2,
};

struct DummyDeviceInfo {
  DummyDeviceId device_id;
  DeviceType device_type;
  uint64_t total_bytes;
  uint64_t free_bytes;
  const std::u16string_view name;
};

const DummyDeviceInfo* GetDummyDeviceInfo(uint32_t device_id);
std::vector<const DummyDeviceInfo*> ListStorageDevices(
    bool include_readonly = false);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_CONTENT_DEVICE_H_
