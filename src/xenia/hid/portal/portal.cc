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

bool Portal::IsConnected() {
  std::lock_guard<xe_mutex> guard(lock_);
  return connected_;
}

X_STATUS Portal::Read(std::span<uint8_t> data, uint32_t& bytes_read,
                      uint16_t& state) {
  std::lock_guard<xe_mutex> guard(lock_);

  if (!connected_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (data.size() > kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  int32_t read_count = 0;
  X_STATUS status = ReadInternal(data, read_count);

  if (XSUCCEEDED(status)) {
    // According to XAM decompilation logic here is reversed. Difference in
    // status should return 1, but these values are taken from internal parts of
    // xinput raw implementation, so it might be completely opposite.
    state = previous_status_ == status;
    bytes_read = read_count;
  }

  previous_status_ = status;

  if (status == X_ERROR_DEVICE_NOT_CONNECTED) {
    CloseDevice();
  }

  return status;
}

X_STATUS Portal::Write(std::span<uint8_t> data) {
  std::lock_guard<xe_mutex> guard(lock_);

  if (!connected_) {
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
