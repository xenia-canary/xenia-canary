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

void HardwarePortal::StartReaderThread() {
  if (!handle_ || reader_thread_.joinable()) {
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
        &packet_length, timeout);

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
      QueuePacket(std::move(buffer), packet_length);
    }
  }
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
  for (const auto& entry : kPortalVendorProductIdList) {
    handle_ =
        libusb_open_device_with_vid_pid(context_, entry.first, entry.second);
    if (handle_) {
      const int claim_result = libusb_claim_interface(handle_, 0);
      if (claim_result != LIBUSB_SUCCESS) {
        libusb_close(handle_);
        handle_ = nullptr;
        continue;
      }
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

  std::lock_guard<std::mutex> guard(read_queue_mutex_);
  while (!read_queue_.empty()) {
    read_queue_.pop();
  }
}

X_STATUS HardwarePortal::ReadInternal(std::span<uint8_t> data,
                                      int32_t& read_count) {
  if (DequeuePacket(data, read_count)) {
    return X_ERROR_SUCCESS;
  }

  read_count = 0;
  return X_ERROR_SUCCESS;
}

X_STATUS HardwarePortal::WriteInternal(std::span<uint8_t> data) {
  // Physical portal output uses fixed-size USB reports. Zero-pad shorter
  // guest writes to the report size before submitting the transfer.
  std::array<uint8_t, kPortalBufferSize> buffer{};
  const size_t copied = std::min<size_t>(data.size(), buffer.size());
  std::copy_n(data.begin(), copied, buffer.begin());

  int bytes_transferred = 0;
  const int result = libusb_interrupt_transfer(
      handle_, write_endpoint, buffer.data(), static_cast<int>(buffer.size()),
      &bytes_transferred, timeout);

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
  CloseDevice();
}

}  // namespace hid
}  // namespace xe
