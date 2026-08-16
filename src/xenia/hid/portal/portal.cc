/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/portal/portal.h"

namespace xe {
namespace hid {

Portal::Portal() {}
Portal::~Portal() {}

X_STATUS Portal::Read(std::span<uint8_t> data, uint32_t& bytes_read,
                      uint16_t& state) {
  std::lock_guard<xe_mutex> guard(lock_);

  if (!IsConnected()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (data.size() > kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  int32_t read_count = 0;
  X_STATUS status = ReadInternal(data, read_count);

  if (XSUCCEEDED(status)) {
    // Empty asynchronous polls complete successfully with no data. Signal
    // state only when a packet was actually returned to the guest.
    state = read_count > 0 ? 1 : 0;
    bytes_read = read_count;
  }

  if (status == X_ERROR_DEVICE_NOT_CONNECTED) {
    CloseDevice();
  }

  return status;
}

X_STATUS Portal::Write(std::span<uint8_t> data) {
  std::lock_guard<xe_mutex> guard(lock_);

  if (!IsConnected()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (data.size() > kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const X_STATUS status = WriteInternal(data);
  if (status == X_ERROR_DEVICE_NOT_CONNECTED) {
    CloseDevice();
  }
  return status;
}

}  // namespace hid
}  // namespace xe
