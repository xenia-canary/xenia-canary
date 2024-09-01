/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xime_app.h"

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XimeApp::XimeApp(KernelState* kernel_state) : App(kernel_state, 0xF5) {}

X_RESULT XimeApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00400001: {
      // XIME_INIT
      XELOGD("XimeInit({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00400002: {
      // XIME_UNINIT
      XELOGD("XimeUninit({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00400003: {
      // XIME_SET_PROPERTY
      XELOGD("XimeSetProperty({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00400004: {
      // XIME_SET_CHARACTER
      XELOGD("XimeSetCharacter({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00400005: {
      // XIME_GET_STRING
      XELOGD("XimeGetString({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented Xime message app=%.8X, msg=%.8X, arg1=%.8X, "
      "arg2=%.8X",
      app_id(), message, buffer_ptr, buffer_length);
  return X_STATUS_UNSUCCESSFUL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
