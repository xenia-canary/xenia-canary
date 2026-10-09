/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/portal/hardware_portal.h"

#include <algorithm>

#include "xenia/base/logging.h"

namespace xe {
namespace hid {

HardwarePortal::HardwarePortal() : Portal() {
  libusb_init(&context_);
  OpenDevice();
  StartReaderThread();
}

HardwarePortal::~HardwarePortal() {
  if (handle_) {
    CloseDevice();
  }

  libusb_exit(context_);
}

bool HardwarePortal::IsConnected() { return handle_ != nullptr; }

bool HardwarePortal::UsesAsyncReads() const {
  return hid_mode_.load(std::memory_order_acquire) != PortalHidMode::kLegacy;
}

void HardwarePortal::StartReaderThread() {
  if (!handle_ || !UsesAsyncReads() || reader_thread_.joinable()) {
    return;
  }

  stop_reader_.store(false, std::memory_order_release);
  reader_thread_ = std::thread(&HardwarePortal::ReaderThreadMain, this);
}

void HardwarePortal::StopReaderThread() {
  if (!reader_thread_.joinable()) {
    return;
  }

  stop_reader_.store(true, std::memory_order_release);
  reader_thread_.join();
  stop_reader_.store(false, std::memory_order_release);
}

void HardwarePortal::ReaderThreadMain() {
  while (!stop_reader_.load(std::memory_order_acquire)) {
    libusb_device_handle* handle = handle_;
    if (!handle) {
      break;
    }

    std::array<uint8_t, kPortalBufferSize> buffer{};
    int32_t packet_length = 0;

    const int result = libusb_interrupt_transfer(
        handle, read_endpoint, buffer.data(), static_cast<int>(buffer.size()),
        &packet_length, kAsyncTransferTimeout);

    if (stop_reader_.load(std::memory_order_acquire)) {
      break;
    }

    switch (result) {
      case LIBUSB_ERROR_TIMEOUT:
        continue;
      case LIBUSB_ERROR_NO_DEVICE:
        stop_reader_.store(true, std::memory_order_release);
        return;
      case LIBUSB_ERROR_PIPE:
        libusb_clear_halt(handle, read_endpoint);
        continue;
      default:
        break;
    }

    if (result < 0) {
      XELOGW("Portal[Read] returned error: {:08X}", result);
      continue;
    }

    if (packet_length > 0 &&
        packet_length <= static_cast<int32_t>(kPortalBufferSize)) {
      const bool use_legacy_hid = IdentifyPortalFromPacket(
          std::span<const uint8_t>(buffer.data(), packet_length));
      QueuePacket(std::move(buffer), packet_length);
      if (use_legacy_hid) {
        // The identifying packet is queued for the guest. All later reads use
        // the original synchronous HID path for this physical portal.
        return;
      }
    }
  }
}

bool HardwarePortal::IdentifyPortalFromPacket(std::span<const uint8_t> packet) {
  if (hid_mode_.load(std::memory_order_acquire) != PortalHidMode::kUnknown) {
    return false;
  }

  size_t id_offset = 0;
  if (packet.size() >= 5 && packet[0] == 0x0B && packet[1] == 0x14 &&
      packet[2] == 0x52) {
    // Xbox 360 portals wrap the standard 'R' identification response.
    id_offset = 3;
  } else if (packet.size() >= 3 && packet[0] == 0x52) {
    id_offset = 1;
  } else {
    return false;
  }

  const uint8_t id_major = packet[id_offset];
  const uint8_t id_minor = packet[id_offset + 1];
  const bool is_traptanium =
      id_major == 0x02 &&
      ((id_minor >= 0x18 && id_minor <= 0x1B) || id_minor == 0x27);
  const PortalHidMode detected_mode =
      is_traptanium ? PortalHidMode::kTraptaniumAsync : PortalHidMode::kLegacy;
  hid_mode_.store(detected_mode, std::memory_order_release);

  XELOGI("Portal identified as {} (ID {:02X} {:02X})",
         is_traptanium ? "Traptanium" : "legacy", id_major, id_minor);
  return detected_mode == PortalHidMode::kLegacy;
}

bool HardwarePortal::IsActiveDevicePresent() const {
  if (!context_ || !handle_) {
    return false;
  }

  libusb_device* active_device = libusb_get_device(handle_);
  if (!active_device) {
    return false;
  }

  libusb_device** device_list = nullptr;
  const ssize_t device_count = libusb_get_device_list(context_, &device_list);
  if (device_count < 0) {
    // A failed rescan is not proof that the portal was removed. Keep the
    // current handle and let the USB transfer path report a real disconnect.
    return true;
  }

  bool is_present = false;
  for (ssize_t i = 0; i < device_count; ++i) {
    if (device_list[i] == active_device) {
      is_present = true;
      break;
    }
  }

  libusb_free_device_list(device_list, 1);
  return is_present;
}

void HardwarePortal::QueuePacket(
    std::array<uint8_t, kPortalBufferSize>&& packet, int32_t length) {
  std::lock_guard<std::mutex> guard(read_queue_mutex_);

  // Keep recent input bounded if the guest temporarily stops polling.
  if (read_queue_.size() >= kMaxQueuedPackets) {
    read_queue_.pop();
  }

  read_queue_.push(QueuedPacket{std::move(packet), length});
}

bool HardwarePortal::DequeuePacket(std::span<uint8_t> data, int32_t& read_count) {
  std::lock_guard<std::mutex> guard(read_queue_mutex_);
  if (read_queue_.empty()) {
    read_count = 0;
    return false;
  }

  QueuedPacket packet = std::move(read_queue_.front());
  read_queue_.pop();

  const size_t copied =
      std::min(data.size(), static_cast<size_t>(packet.length));
  std::copy_n(packet.data.begin(), copied, data.begin());
  read_count = static_cast<int32_t>(copied);

  return true;
}

void HardwarePortal::OpenDevice() {
  if (!context_ || handle_) {
    return;
  }

  // Allow only one portal device at the time.
  for (const auto& entry : kSupportedPortalDevices) {
    handle_ = libusb_open_device_with_vid_pid(context_, entry.vendor_id,
                                              entry.product_id);
    if (handle_) {
      const int claim_result = libusb_claim_interface(handle_, 0);
      if (claim_result != LIBUSB_SUCCESS) {
        libusb_close(handle_);
        handle_ = nullptr;
        continue;
      }
      hid_mode_.store(entry.initial_mode, std::memory_order_release);
      break;
    }
  }
}

void HardwarePortal::CloseDevice() {
  if (!handle_) {
    return;
  }

  StopReaderThread();

  libusb_release_interface(handle_, 0);
  libusb_close(handle_);
  handle_ = nullptr;
  hid_mode_.store(PortalHidMode::kUnknown, std::memory_order_release);

  std::lock_guard<std::mutex> guard(read_queue_mutex_);
  while (!read_queue_.empty()) {
    read_queue_.pop();
  }
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  // Drain any packet queued before a portal identified itself as legacy.
  if (DequeuePacket(data, read_count)) {
    return X_ERROR_SUCCESS;
  }

  if (UsesAsyncReads()) {
    read_count = 0;
    return X_ERROR_SUCCESS;
  }

  // Original HID behavior used by older Skylanders portals and other bases.
  const int result = libusb_interrupt_transfer(
      handle_, read_endpoint, data.data(), static_cast<int>(data.size()),
      &read_count, kLegacyTransferTimeout);

  switch (result) {
    case LIBUSB_ERROR_NO_DEVICE:
      return X_ERROR_DEVICE_NOT_CONNECTED;
    case LIBUSB_ERROR_TIMEOUT:
      return X_ERROR_SUCCESS;
    default:
      break;
  }

  if (result < 0) {
    XELOGW("Portal[Read] returned error: {:08X}", result);
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  if (!UsesAsyncReads()) {
    // Preserve the original report length and timeout for legacy hardware.
    const int result = libusb_interrupt_transfer(
        handle_, write_endpoint, data.data(), static_cast<int>(data.size()),
        nullptr, kLegacyTransferTimeout);

    if (result < 0) {
      XELOGW("Portal[Write] returned error: {:08X}", result);
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }

    return X_ERROR_SUCCESS;
  }

  // Physical portal output uses fixed-size USB reports. Zero-pad shorter
  // guest writes to the report size before submitting the transfer.
  std::array<uint8_t, kPortalBufferSize> buffer{};
  const size_t copied = std::min<size_t>(data.size(), buffer.size());
  std::copy_n(data.begin(), copied, buffer.begin());

  int bytes_transferred = 0;
  const int result = libusb_interrupt_transfer(
      handle_, write_endpoint, buffer.data(), static_cast<int>(buffer.size()),
      &bytes_transferred, kAsyncTransferTimeout);

  if (result < 0) {
    XELOGW("Portal[Write] returned error: {:08X}", result);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (bytes_transferred != static_cast<int>(buffer.size())) {
    XELOGW("Portal[Write] incomplete transfer: {} of {} bytes",
           bytes_transferred, buffer.size());
    return X_ERROR_FUNCTION_FAILED;
  }

  return X_ERROR_SUCCESS;
};

void HardwarePortal::OnDeviceArrival() {
  std::lock_guard<xe_mutex> guard(lock_);
  OpenDevice();
  StartReaderThread();
}

void HardwarePortal::OnDeviceRemoval() {
  std::lock_guard<xe_mutex> guard(lock_);
  if (IsActiveDevicePresent()) {
    // Windows reports removal for every USB device, including controllers.
    // Do not reset a portal that is still physically connected.
    return;
  }
  CloseDevice();
}

}  // namespace hid
}  // namespace xe
