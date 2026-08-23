/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_OPTICAL_DRIVE_DEVICE_H_
#define XENIA_VFS_OPTICAL_DRIVE_DEVICE_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "xenia/vfs/physical_device.h"

namespace xe {
namespace vfs {

class OpticalDriveDevice : public PhysicalDevice {
 public:
  using PhysicalDevice::PhysicalDevice;

 protected:
  static bool IsSecuritySectorSane(std::span<const uint8_t> payload);
  static constexpr size_t SecuritySectorPayloadSize() { return 2048; }
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_OPTICAL_DRIVE_DEVICE_H_
