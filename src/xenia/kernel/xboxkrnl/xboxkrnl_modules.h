/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULES_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULES_H_

#include "xenia/kernel/util/shim_utils.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XexGetModuleHandle(std::string module_name,
                                  xe::be<uint32_t>* hmodule_ptr);

enum X_MODULE_FLAGS : uint32_t {
  TITLE_PROCESS = 0x00000001,
  TITLE_IMPORT = 0x00000002,
  DEBUGGER = 0x00000004,
  LOAD_DLL = 0x00000008,
  LOAD_PATCH = 0x00000010,
  LOAD_FULL_PATCH = 0x00000020,
  LOAD_DELTA_PATCH = 0x00000040,
  BOUND_PATH = 0x40000000,
  SILENT_LOAD = 0x80000000,
};

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULES_H_
