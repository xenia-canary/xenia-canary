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

enum class PortalHidMode : uint8_t {
  kUnknown,
  kLegacy,
  kTraptaniumAsync,
};

struct SupportedPortalDevice {
  uint16_t vendor_id;
  uint16_t product_id;
  PortalHidMode initial_mode;
};

constexpr std::array<SupportedPortalDevice, 2> kSupportedPortalDevices = {{
    {0x1430, 0x1F17, PortalHidMode::kUnknown},
    {0x24C6, 0xFA00, PortalHidMode::kLegacy},
}};

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
  static constexpr uint16_t kAsyncTransferTimeout = 30;
  static constexpr uint16_t kLegacyTransferTimeout = 100;

  void StartReaderThread();
  void StopReaderThread();
  void ReaderThreadMain();
  bool IdentifyPortalFromPacket(std::span<const uint8_t> packet);
  bool IsActiveDevicePresent() const;
  void QueuePacket(std::array<uint8_t, kPortalBufferSize>&& packet,
                   int32_t length);
  bool DequeuePacket(std::span<uint8_t> data, int32_t& read_count);

  virtual void OpenDevice() override;
  virtual void CloseDevice() override;
  virtual bool UsesAsyncReads() const override;
  virtual X_STATUS ReadInternal(std::span<uint8_t> data,
                                int32_t& read_count) override;
  virtual X_STATUS WriteInternal(std::span<uint8_t> data) override;

  const uint8_t read_endpoint = 0x81;
  const uint8_t write_endpoint = 0x02;

  libusb_context* context_ = nullptr;
  libusb_device_handle* handle_ = nullptr;
  std::atomic<PortalHidMode> hid_mode_{PortalHidMode::kUnknown};
  std::atomic_bool stop_reader_{false};
  std::thread reader_thread_;
  std::mutex read_queue_mutex_;
  std::queue<QueuedPacket> read_queue_;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_PORTAL_HARDWARE_PORTAL_H_
