/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_PORTAL_PORTAL_H_
#define XENIA_HID_PORTAL_PORTAL_H_

#include <span>

#include "xenia/base/mutex.h"
#include "xenia/xbox.h"

namespace xe {
namespace hid {

constexpr uint8_t kPortalBufferSize = 0x20;

class Portal {
 public:
  Portal();
  virtual ~Portal();

  bool IsConnected();

  virtual X_STATUS Read(std::span<uint8_t> data, uint32_t& bytes_read,
                        uint16_t& state);
  virtual X_STATUS Write(std::span<uint8_t> data);

  virtual void OnDeviceArrival() = 0;
  virtual void OnDeviceRemoval() = 0;

 protected:
  bool connected_ = false;

 private:
  virtual void OpenDevice() = 0;
  virtual void CloseDevice() = 0;

  virtual X_STATUS ReadInternal(std::span<uint8_t> data,
                                int32_t& read_count) = 0;
  virtual X_STATUS WriteInternal(std::span<uint8_t> data) = 0;

  xe_mutex lock_;
  X_STATUS previous_status_ = 0;
};

}  // namespace hid
}  // namespace xe

#endif
