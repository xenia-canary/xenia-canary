/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

X_STATUS xeExGetXConfigSetting(X_CONFIG_CATEGORY category,
                               const uint16_t setting_id, void* buffer,
                               uint16_t buffer_size, uint16_t* required_size) {
  if (!buffer && buffer_size) {
    return X_STATUS_INVALID_PARAMETER_3;
  }

  if (buffer) {
    if (buffer_size <
        kernel_state()->xconfig()->GetSettingSize(category, setting_id)) {
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    kernel_state()->xconfig()->ReadSetting(category, setting_id, buffer);
  }

  if (required_size) {
    *required_size = static_cast<uint16_t>(
        kernel_state()->xconfig()->GetSettingSize(category, setting_id));
  }

  return X_STATUS_SUCCESS;
}

dword_result_t ExGetXConfigSetting_entry(word_t category, word_t setting,
                                         lpvoid_t buffer_ptr,
                                         word_t buffer_size,
                                         lpword_t required_size_ptr) {
  uint16_t required_size = 0;
  X_STATUS result =
      xeExGetXConfigSetting(static_cast<X_CONFIG_CATEGORY>(category.value()),
                            setting, buffer_ptr, buffer_size, &required_size);

  if (required_size_ptr) {
    *required_size_ptr = required_size;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ExGetXConfigSetting, kModules, kImplemented);

dword_result_t ExSetXConfigSetting_entry(word_t category, word_t setting,
                                         lpvoid_t buffer_ptr,
                                         dword_t buffer_size) {
  /* Notes:
      Handles settings the only have a single flag/value like
     XCONFIG_USER_VIDEO_FLAGS to swap
  */
  kernel_state()->xconfig()->WriteSetting(
      static_cast<X_CONFIG_CATEGORY>(category.value()), setting.value(),
      kernel_memory()->TranslateVirtual<void*>(buffer_ptr));

  XELOGI("ExSetXConfigSetting: category: 0x{:04x}, setting: 0x{:04x}",
         static_cast<uint16_t>(category), static_cast<uint16_t>(setting));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ExSetXConfigSetting, kModules, kStub);

dword_result_t ExReadModifyWriteXConfigSettingUlong_entry(word_t category,
                                                          word_t setting,
                                                          dword_t bit_affected,
                                                          dword_t flag) {
  /* Notes:
      Handles settings with multiple flags like XCONFIG_USER_RETAIL_FLAGS and
     XCONFIG_CONSOLE_RETAIL_EX_FLAGS
  */
  uint32_t value = kernel_state()->xconfig()->ReadSetting<uint32_t>(
      static_cast<X_CONFIG_CATEGORY>(category.value()), setting);

  value = xe::byte_swap((value & bit_affected) | flag);

  kernel_state()->xconfig()->WriteSetting(
      static_cast<X_CONFIG_CATEGORY>(category.value()), setting, &value);

  XELOGI(
      "ExReadModifyWriteXConfigSettingUlong: category: 0x{:04x}, setting: "
      "{:04x}, changed bits: 0X{:08x}, setting flag 0X{:08x}",
      static_cast<uint16_t>(category), static_cast<uint16_t>(setting),
      static_cast<uint32_t>(bit_affected), static_cast<uint32_t>(flag));
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(ExReadModifyWriteXConfigSettingUlong, kModules, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(XConfig);
