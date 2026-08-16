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
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

#include "xenia/hid/portal/portal.h"

#include "third_party/libusb/libusb/libusb.h"

#ifdef Portal
#undef Portal
#endif

namespace xe {
namespace hid {

constexpr std::array<std::pair<uint16_t, uint16_t>, 2>
    kPortalVendorProductIdList = {
        std::pair<uint16_t, uint16_t>{0x1430, 0x1F17},
        std::pair<uint16_t, uint16_t>{0x24C6, 0xFA00}};

struct QueuedPacket {
  std::array<uint8_t, kPortalBufferSize> data{};
  int32_t length = 0;
};

class HardwarePortal final : public Portal {
 public:
  HardwarePortal();
  ~HardwarePortal() override;

  virtual bool IsConnected() override;

  virtual void OnDeviceArrival() override;
  virtual void OnDeviceRemoval() override;

 private:
  static constexpr size_t kMaxQueuedPackets = 64;

  void StartReaderThread();
  void StopReaderThread();
  void ReaderThreadMain();
  void QueuePacket(std::array<uint8_t, kPortalBufferSize>&& packet,
                   int32_t length);
  bool DequeuePacket(std::span<uint8_t> data, int32_t& read_count);

  virtual void OpenDevice() override;
  virtual void CloseDevice() override;
  virtual X_STATUS ReadInternal(std::span<uint8_t> data,
                                int32_t& read_count) override;
  virtual X_STATUS WriteInternal(std::span<uint8_t> data) override;

  const uint8_t read_endpoint = 0x81;
  const uint8_t write_endpoint = 0x02;
  const uint16_t timeout = 30;

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  std::atomic_bool stop_reader_{false};
  std::thread reader_thread_;
  std::mutex read_queue_mutex_;
  std::queue<QueuedPacket> read_queue_;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_PORTAL_HARDWARE_PORTAL_H_
