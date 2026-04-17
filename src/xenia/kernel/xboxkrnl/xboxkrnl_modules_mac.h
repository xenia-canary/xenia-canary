/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULES_MAC_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULES_MAC_H_

#include <array>
#include <cstdint>

#include "xenia/kernel/util/shim_utils.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

constexpr std::array<uint16_t, 13> kGpuBootstrapOrdinals = {
    0x01B6, 0x01BC, 0x01BD, 0x01BE, 0x01BF, 0x01C2, 0x01C3,
    0x01C6, 0x01D5, 0x025B, 0x0268, 0x0269, 0x026A};

struct GpuBootstrapOrdinalRuntimeStat {
  uint16_t ordinal = 0;
  uint64_t dynamic_lookup_calls = 0;
  uint64_t dynamic_lookup_resolved_calls = 0;
  uint64_t dynamic_lookup_missing_calls = 0;
  uint64_t dynamic_lookup_invalid_module_calls = 0;
  uint64_t dynamic_lookup_fallback_hits = 0;
  uint64_t static_thunk_calls = 0;
  uint64_t export_entry_calls = 0;
};

struct GpuBootstrapImportLookupStats {
  uint64_t loader_module_handle_calls_total = 0;
  uint64_t loader_proc_lookup_calls_total = 0;
  uint64_t guest_main_dispatch_bridge_calls_total = 0;
  uint64_t module_handle_calls = 0;
  uint64_t module_handle_failures = 0;
  uint64_t proc_lookup_calls = 0;
  uint64_t proc_lookup_resolved = 0;
  uint64_t proc_lookup_missing = 0;
  uint64_t proc_lookup_invalid_module = 0;
  uint64_t proc_lookup_fallback_hits = 0;
  uint64_t ordinal_01BC_calls = 0;
  uint64_t ordinal_01BF_calls = 0;
  uint64_t ordinal_0268_calls = 0;
  uint64_t xex_check_executable_privilege_static_thunk_calls = 0;
  uint64_t xex_check_executable_privilege_export_entry_calls = 0;
  uint64_t xex_check_executable_privilege_calls = 0;
  uint64_t xex_check_executable_privilege_true = 0;
  uint64_t xex_check_executable_privilege_false = 0;
  uint32_t xex_check_executable_privilege_last_privilege = 0;
  uint32_t xex_check_executable_privilege_last_flags = 0;
  uint32_t xex_check_executable_privilege_last_result = 0;
  std::array<GpuBootstrapOrdinalRuntimeStat, kGpuBootstrapOrdinals.size()>
      ordinal_runtime_stats{};
};

dword_result_t XexGetModuleHandle(std::string module_name,
                                  xe::be<uint32_t>* hmodule_ptr);
GpuBootstrapImportLookupStats GetGpuBootstrapImportLookupStats();
void RecordGpuBootstrapGuestMainDispatchBridge(uint32_t address,
                                               uint32_t thread_id,
                                               const char* stage);
void RecordGpuBootstrapStaticThunkCall(uint16_t ordinal, uint32_t pc,
                                       uint32_t lr, uint32_t thread_id);
void RecordGpuBootstrapExportEntry(uint16_t ordinal, uint32_t pc, uint32_t lr,
                                   uint32_t thread_id);
const char* GetGpuBootstrapOrdinalName(uint16_t ordinal);
bool IsGpuBootstrapOrdinal(uint16_t ordinal);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULES_MAC_H_
