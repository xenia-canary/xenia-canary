/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_PORTAL_HARDWARE_PORTAL_H_
#define XENIA_HID_PORTAL_HARDWARE_PORTAL_H_

#include <array>
#include "xenia/hid/portal/portal.h"

#include "third_party/libusb/libusb/libusb.h"

namespace xe {
namespace hid {

constexpr std::array<std::pair<uint16_t, uint16_t>, 2>
    kPortalVendorProductIdList = {
        std::pair<uint16_t, uint16_t>{0x1430, 0x1F17},
        std::pair<uint16_t, uint16_t>{0x24C6, 0xFA00}};

class HardwarePortal final : public Portal {
 public:
  HardwarePortal();
  ~HardwarePortal() override;

  virtual bool IsConnected() override;

  virtual void OnDeviceArrival() override;
  virtual void OnDeviceRemoval() override;

 private:
  virtual void OpenDevice() override;
  virtual void CloseDevice() override;
  virtual X_STATUS ReadInternal(std::span<uint8_t> data,
                                int32_t& read_count) override;
  virtual X_STATUS WriteInternal(std::span<uint8_t> data) override;

  const uint8_t read_endpoint = 0x81;
  const uint8_t write_endpoint = 0x02;
  const uint16_t timeout = 100;

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_PORTAL_HARDWARE_PORTAL_H_
