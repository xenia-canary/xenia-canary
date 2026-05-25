/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_modules_mac.h"

#include <array>
#include <atomic>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

enum class GpuBootstrapLookupKind {
  kNone = 0,
  kOrdinal = 1,
  kName = 2,
};

struct GpuBootstrapLookupTarget {
  GpuBootstrapLookupKind kind = GpuBootstrapLookupKind::kNone;
  uint16_t ordinal = 0;
  const char* name = nullptr;
};

std::atomic<uint64_t> gpu_module_handle_calls{0};
std::atomic<uint64_t> gpu_module_handle_failures{0};
std::atomic<uint64_t> gpu_proc_lookup_calls{0};
std::atomic<uint64_t> gpu_proc_lookup_resolved{0};
std::atomic<uint64_t> gpu_proc_lookup_missing{0};
std::atomic<uint64_t> gpu_proc_lookup_invalid_module{0};
std::atomic<uint64_t> gpu_proc_lookup_fallback_hits{0};
std::atomic<uint64_t> loader_module_handle_calls_total{0};
std::atomic<uint64_t> loader_proc_lookup_calls_total{0};
std::atomic<uint64_t> gpu_guest_main_dispatch_bridge_calls_total{0};
std::atomic<uint64_t> gpu_proc_lookup_ordinal_01BC{0};
std::atomic<uint64_t> gpu_proc_lookup_ordinal_01BF{0};
std::atomic<uint64_t> gpu_proc_lookup_ordinal_0268{0};
constexpr uint16_t kXexCheckExecutablePrivilegeOrdinal = 0x0194;
std::atomic<uint64_t> gpu_xex_check_executable_privilege_static_thunk_calls{0};
std::atomic<uint64_t> gpu_xex_check_executable_privilege_export_entry_calls{0};
std::atomic<uint64_t> gpu_xex_check_executable_privilege_calls{0};
std::atomic<uint64_t> gpu_xex_check_executable_privilege_true{0};
std::atomic<uint64_t> gpu_xex_check_executable_privilege_false{0};
std::atomic<uint32_t> gpu_xex_check_executable_privilege_last_privilege{0};
std::atomic<uint32_t> gpu_xex_check_executable_privilege_last_flags{0};
std::atomic<uint32_t> gpu_xex_check_executable_privilege_last_result{0};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_static_thunk_call_counts{};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_dynamic_lookup_call_counts{};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_dynamic_lookup_resolved_counts{};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_dynamic_lookup_missing_counts{};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_dynamic_lookup_invalid_module_counts{};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_dynamic_lookup_fallback_hit_counts{};
std::array<std::atomic<uint64_t>, kGpuBootstrapOrdinals.size()>
    gpu_export_entry_call_counts{};

bool IsXboxkrnlModuleName(const std::string_view module_name) {
  if (module_name.empty()) {
    return false;
  }
  const std::string guest_name =
      xe::utf8::find_name_from_guest_path(module_name);
  const std::string guest_base =
      xe::utf8::find_base_name_from_guest_path(module_name);
  return xe::utf8::equal_case(module_name, "xboxkrnl") ||
         xe::utf8::equal_case(module_name, "xboxkrnl.exe") ||
         xe::utf8::equal_case(guest_name, "xboxkrnl.exe") ||
         xe::utf8::equal_case(guest_base, "xboxkrnl");
}

GpuBootstrapLookupTarget ResolveGpuBootstrapLookupTarget(
    uint32_t lookup_value, bool is_string_name, const char* string_name) {
  GpuBootstrapLookupTarget target;
  if (!is_string_name) {
    switch (lookup_value & 0xFFFF) {
      case 0x01B6:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01B6,
                  "VdEnableRingBufferRPtrWriteBack"};
        break;
      case 0x01BC:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01BC,
                  "VdGetGraphicsAsicID"};
        break;
      case 0x01BD:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01BD,
                  "VdGetSystemCommandBuffer"};
        break;
      case 0x01BE:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01BE,
                  "VdGlobalDevice"};
        break;
      case 0x01BF:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01BF,
                  "VdGlobalXamDevice"};
        break;
      case 0x01C2:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01C2,
                  "VdInitializeEngines"};
        break;
      case 0x01C3:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01C3,
                  "VdInitializeRingBuffer"};
        break;
      case 0x01C6:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01C6,
                  "VdIsHSIOTrainingSucceeded"};
        break;
      case 0x01D5:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x01D5,
                  "VdSetGraphicsInterruptCallback"};
        break;
      case 0x025B:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x025B, "VdSwap"};
        break;
      case 0x0268:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x0268,
                  "VdInitializeEDRAM"};
        break;
      case 0x0269:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x0269,
                  "VdRetrainEDRAM"};
        break;
      case 0x026A:
        target = {GpuBootstrapLookupKind::kOrdinal, 0x026A,
                  "VdRetrainEDRAMWorker"};
        break;
      default:
        break;
    }
    return target;
  }

  if (!string_name || string_name[0] == '\0') {
    return target;
  }

  std::string_view export_name(string_name);
  if (xe::utf8::equal_case(export_name, "VdEnableRingBufferRPtrWriteBack")) {
    return {GpuBootstrapLookupKind::kName, 0x01B6,
            "VdEnableRingBufferRPtrWriteBack"};
  }
  if (xe::utf8::equal_case(export_name, "VdGetGraphicsAsicID")) {
    return {GpuBootstrapLookupKind::kName, 0x01BC, "VdGetGraphicsAsicID"};
  }
  if (xe::utf8::equal_case(export_name, "VdGetSystemCommandBuffer")) {
    return {GpuBootstrapLookupKind::kName, 0x01BD,
            "VdGetSystemCommandBuffer"};
  }
  if (xe::utf8::equal_case(export_name, "VdGlobalDevice")) {
    return {GpuBootstrapLookupKind::kName, 0x01BE, "VdGlobalDevice"};
  }
  if (xe::utf8::equal_case(export_name, "VdGlobalXamDevice")) {
    return {GpuBootstrapLookupKind::kName, 0x01BF, "VdGlobalXamDevice"};
  }
  if (xe::utf8::equal_case(export_name, "VdInitializeEngines")) {
    return {GpuBootstrapLookupKind::kName, 0x01C2, "VdInitializeEngines"};
  }
  if (xe::utf8::equal_case(export_name, "VdInitializeRingBuffer")) {
    return {GpuBootstrapLookupKind::kName, 0x01C3,
            "VdInitializeRingBuffer"};
  }
  if (xe::utf8::equal_case(export_name, "VdIsHSIOTrainingSucceeded")) {
    return {GpuBootstrapLookupKind::kName, 0x01C6,
            "VdIsHSIOTrainingSucceeded"};
  }
  if (xe::utf8::equal_case(export_name, "VdSetGraphicsInterruptCallback")) {
    return {GpuBootstrapLookupKind::kName, 0x01D5,
            "VdSetGraphicsInterruptCallback"};
  }
  if (xe::utf8::equal_case(export_name, "VdSwap")) {
    return {GpuBootstrapLookupKind::kName, 0x025B, "VdSwap"};
  }
  if (xe::utf8::equal_case(export_name, "VdInitializeEDRAM")) {
    return {GpuBootstrapLookupKind::kName, 0x0268, "VdInitializeEDRAM"};
  }
  if (xe::utf8::equal_case(export_name, "VdRetrainEDRAM")) {
    return {GpuBootstrapLookupKind::kName, 0x0269, "VdRetrainEDRAM"};
  }
  if (xe::utf8::equal_case(export_name, "VdRetrainEDRAMWorker")) {
    return {GpuBootstrapLookupKind::kName, 0x026A,
            "VdRetrainEDRAMWorker"};
  }
  return target;
}

const char* GetGpuBootstrapLookupKindName(GpuBootstrapLookupKind kind) {
  switch (kind) {
    case GpuBootstrapLookupKind::kOrdinal:
      return "ordinal";
    case GpuBootstrapLookupKind::kName:
      return "name";
    case GpuBootstrapLookupKind::kNone:
    default:
      return "none";
  }
}

void CountGpuBootstrapOrdinalLookup(uint32_t ordinal) {
  int ordinal_index = -1;
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] == ordinal) {
      ordinal_index = static_cast<int>(i);
      break;
    }
  }
  if (ordinal_index >= 0) {
    gpu_dynamic_lookup_call_counts[ordinal_index].fetch_add(
        1, std::memory_order_relaxed);
  }
  switch (ordinal) {
    case 0x01BC:
      gpu_proc_lookup_ordinal_01BC.fetch_add(1, std::memory_order_relaxed);
      break;
    case 0x01BF:
      gpu_proc_lookup_ordinal_01BF.fetch_add(1, std::memory_order_relaxed);
      break;
    case 0x0268:
      gpu_proc_lookup_ordinal_0268.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      break;
  }
}

void CountGpuBootstrapOrdinalLookupResolved(uint32_t ordinal) {
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] == ordinal) {
      gpu_dynamic_lookup_resolved_counts[i].fetch_add(
          1, std::memory_order_relaxed);
      return;
    }
  }
}

void CountGpuBootstrapOrdinalLookupMissing(uint32_t ordinal) {
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] == ordinal) {
      gpu_dynamic_lookup_missing_counts[i].fetch_add(
          1, std::memory_order_relaxed);
      return;
    }
  }
}

void CountGpuBootstrapOrdinalLookupInvalidModule(uint32_t ordinal) {
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] == ordinal) {
      gpu_dynamic_lookup_invalid_module_counts[i].fetch_add(
          1, std::memory_order_relaxed);
      return;
    }
  }
}

void CountGpuBootstrapOrdinalLookupFallbackHit(uint32_t ordinal) {
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] == ordinal) {
      gpu_dynamic_lookup_fallback_hit_counts[i].fetch_add(
          1, std::memory_order_relaxed);
      return;
    }
  }
}

}  // namespace

bool IsGpuBootstrapOrdinal(uint16_t ordinal) {
  for (uint16_t tracked : kGpuBootstrapOrdinals) {
    if (tracked == ordinal) {
      return true;
    }
  }
  return false;
}

const char* GetGpuBootstrapOrdinalName(uint16_t ordinal) {
  switch (ordinal) {
    case 0x01B6:
      return "VdEnableRingBufferRPtrWriteBack";
    case 0x01BC:
      return "VdGetGraphicsAsicID";
    case 0x01BD:
      return "VdGetSystemCommandBuffer";
    case 0x01BE:
      return "VdGlobalDevice";
    case 0x01BF:
      return "VdGlobalXamDevice";
    case 0x01C2:
      return "VdInitializeEngines";
    case 0x01C3:
      return "VdInitializeRingBuffer";
    case 0x01C6:
      return "VdIsHSIOTrainingSucceeded";
    case 0x01D5:
      return "VdSetGraphicsInterruptCallback";
    case 0x025B:
      return "VdSwap";
    case 0x0268:
      return "VdInitializeEDRAM";
    case 0x0269:
      return "VdRetrainEDRAM";
    case 0x026A:
      return "VdRetrainEDRAMWorker";
    default:
      return "<unknown>";
  }
}

void RecordGpuBootstrapStaticThunkCall(uint16_t ordinal, uint32_t pc,
                                       uint32_t lr, uint32_t thread_id) {
  if (ordinal == kXexCheckExecutablePrivilegeOrdinal) {
    uint64_t count =
        gpu_xex_check_executable_privilege_static_thunk_calls.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    if (count <= 5 || (count % 120) == 0) {
      XELOGW(
          "RING BUFFER: title pre-gpu static thunk call ordinal=0x{:03X} "
          "name=XexCheckExecutablePrivilege count={} pc={:08X} lr={:08X} "
          "thread_id={}",
          static_cast<uint32_t>(ordinal), count, pc, lr, thread_id);
    }
    return;
  }
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] != ordinal) {
      continue;
    }
    uint64_t count = gpu_static_thunk_call_counts[i].fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
    if (count <= 5 || (count % 120) == 0) {
      XELOGW(
          "RING BUFFER: static import thunk call ordinal=0x{:03X} name={} "
          "count={} pc={:08X} lr={:08X} thread_id={}",
          static_cast<uint32_t>(ordinal), GetGpuBootstrapOrdinalName(ordinal),
          count, pc, lr, thread_id);
    }
    return;
  }
}

void RecordGpuBootstrapGuestMainDispatchBridge(uint32_t address,
                                               uint32_t thread_id,
                                               const char* stage) {
  if (!address) {
    return;
  }
  uint64_t count =
      gpu_guest_main_dispatch_bridge_calls_total.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  if (count <= 5 || (count % 120) == 0) {
    XELOGW(
        "RING BUFFER: guest main dispatch entered stage={} count={} "
        "target={:08X} thread_id={}",
        stage ? stage : "<unknown>", count, address, thread_id);
  }
}

void RecordGpuBootstrapExportEntry(uint16_t ordinal, uint32_t pc, uint32_t lr,
                                   uint32_t thread_id) {
  if (ordinal == kXexCheckExecutablePrivilegeOrdinal) {
    uint64_t count =
        gpu_xex_check_executable_privilege_export_entry_calls.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    if (count <= 5 || (count % 120) == 0) {
      XELOGW(
          "RING BUFFER: title pre-gpu export entry call ordinal=0x{:03X} "
          "name=XexCheckExecutablePrivilege count={} pc={:08X} lr={:08X} "
          "thread_id={}",
          static_cast<uint32_t>(ordinal), count, pc, lr, thread_id);
    }
    return;
  }
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    if (kGpuBootstrapOrdinals[i] != ordinal) {
      continue;
    }
    uint64_t count = gpu_export_entry_call_counts[i].fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
    if (count <= 5 || (count % 120) == 0) {
      XELOGW(
          "RING BUFFER: export entry call ordinal=0x{:03X} name={} "
          "count={} pc={:08X} lr={:08X} thread_id={}",
          static_cast<uint32_t>(ordinal), GetGpuBootstrapOrdinalName(ordinal),
          count, pc, lr, thread_id);
    }
    return;
  }
}

dword_result_t XexCheckExecutablePrivilege_entry(dword_t privilege) {
  // BOOL
  // DWORD Privilege

  // Privilege is bit position in xe_xex2_system_flags enum - so:
  // Privilege=6 -> 0x00000040 -> XEX_SYSTEM_INSECURE_SOCKETS
  uint32_t privilege_value = privilege;
  uint32_t mask = privilege_value < 32 ? (1u << privilege_value) : 0u;

  auto module = kernel_state()->GetExecutableModule();
  uint32_t flags = 0;
  if (module) {
    module->GetOptHeader<uint32_t>(XEX_HEADER_SYSTEM_FLAGS, &flags);
  }
  uint32_t result = (module && (flags & mask) != 0) ? 1u : 0u;

  uint64_t export_entry_count =
      gpu_xex_check_executable_privilege_export_entry_calls.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  uint64_t call_count = gpu_xex_check_executable_privilege_calls.fetch_add(
                            1, std::memory_order_relaxed) +
                        1;
  if (result) {
    gpu_xex_check_executable_privilege_true.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    gpu_xex_check_executable_privilege_false.fetch_add(
        1, std::memory_order_relaxed);
  }
  gpu_xex_check_executable_privilege_last_privilege.store(
      privilege_value, std::memory_order_relaxed);
  gpu_xex_check_executable_privilege_last_flags.store(
      flags, std::memory_order_relaxed);
  gpu_xex_check_executable_privilege_last_result.store(
      result, std::memory_order_relaxed);
  if (call_count <= 5 || (call_count % 120) == 0 || !result) {
    XELOGW(
        "RING BUFFER: XexCheckExecutablePrivilege call privilege={} "
        "mask={:08X} flags={:08X} result={} executable_module={} "
        "call_count={} export_entry_count={} static_thunk_calls={}",
        privilege_value, mask, flags, result ? "ALLOW" : "DENY",
        module ? "YES" : "NO", call_count, export_entry_count,
        gpu_xex_check_executable_privilege_static_thunk_calls.load(
            std::memory_order_relaxed));
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexCheckExecutablePrivilege, kModules, kImplemented);

dword_result_t XexGetModuleHandle(std::string module_name,
                                  xe::be<uint32_t>* hmodule_ptr) {
  object_ref<XModule> module;
  loader_module_handle_calls_total.fetch_add(1, std::memory_order_relaxed);
  const bool gpu_module_lookup = IsXboxkrnlModuleName(module_name);
  if (gpu_module_lookup) {
    gpu_module_handle_calls.fetch_add(1, std::memory_order_relaxed);
  }

  if (module_name.empty()) {
    module = kernel_state()->GetExecutableModule();
  } else {
    module = kernel_state()->GetModule(module_name);
  }

  if (!module) {
    *hmodule_ptr = 0;
    if (gpu_module_lookup) {
      gpu_module_handle_failures.fetch_add(1, std::memory_order_relaxed);
      XELOGE(
          "RING BUFFER: XexGetModuleHandle failed module='{}' (status={:08X})",
          module_name, static_cast<uint32_t>(X_ERROR_NOT_FOUND));
    }
    return X_ERROR_NOT_FOUND;
  }

  // NOTE: we don't retain the handle for return.
  *hmodule_ptr = module->hmodule_ptr();
  if (gpu_module_lookup) {
    XELOGI(
        "RING BUFFER: XexGetModuleHandle resolved module='{}' hmodule={:08X} "
        "resolved_name='{}' resolved_path='{}'",
        module_name, static_cast<uint32_t>(*hmodule_ptr), module->name(),
        module->path());
  }

  return X_ERROR_SUCCESS;
}

dword_result_t XexGetModuleHandle_entry(lpstring_t module_name,
                                        lpdword_t hmodule_ptr) {
  return XexGetModuleHandle(module_name ? module_name.value() : "",
                            hmodule_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(XexGetModuleHandle, kModules, kImplemented);

dword_result_t XexGetModuleSection_entry(lpvoid_t hmodule, lpstring_t name,
                                         lpdword_t data_ptr,
                                         lpdword_t size_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (module) {
    uint32_t section_data = 0;
    uint32_t section_size = 0;
    result = module->GetSection(name.value(), &section_data, &section_size);
    if (XSUCCEEDED(result)) {
      *data_ptr = section_data;
      *size_ptr = section_size;
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexGetModuleSection, kModules, kImplemented);

dword_result_t xeXexLoadImage(
    lpstring_t module_name, dword_t module_flags, dword_t min_version,
    lpdword_t hmodule_ptr,
    const std::function<object_ref<UserModule>()>& load_callback,
    bool isFromMemory) {
  X_STATUS result = X_STATUS_NO_SUCH_FILE;

  if (!hmodule_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  uint32_t hmodule = 0;
  auto module = kernel_state()->GetModule(module_name.value());
  if (module) {
    if (isFromMemory) {
      // Existing module found; return error status.
      *hmodule_ptr = hmodule;
      return X_STATUS_OBJECT_NAME_COLLISION;
    } else {
      // Existing module found.
      hmodule = module->hmodule_ptr();
      result = X_STATUS_SUCCESS;
    }
  } else {
    // Not found; attempt to load as a user module.
    auto user_module = load_callback();
    if (user_module) {
      kernel_state()->ApplyTitleUpdate(user_module);
      kernel_state()->FinishLoadingUserModule(user_module);
      // Give up object ownership, this reference will be released by the last
      // XexUnloadImage call
      auto user_module_raw = user_module.release();
      hmodule = user_module_raw->hmodule_ptr();
      result = X_STATUS_SUCCESS;
    }
  }

  // Increment the module's load count.
  if (hmodule) {
    auto ldr_data =
        kernel_memory()->TranslateVirtual<X_LDR_DATA_TABLE_ENTRY*>(hmodule);
    ldr_data->load_count++;
  }

  *hmodule_ptr = hmodule;

  return result;
}

dword_result_t XexLoadImage_entry(lpstring_t module_name, dword_t module_flags,
                                  dword_t min_version, lpdword_t hmodule_ptr) {
  auto load_callback = [module_name] {
    return kernel_state()->LoadUserModule(module_name.value());
  };
  return xeXexLoadImage(module_name, module_flags, min_version, hmodule_ptr,
                        load_callback, false);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImage, kModules, kImplemented);

dword_result_t XexLoadImageFromMemory_entry(lpdword_t buffer, dword_t size,
                                            lpstring_t module_name,
                                            dword_t module_flags,
                                            dword_t min_version,
                                            lpdword_t hmodule_ptr) {
  auto load_callback = [module_name, buffer, size] {
    return kernel_state()->LoadUserModuleFromMemory(module_name.value(), buffer,
                                                    size);
  };
  return xeXexLoadImage(module_name, module_flags, min_version, hmodule_ptr,
                        load_callback, true);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImageFromMemory, kModules, kImplemented);

dword_result_t XexLoadExecutable_entry(lpstring_t module_name,
                                       dword_t module_flags,
                                       dword_t min_version,
                                       lpdword_t hmodule_ptr) {
  return XexLoadImage_entry(module_name, module_flags, min_version,
                            hmodule_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadExecutable, kModules, kSketchy);

dword_result_t XexUnloadImage_entry(lpvoid_t hmodule) {
  auto module = XModule::GetFromHModule(kernel_state(), hmodule);
  if (!module) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Can't unload kernel modules from user code.
  if (module->module_type() != XModule::ModuleType::kKernelModule) {
    auto ldr_data = hmodule.as<X_LDR_DATA_TABLE_ENTRY*>();
    if (--ldr_data->load_count == 0) {
      // No more references, free it.
      module->Release();
      kernel_state()->UnloadUserModule(object_ref<UserModule>(
          reinterpret_cast<UserModule*>(module.release())));
    }
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XexUnloadImage, kModules, kImplemented);

dword_result_t XexGetProcedureAddress_entry(lpvoid_t hmodule, dword_t ordinal,
                                            lpdword_t out_function_ptr) {
  // May be entry point?
  assert_not_zero(ordinal);
  loader_proc_lookup_calls_total.fetch_add(1, std::memory_order_relaxed);

  bool is_string_name = (ordinal & 0xFFFF0000) != 0;
  auto string_name = is_string_name
                         ? reinterpret_cast<const char*>(
                               kernel_memory()->TranslateVirtual(ordinal))
                         : nullptr;
  GpuBootstrapLookupTarget gpu_lookup =
      ResolveGpuBootstrapLookupTarget(ordinal, is_string_name, string_name);
  const bool gpu_bootstrap_lookup =
      gpu_lookup.kind != GpuBootstrapLookupKind::kNone;
  if (gpu_bootstrap_lookup) {
    gpu_proc_lookup_calls.fetch_add(1, std::memory_order_relaxed);
    CountGpuBootstrapOrdinalLookup(gpu_lookup.ordinal);
  }

  X_STATUS result = X_STATUS_INVALID_HANDLE;

  object_ref<XModule> module;
  const char* module_lookup_source = "hmodule";
  if (!hmodule) {
    module = kernel_state()->GetExecutableModule();
    module_lookup_source = "executable";
  } else {
    module = XModule::GetFromHModule(kernel_state(), hmodule);
  }

  auto resolve_proc_address = [&](XModule* lookup_module) -> uint32_t {
    if (!lookup_module) {
      return 0;
    }
    if (gpu_bootstrap_lookup) {
      switch (gpu_lookup.kind) {
        case GpuBootstrapLookupKind::kOrdinal:
          return lookup_module->GetProcAddressByOrdinal(gpu_lookup.ordinal);
        case GpuBootstrapLookupKind::kName:
          return lookup_module->GetProcAddressByName(gpu_lookup.name);
        case GpuBootstrapLookupKind::kNone:
        default:
          break;
      }
    }
    return is_string_name ? lookup_module->GetProcAddressByName(string_name)
                          : lookup_module->GetProcAddressByOrdinal(ordinal);
  };

  if (module) {
    uint32_t ptr = resolve_proc_address(module.get());
    bool used_xboxkrnl_fallback = false;
    if (!ptr && gpu_bootstrap_lookup) {
      auto xboxkrnl_module = kernel_state()->GetModule("xboxkrnl.exe");
      if (xboxkrnl_module) {
        uint32_t fallback_ptr = resolve_proc_address(xboxkrnl_module.get());
        if (fallback_ptr) {
          module = std::move(xboxkrnl_module);
          ptr = fallback_ptr;
          used_xboxkrnl_fallback = true;
          module_lookup_source = "xboxkrnl-fallback";
          gpu_proc_lookup_fallback_hits.fetch_add(1,
                                                  std::memory_order_relaxed);
          CountGpuBootstrapOrdinalLookupFallbackHit(gpu_lookup.ordinal);
        }
      }
    }
    if (ptr) {
      *out_function_ptr = ptr;
      result = X_STATUS_SUCCESS;
      if (gpu_bootstrap_lookup) {
        gpu_proc_lookup_resolved.fetch_add(1, std::memory_order_relaxed);
        CountGpuBootstrapOrdinalLookupResolved(gpu_lookup.ordinal);
      }
      if (gpu_bootstrap_lookup) {
        XELOGI(
            "RING BUFFER: XexGetProcedureAddress resolved ordinal=0x{:03X} "
            "name={} query_kind={} hmodule={:08X} module='{}' source={} "
            "ptr={:08X} fallback={}",
            static_cast<uint32_t>(gpu_lookup.ordinal), gpu_lookup.name,
            GetGpuBootstrapLookupKindName(gpu_lookup.kind),
            hmodule.guest_address(), module->name(), module_lookup_source, ptr,
            used_xboxkrnl_fallback ? "YES" : "NO");
      }
    } else {
      if (is_string_name) {
        XELOGW("ERROR: XexGetProcedureAddress export '{}' in '{}' not found!",
               string_name, module->name());
      } else {
        XELOGW(
            "ERROR: XexGetProcedureAddress ordinal {} (0x{:X}) in '{}' not "
            "found!",
            static_cast<uint32_t>(ordinal), static_cast<uint32_t>(ordinal),
            module->name());
      }
      if (gpu_bootstrap_lookup) {
        XELOGE(
            "RING BUFFER: XexGetProcedureAddress missing ordinal=0x{:03X} "
            "name={} query_kind={} hmodule={:08X} module='{}' source={}",
            static_cast<uint32_t>(gpu_lookup.ordinal), gpu_lookup.name,
            GetGpuBootstrapLookupKindName(gpu_lookup.kind),
            hmodule.guest_address(), module->name(), module_lookup_source);
      }
      *out_function_ptr = 0;
      result = X_STATUS_DRIVER_ENTRYPOINT_NOT_FOUND;
      if (gpu_bootstrap_lookup) {
        gpu_proc_lookup_missing.fetch_add(1, std::memory_order_relaxed);
        CountGpuBootstrapOrdinalLookupMissing(gpu_lookup.ordinal);
      }
    }
  } else if (gpu_bootstrap_lookup) {
    gpu_proc_lookup_invalid_module.fetch_add(1, std::memory_order_relaxed);
    CountGpuBootstrapOrdinalLookupInvalidModule(gpu_lookup.ordinal);
    XELOGE(
        "RING BUFFER: XexGetProcedureAddress invalid module handle "
        "for ordinal=0x{:03X} name={} query_kind={} hmodule={:08X}",
        static_cast<uint32_t>(gpu_lookup.ordinal), gpu_lookup.name,
        GetGpuBootstrapLookupKindName(gpu_lookup.kind), hmodule.guest_address());
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(XexGetProcedureAddress, kModules, kImplemented);

GpuBootstrapImportLookupStats GetGpuBootstrapImportLookupStats() {
  GpuBootstrapImportLookupStats stats;
  stats.loader_module_handle_calls_total =
      loader_module_handle_calls_total.load(std::memory_order_relaxed);
  stats.loader_proc_lookup_calls_total =
      loader_proc_lookup_calls_total.load(std::memory_order_relaxed);
  stats.guest_main_dispatch_bridge_calls_total =
      gpu_guest_main_dispatch_bridge_calls_total.load(
          std::memory_order_relaxed);
  stats.module_handle_calls =
      gpu_module_handle_calls.load(std::memory_order_relaxed);
  stats.module_handle_failures =
      gpu_module_handle_failures.load(std::memory_order_relaxed);
  stats.proc_lookup_calls =
      gpu_proc_lookup_calls.load(std::memory_order_relaxed);
  stats.proc_lookup_resolved =
      gpu_proc_lookup_resolved.load(std::memory_order_relaxed);
  stats.proc_lookup_missing =
      gpu_proc_lookup_missing.load(std::memory_order_relaxed);
  stats.proc_lookup_invalid_module =
      gpu_proc_lookup_invalid_module.load(std::memory_order_relaxed);
  stats.proc_lookup_fallback_hits =
      gpu_proc_lookup_fallback_hits.load(std::memory_order_relaxed);
  stats.ordinal_01BC_calls =
      gpu_proc_lookup_ordinal_01BC.load(std::memory_order_relaxed);
  stats.ordinal_01BF_calls =
      gpu_proc_lookup_ordinal_01BF.load(std::memory_order_relaxed);
  stats.ordinal_0268_calls =
      gpu_proc_lookup_ordinal_0268.load(std::memory_order_relaxed);
  stats.xex_check_executable_privilege_static_thunk_calls =
      gpu_xex_check_executable_privilege_static_thunk_calls.load(
          std::memory_order_relaxed);
  stats.xex_check_executable_privilege_export_entry_calls =
      gpu_xex_check_executable_privilege_export_entry_calls.load(
          std::memory_order_relaxed);
  stats.xex_check_executable_privilege_calls =
      gpu_xex_check_executable_privilege_calls.load(
          std::memory_order_relaxed);
  stats.xex_check_executable_privilege_true =
      gpu_xex_check_executable_privilege_true.load(std::memory_order_relaxed);
  stats.xex_check_executable_privilege_false =
      gpu_xex_check_executable_privilege_false.load(std::memory_order_relaxed);
  stats.xex_check_executable_privilege_last_privilege =
      gpu_xex_check_executable_privilege_last_privilege.load(
          std::memory_order_relaxed);
  stats.xex_check_executable_privilege_last_flags =
      gpu_xex_check_executable_privilege_last_flags.load(
          std::memory_order_relaxed);
  stats.xex_check_executable_privilege_last_result =
      gpu_xex_check_executable_privilege_last_result.load(
          std::memory_order_relaxed);
  for (size_t i = 0; i < kGpuBootstrapOrdinals.size(); ++i) {
    stats.ordinal_runtime_stats[i].ordinal = kGpuBootstrapOrdinals[i];
    stats.ordinal_runtime_stats[i].dynamic_lookup_calls =
        gpu_dynamic_lookup_call_counts[i].load(std::memory_order_relaxed);
    stats.ordinal_runtime_stats[i].dynamic_lookup_resolved_calls =
        gpu_dynamic_lookup_resolved_counts[i].load(std::memory_order_relaxed);
    stats.ordinal_runtime_stats[i].dynamic_lookup_missing_calls =
        gpu_dynamic_lookup_missing_counts[i].load(std::memory_order_relaxed);
    stats.ordinal_runtime_stats[i].dynamic_lookup_invalid_module_calls =
        gpu_dynamic_lookup_invalid_module_counts[i].load(
            std::memory_order_relaxed);
    stats.ordinal_runtime_stats[i].dynamic_lookup_fallback_hits =
        gpu_dynamic_lookup_fallback_hit_counts[i].load(
            std::memory_order_relaxed);
    stats.ordinal_runtime_stats[i].static_thunk_calls =
        gpu_static_thunk_call_counts[i].load(std::memory_order_relaxed);
    stats.ordinal_runtime_stats[i].export_entry_calls =
        gpu_export_entry_call_counts[i].load(std::memory_order_relaxed);
  }
  return stats;
}

void ExRegisterTitleTerminateNotification_entry(
    pointer_t<X_EX_TITLE_TERMINATE_REGISTRATION> reg, dword_t create) {
  if (create) {
    // Adding.
    kernel_state()->RegisterTitleTerminateNotification(
        reg->notification_routine, reg->priority);
  } else {
    // Removing.
    kernel_state()->RemoveTitleTerminateNotification(reg->notification_routine);
  }
}
DECLARE_XBOXKRNL_EXPORT1(ExRegisterTitleTerminateNotification, kModules,
                         kImplemented);
// todo: replace magic numbers
dword_result_t XexLoadImageHeaders_entry(pointer_t<X_ANSI_STRING> path,
                                         pointer_t<xex2_header> header,
                                         dword_t buffer_size,
                                         const ppc_context_t& ctx) {
  if (buffer_size < 0x800) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }
  auto current_kernel = ctx->kernel_state;
  auto target_path = util::TranslateAnsiPath(current_kernel->memory(), path);

  vfs::File* vfs_file = nullptr;
  vfs::FileAction file_action;
  X_STATUS result = current_kernel->file_system()->OpenFile(
      nullptr, target_path, vfs::FileDisposition::kOpen,
      vfs::FileAccess::kGenericRead, false, true, &vfs_file, &file_action);

  if (!vfs_file) {
    return result;
  }
  size_t bytes_read = 0;

  X_STATUS result_status = vfs_file->ReadSync(
      std::span<uint8_t>(reinterpret_cast<uint8_t*>(header.host_address()),
                         2048),
      0, &bytes_read);

  if (result_status < 0) {
    vfs_file->Destroy();
    return result_status;
  }

  if (header->magic != 'XEX2') {
    vfs_file->Destroy();
    return X_STATUS_INVALID_IMAGE_FORMAT;
  }
  unsigned int header_size = header->header_size;

  if (header_size < 0x800 || header_size > 0x10000 ||
      (header_size & 0x7FF) != 0) {
    result_status = X_STATUS_INVALID_IMAGE_FORMAT;
  } else if (header_size <= buffer_size) {
    if (header_size <= 0x800) {
      result_status = X_STATUS_SUCCESS;
    } else {
      result_status = vfs_file->ReadSync(
          std::span<uint8_t>(
              reinterpret_cast<uint8_t*>(header.host_address() + 2048),
              header_size - 2048),
          2048, &bytes_read);
      if (result_status >= X_STATUS_SUCCESS) {
        result_status = X_STATUS_SUCCESS;
      }
    }

  } else {
    result_status = X_STATUS_BUFFER_TOO_SMALL;
  }

  vfs_file->Destroy();
  return result_status;
}
DECLARE_XBOXKRNL_EXPORT1(XexLoadImageHeaders, kModules, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Module);
