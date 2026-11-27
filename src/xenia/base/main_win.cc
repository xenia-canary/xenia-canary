/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

#include "version.h"

// For RequestWin32MMCSS.
#include <dwmapi.h>
// For RequestWin32HighResolutionTimer.
#include <winternl.h>

DEFINE_bool(win32_high_resolution_timer, true,
            "Requests high-resolution timer from the NT kernel", "Win32");
DEFINE_bool(
    win32_mmcss, true,
    "Opt in the Multimedia Class Scheduler Service (MMCSS) scheduling for "
    "prioritized access to CPU resources",
    "Win32");

namespace xe {

namespace {

struct Win32TimerStatus {
  bool throttling_opt_out_skipped = false;
  DWORD throttling_opt_out_error = ERROR_SUCCESS;
  ULONG resolution = 0;
};

Win32TimerStatus timer_status;
bool high_performance_requested = false;

}  // namespace

static bool IsBeforeWindows11(HMODULE ntdll_module) {
  constexpr DWORD kWindows11BuildNumber = 22000;

  // clang-format off
  NTSTATUS (NTAPI* rtl_get_version)(OUT PRTL_OSVERSIONINFOW VersionInformation);
  // clang-format on
  rtl_get_version = reinterpret_cast<decltype(rtl_get_version)>(
      GetProcAddress(ntdll_module, "RtlGetVersion"));

  if (!rtl_get_version) {
    return false;
  }

  RTL_OSVERSIONINFOW version_info = {};
  version_info.dwOSVersionInfoSize = sizeof(version_info);

  if (rtl_get_version(&version_info) < 0) {
    return false;
  }

  return version_info.dwBuildNumber < kWindows11BuildNumber;
}

static DWORD OptOutOfWin32TimerResolutionThrottling() {
  HMODULE kernel32_module = GetModuleHandleW(L"kernel32.dll");

  if (!kernel32_module) {
    return ERROR_SUCCESS;
  }

  // clang-format off
  BOOL (WINAPI* set_process_information)(
      HANDLE hProcess, PROCESS_INFORMATION_CLASS ProcessInformationClass,
      LPVOID ProcessInformation, DWORD ProcessInformationSize);
  // clang-format on
  set_process_information = reinterpret_cast<decltype(set_process_information)>(
      GetProcAddress(kernel32_module, "SetProcessInformation"));

  if (!set_process_information) {
    return ERROR_SUCCESS;
  }

  // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, missing from older SDKs
  constexpr ULONG kIgnoreTimerResolution = 0x4;

  PROCESS_POWER_THROTTLING_STATE throttling_state = {};
  throttling_state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling_state.ControlMask = kIgnoreTimerResolution;
  throttling_state.StateMask = 0;

  if (!set_process_information(GetCurrentProcess(), ProcessPowerThrottling,
                               &throttling_state, sizeof(throttling_state))) {
    return GetLastError();
  }

  return ERROR_SUCCESS;
}

static Win32TimerStatus RequestWin32HighResolutionTimer() {
  Win32TimerStatus status;

  HMODULE ntdll_module = GetModuleHandleW(L"ntdll.dll");

  if (!ntdll_module) {
    return status;
  }

  // clang-format off
  NTSTATUS (NTAPI* nt_query_timer_resolution)(OUT PULONG MinimumResolution,
                                              OUT PULONG MaximumResolution,
                                              OUT PULONG CurrentResolution);
  NTSTATUS (NTAPI* nt_set_timer_resolution)(IN ULONG DesiredResolution,
                                            IN BOOLEAN SetResolution,
                                            OUT PULONG CurrentResolution);
  // clang-format on
  nt_query_timer_resolution =
      reinterpret_cast<decltype(nt_query_timer_resolution)>(
          GetProcAddress(ntdll_module, "NtQueryTimerResolution"));
  nt_set_timer_resolution = reinterpret_cast<decltype(nt_set_timer_resolution)>(
      GetProcAddress(ntdll_module, "NtSetTimerResolution"));

  if (!nt_query_timer_resolution || !nt_set_timer_resolution) {
    return status;
  }

  // Windows 11 may ignore the resolution request while the window is occluded
  // or minimized, falling back to the 15.625 ms tick, which halves the speed
  // of titles paced by a 16 ms timer. Opt out of that throttling.
  if (IsBeforeWindows11(ntdll_module)) {
    status.throttling_opt_out_skipped = true;
  } else {
    status.throttling_opt_out_error = OptOutOfWin32TimerResolutionThrottling();
  }

  ULONG minimum_resolution, maximum_resolution, current_resolution;
  nt_query_timer_resolution(&minimum_resolution, &maximum_resolution,
                            &current_resolution);
  nt_set_timer_resolution(maximum_resolution, true, &current_resolution);

  status.resolution = current_resolution;

  return status;
}

static void RequestWin32MMCSS() {
  HMODULE dwmapi_module = LoadLibraryW(L"dwmapi.dll");
  if (!dwmapi_module) {
    return;
  }
  // clang-format off
  HRESULT (STDAPICALLTYPE* dwm_enable_mmcss)(BOOL fEnableMMCSS);
  // clang-format on
  dwm_enable_mmcss = reinterpret_cast<decltype(dwm_enable_mmcss)>(
      GetProcAddress(dwmapi_module, "DwmEnableMMCSS"));
  if (dwm_enable_mmcss) {
    dwm_enable_mmcss(true);
  }
  FreeLibrary(dwmapi_module);
}

bool ParseWin32LaunchArguments(
    bool transparent_options, const std::string_view positional_usage,
    const std::vector<std::string>& positional_options,
    std::vector<std::string>* args_out) {
  auto command_line = GetCommandLineW();

  int wargc;
  wchar_t** wargv = CommandLineToArgvW(command_line, &wargc);
  if (!wargv) {
    return false;
  }

  // Convert all args to narrow, as cxxopts doesn't support wchar.
  // Use WideCharToMultiByte with CP_UTF8 to support non-ANSI characters
  // (e.g. CJK paths), unlike locale-dependent wcstombs.
  int argc = wargc;
  char** argv = reinterpret_cast<char**>(alloca(sizeof(char*) * argc));
  for (int n = 0; n < argc; n++) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wargv[n], -1, nullptr, 0, nullptr,
                                  nullptr);
    argv[n] = reinterpret_cast<char*>(alloca(sizeof(char) * len));
    WideCharToMultiByte(CP_UTF8, 0, wargv[n], -1, argv[n], len, nullptr,
                        nullptr);
  }

  LocalFree(wargv);

  if (!transparent_options) {
    cvar::ParseLaunchArguments(argc, argv, positional_usage,
                               positional_options);
  }

  if (args_out) {
    args_out->clear();
    for (int n = 0; n < argc; n++) {
      args_out->push_back(std::string(argv[n]));
    }
  }

  return true;
}

int InitializeWin32App(const std::string_view app_name) {
  // Initialize logging. Needs parsed FLAGS.
  xe::InitializeLogging(app_name);

  // Print version info.
  XELOGI(
      "Build: "
#ifdef XE_BUILD_IS_PR
      "PR#" XE_BUILD_PR_NUMBER " - "
#endif
      XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE);

  return 0;
}

void RequestWin32HighPerformance() {
  // Apps loading a config file call this once it is loaded, so the cvars set
  // in it are respected. Other apps get it after their initialization.
  if (high_performance_requested) {
    return;
  }
  high_performance_requested = true;

  if (cvars::win32_high_resolution_timer) {
    timer_status = RequestWin32HighResolutionTimer();
  }
  if (cvars::win32_mmcss) {
    RequestWin32MMCSS();
  }
}

void LogWin32StartupStatus() {
  if (!cvars::win32_high_resolution_timer) {
    XELOGI(
        "Host timer resolution left unchanged (win32_high_resolution_timer "
        "is disabled)");
  }
  if (!cvars::win32_mmcss) {
    XELOGI("MMCSS scheduling not requested (win32_mmcss is disabled)");
  }

  if (timer_status.throttling_opt_out_skipped) {
    XELOGI("Timer resolution throttling opt-out not needed before Windows 11");
  } else if (timer_status.throttling_opt_out_error != ERROR_SUCCESS) {
    XELOGW(
        "Failed to opt out of timer resolution throttling (error {}); "
        "guest timers may run slow while the window is not visible",
        timer_status.throttling_opt_out_error);
  }

  if (timer_status.resolution) {
    XELOGI("Host timer resolution set to {} x 100 ns", timer_status.resolution);
  }
}

void ShutdownWin32App() { xe::ShutdownLogging(); }

}  // namespace xe
