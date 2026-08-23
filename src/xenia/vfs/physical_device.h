/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_VFS_PHYSICAL_DEVICE_H_
#define XENIA_VFS_PHYSICAL_DEVICE_H_

#include <cstddef>
#include <cstdint>
#include <span>

#include "xenia/vfs/device.h"

namespace xe {
namespace vfs {

class PhysicalDevice : public Device {
 public:
  using Device::Device;

  virtual bool ReadFileBytes(size_t offset,
                             std::span<uint8_t> buffer) const = 0;
  virtual bool is_media_available() const = 0;
  virtual bool has_physical_backend() const override { return true; }
};

}  // namespace vfs
}  // namespace xe

#endif  // XENIA_VFS_PHYSICAL_DEVICE_H_
