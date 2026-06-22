/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xboxkrnl/cert_monitor.h"
#include "xenia/kernel/xboxkrnl/debug_monitor.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

DEFINE_string(cl, "", "Specify additional command-line provided to guest.",
              "Kernel");

DEFINE_bool(kernel_debug_monitor, false, "Enable debug monitor.", "Kernel");
DEFINE_bool(kernel_cert_monitor, false, "Enable cert monitor.", "Kernel");
DEFINE_bool(kernel_pix, false, "Enable PIX.", "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

bool XboxkrnlModule::SendPIXCommand(const char* cmd) {
  if (!cvars::kernel_pix) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();
  if (!XThread::IsInThread()) {
    assert_always();
    return false;
  }

  uint32_t scratch_size = 260 + 260;
  auto scratch_ptr = memory_->SystemHeapAlloc(scratch_size);
  auto scratch = memory_->TranslateVirtual(scratch_ptr);
  std::memset(scratch, 0, scratch_size);

  auto response = reinterpret_cast<char*>(scratch + 0);
  auto command = reinterpret_cast<char*>(scratch + 260);

  fmt::format_to_n(command, 259, "PIX!m!{}", cmd);

  global_lock.unlock();
  uint64_t args[] = {scratch_ptr + 260, scratch_ptr, 260};
  auto result = kernel_state_->processor()->Execute(
      XThread::GetCurrentThread()->thread_state(), pix_function_, args,
      xe::countof(args));
  global_lock.lock();

  XELOGD("PIX(command): {}", cmd);
  XELOGD("PIX(response): {}", response);

  memory_->SystemHeapFree(scratch_ptr);

  if (XSUCCEEDED(result)) {
    return true;
  }

  return false;
}

XboxkrnlModule::XboxkrnlModule(Emulator* emulator, KernelState* kernel_state)
    : KernelModule(kernel_state, "xe:\\xboxkrnl.exe") {
  // Allocate kernel memory for export global variables. Allocate memory from
  // 0x80070000 because guest trampoline for kernel, xam and xbdm are allocated
  // from 0x80040000.
  auto heap = memory()->LookupHeap(0x80070000);
  // Allocate remaining memory for kernel
  uint32_t kernel_range;
  if (!heap->AllocRange(
          0x80070000, 0x80280000, 0x200000, 0x1000, kMemoryAllocationCommit,
          kMemoryProtectRead | kMemoryProtectWrite, false, &kernel_range)) {
    XELOGW("XboxkrnlModule could not allocate kernel memory!");
  }

  RegisterExportTable(export_resolver_);

  // Register all exported functions.
#define XE_MODULE_EXPORT_GROUP(m, n) \
  Register##n##Exports(export_resolver_, kernel_state_);
#include "xboxkrnl_module_export_groups.inc"
#undef XE_MODULE_EXPORT_GROUP

  // KeDebugMonitorData (?*)
  // Set to a valid value when a remote debugger is attached.
  // Offset 0x18 is a 4b pointer to a handler function that seems to take two
  // arguments. If we wanted to see what would happen we could fake that.
  const uint32_t KeDebugMonitorData = 0x80207A64;
  if (!cvars::kernel_debug_monitor) {
    auto lpKeDebugMonitorData = memory_->TranslateVirtual(KeDebugMonitorData);
    xe::store_and_swap<uint32_t>(lpKeDebugMonitorData, 0);
  } else {
    uint32_t pKeDebugMonitorData =
        memory_->SystemHeapAlloc(sizeof(X_KEDEBUGMONITORDATA));
    xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(pKeDebugMonitorData),
                                 pKeDebugMonitorData);
    auto lpKeDebugMonitorData =
        memory_->TranslateVirtual<X_KEDEBUGMONITORDATA*>(pKeDebugMonitorData);
    std::memset(lpKeDebugMonitorData, 0, sizeof(X_KEDEBUGMONITORDATA));
    lpKeDebugMonitorData->callback_fn =
        GenerateTrampoline("KeDebugMonitorCallback", KeDebugMonitorCallback);
  }
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::KeDebugMonitorData, KeDebugMonitorData);

  // KeCertMonitorData (?*)
  // Always set to zero, ignored.
  const uint32_t KeCertMonitorData = 0x80207A60;
  if (!cvars::kernel_cert_monitor) {
    auto lpKeCertMonitorData = memory_->TranslateVirtual(KeCertMonitorData);
    xe::store_and_swap<uint32_t>(lpKeCertMonitorData, 0);
  } else {
    uint32_t pKeCertMonitorData =
        memory_->SystemHeapAlloc(sizeof(X_KECERTMONITORDATA));
    xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(pKeCertMonitorData),
                                 pKeCertMonitorData);
    auto lpKeCertMonitorData =
        memory_->TranslateVirtual<X_KECERTMONITORDATA*>(pKeCertMonitorData);
    std::memset(lpKeCertMonitorData, 0, sizeof(X_KECERTMONITORDATA));
    lpKeCertMonitorData->callback_fn =
        GenerateTrampoline("KeCertMonitorCallback", KeCertMonitorCallback);
  }
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::KeCertMonitorData, KeCertMonitorData);

  // XboxHardwareInfo (XboxHardwareInfo_t, 16b)
  // flags       cpu#  ?     ?     ?     ?           ?       ?
  // 0x00000000, 0x06, 0x00, 0x00, 0x00, 0x00000000, 0x0000, 0x0000
  // Games seem to check if bit 26 (0x20) is set, which at least for xbox1
  // was whether an HDD was present. Not sure what the other flags are.
  //
  // aomega08 says the value is 0x02000817, bit 27: debug mode on.
  // When that is set, though, allocs crash in weird ways.
  //
  // From kernel dissasembly, after storage is initialized
  // XboxHardwareInfo flags is set with flag 5 (0x20).
  const uint32_t XboxHardwareInfo = 0x801D0030;
  auto lpXboxHardwareInfo = memory_->TranslateVirtual(XboxHardwareInfo);
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::XboxHardwareInfo, XboxHardwareInfo);
  xe::store_and_swap<uint32_t>(lpXboxHardwareInfo + 0, 0x20);  // flags
  xe::store_and_swap<uint8_t>(lpXboxHardwareInfo + 4, 0x06);   // cpu count
  // Remaining 11b are zeroes?

  // ExConsoleGameRegion, probably same values as keyvault region uses?
  // Just return all 0xFF, should satisfy anything that checks it
  const uint32_t ExConsoleGameRegion = 0x80205150;
  auto lpExConsoleGameRegion = memory_->TranslateVirtual(ExConsoleGameRegion);
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::ExConsoleGameRegion, ExConsoleGameRegion);
  xe::store<uint16_t>(lpExConsoleGameRegion, 0xFFFF);

  // XexExecutableModuleHandle (?**)
  // Games try to dereference this to get a pointer to some module struct.
  // So far it seems like it's just in loader code, and only used to look up
  // the XexHeaderBase for use by RtlImageXexHeaderField.
  // We fake it so that the address passed to that looks legit.
  // 0x80100FFC <- pointer to structure
  // 0x80101000 <- our module structure
  // 0x80101058 <- pointer to xex header
  // 0x80101100 <- xex header base
  const uint32_t XexExecutableModuleHandle = 0x802080D4;
  export_resolver_->SetVariableMapping("xboxkrnl.exe",
                                       ordinals::XexExecutableModuleHandle,
                                       XexExecutableModuleHandle);

  // ExLoadedImageName (char*)
  // The full path to loaded image/xex including its name.
  // Used usually in custom dashboards (Aurora)
  uint32_t ExLoadedImageName = 0x80207E60;
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::ExLoadedImageName, ExLoadedImageName);

  // ExLoadedCommandLine (char*)
  // The name of the xex. Not sure this is ever really used on real devices.
  // Perhaps it's how swap disc/etc data is sent?
  // Always set to "default.xex" (with quotes) for now.
  // TODO(gibbed): set this to the actual module name.
  std::string command_line("\"default.xex\"");
  if (cvars::cl.length()) {
    command_line += " " + cvars::cl;
  }
  const uint32_t command_line_length =
      xe::align(static_cast<uint32_t>(command_line.length()) + 1,
                static_cast<uint32_t>(kExLoadedCommandLineSize));
  const uint32_t ExLoadedCommandLine = 0x80207C60;
  auto lpExLoadedCommandLine = memory_->TranslateVirtual(ExLoadedCommandLine);
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::ExLoadedCommandLine, ExLoadedCommandLine);
  std::memset(lpExLoadedCommandLine, 0, command_line_length);
  std::memcpy(lpExLoadedCommandLine, command_line.c_str(),
              command_line.length());

  // XboxKrnlVersion (8b)
  // Kernel version, looks like 2b.2b.2b.2b.
  // I've only seen games check >=, so we just fake something here.
  const uint32_t XboxKrnlVersion = 0x8004045C;
  auto lpXboxKrnlVersion = memory_->TranslateVirtual(XboxKrnlVersion);
  export_resolver_->SetVariableMapping(
      "xboxkrnl.exe", ordinals::XboxKrnlVersion, XboxKrnlVersion);
  std::memcpy(lpXboxKrnlVersion, kernel_state_->GetKernelVersion(),
              sizeof(KernelVersion));

  export_resolver_->SetVariableMapping("xboxkrnl.exe",
                                       ordinals::KeTimeStampBundle,
                                       kernel_state->GetKeTimestampBundle());
#define EXPORT_KVAR(typ)                                                       \
  export_resolver_->SetVariableMapping("xboxkrnl.exe", ordinals::typ,          \
                                       kernel_state->GetKernelGuestGlobals() + \
                                           offsetof(KernelGuestGlobals, typ))

  EXPORT_KVAR(ExThreadObjectType);
  EXPORT_KVAR(ExEventObjectType);
  EXPORT_KVAR(ExMutantObjectType);
  EXPORT_KVAR(ExSemaphoreObjectType);
  EXPORT_KVAR(ExTimerObjectType);
  EXPORT_KVAR(IoCompletionObjectType);
  EXPORT_KVAR(IoDeviceObjectType);
  EXPORT_KVAR(IoFileObjectType);
  EXPORT_KVAR(ObDirectoryObjectType);
  EXPORT_KVAR(ObSymbolicLinkObjectType);
  EXPORT_KVAR(UsbdBootEnumerationDoneEvent);
#undef EXPORT_KVAR
}

static auto& get_xboxkrnl_exports() {
  static std::vector<xe::cpu::Export*> xboxkrnl_exports(4096);
  return xboxkrnl_exports;
}

xe::cpu::Export* RegisterExport_xboxkrnl(xe::cpu::Export* export_entry) {
  auto& xboxkrnl_exports = get_xboxkrnl_exports();
  assert_true(export_entry->ordinal < xboxkrnl_exports.size());
  xboxkrnl_exports[export_entry->ordinal] = export_entry;
  return export_entry;
}

void XboxkrnlModule::RegisterExportTable(
    xe::cpu::ExportResolver* export_resolver) {
  assert_not_null(export_resolver);

// Build the export table used for resolution.
#include "xenia/kernel/util/export_table_pre.inc"
  static xe::cpu::Export xboxkrnl_export_table[] = {
#include "xenia/kernel/xboxkrnl/xboxkrnl_table.inc"
  };
#include "xenia/kernel/util/export_table_post.inc"
  auto& xboxkrnl_exports = get_xboxkrnl_exports();
  for (size_t i = 0; i < xe::countof(xboxkrnl_export_table); ++i) {
    auto& export_entry = xboxkrnl_export_table[i];
    assert_true(export_entry.ordinal < xboxkrnl_exports.size());
    if (!xboxkrnl_exports[export_entry.ordinal]) {
      xboxkrnl_exports[export_entry.ordinal] = &export_entry;
    }
  }
  export_resolver->RegisterTable("xboxkrnl.exe", &xboxkrnl_exports);
}

XboxkrnlModule::~XboxkrnlModule() = default;

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
