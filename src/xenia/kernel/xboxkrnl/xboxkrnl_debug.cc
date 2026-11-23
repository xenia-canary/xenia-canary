/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_crash_dialog.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

void DbgBreakPoint_entry() { xe::debugging::Break(); }
DECLARE_XBOXKRNL_EXPORT2(DbgBreakPoint, kDebug, kStub, kImportant);

static const char* GetBugCheckDescription(uint32_t code) {
  switch (code) {
    case 0x0000001E:
      return "Kernel exception not handled";
    case 0x00000032:
      return "Initialization failed";
    case 0x00000035:
      return "No more IRP stack locations";
    case 0x00000044:
      return "IRP completed more than once";
    case 0x000000F2:
      return "Stack buffer overrun or fatal trap";
    case 0x000000F4:
      return "Heap check failed";
    case 0x000000F6:
      return "Devkit-only function called";
    default:
      return nullptr;
  }
}

void KeBugCheckEx_entry(dword_t code, dword_t param1, dword_t param2,
                        dword_t param3, dword_t param4) {
  const char* description = GetBugCheckDescription(code);

  auto msg =
      fmt::format("*** STOP: 0x{:08X} (0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X})",
                  static_cast<uint32_t>(code), static_cast<uint32_t>(param1),
                  static_cast<uint32_t>(param2), static_cast<uint32_t>(param3),
                  static_cast<uint32_t>(param4));

  if (description) {
    msg = fmt::format("{} - {}", msg, description);
  }

  XELOGE("{}", msg);
  FlushLog();

  if (xe::debugging::IsDebuggerAttached()) {
    xe::debugging::Break();
  }

  // Show crash dialog and suspend the guest thread instead of killing the
  // host process.
  auto current_thread = kernel::XThread::GetCurrentThread();

  xe::ui::CrashReport report;
  report.heading = "The game's kernel has crashed";
  report.description =
      "The game stopped itself with a bug check and the thread that raised "
      "it has been suspended. Attach the log when reporting this crash.";

  std::string stop_code = fmt::format("0x{:08X}", static_cast<uint32_t>(code));

  if (description) {
    stop_code = fmt::format("{} ({})", stop_code, description);
  }

  report.details.emplace_back("Stop code", stop_code);
  report.details.emplace_back(
      "Parameters",
      fmt::format("0x{:08X}, 0x{:08X}, 0x{:08X}, 0x{:08X}",
                  static_cast<uint32_t>(param1), static_cast<uint32_t>(param2),
                  static_cast<uint32_t>(param3),
                  static_cast<uint32_t>(param4)));

  if (current_thread) {
    report.details.emplace_back(
        "Thread",
        fmt::format("0x{:08X} (host 0x{:08X})", current_thread->thread_id(),
                    current_thread->thread()->system_id()));

    // Return address of the KeBugCheck call.
    const uint32_t caller =
        static_cast<uint32_t>(current_thread->thread_state()->context()->lr);
    report.details.emplace_back("Caller", fmt::format("0x{:08X}", caller));
  }

  kernel_state()->emulator()->ShowCrashDialog(std::move(report));

  if (current_thread) {
    current_thread->Suspend(nullptr);
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeBugCheckEx, kDebug, kStub, kImportant);

void KeBugCheck_entry(dword_t code) { KeBugCheckEx_entry(code, 0, 0, 0, 0); }
DECLARE_XBOXKRNL_EXPORT2(KeBugCheck, kDebug, kImplemented, kImportant);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Debug);
