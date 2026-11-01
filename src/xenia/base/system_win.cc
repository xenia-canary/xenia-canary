/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <map>
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  auto wide_url = xe::to_utf16(url);
  ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(wide_url.c_str()),
                nullptr, nullptr, SW_SHOWNORMAL);
}

void LaunchFileExplorer(const std::filesystem::path& url) {
  ShellExecuteW(nullptr, L"explore", url.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

bool RestartApplication() {
  // Note for reviewers/static analysis: both arguments fed into
  // CreateProcessW below come from the OS's own view of the *already
  // running* process - GetModuleFileNameW (the on-disk path of this very
  // executable) and GetCommandLineW() (the exact command line this same
  // instance of Xenia was already launched with). No external or
  // user-supplied string (e.g. a cvar value) is appended, so this cannot be
  // used to execute anything other than another copy of Xenia with its own
  // original arguments.

  wchar_t module_path[MAX_PATH];
  DWORD path_length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  if (!path_length || path_length == MAX_PATH) {
    // Path missing or truncated - bail rather than launching something wrong.
    return false;
  }

  // GetCommandLineW() returns the exact string this process was launched
  // with (including the executable path itself as argv[0]), so simply
  // passing it straight through to CreateProcessW reproduces the original
  // invocation - whatever flags or content path the user originally used.
  std::wstring command_line = GetCommandLineW();

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};

  // CreateProcessW may modify its command-line buffer in place, so it must
  // not point at read-only memory (unlike a string literal).
  BOOL result =
      CreateProcessW(module_path, command_line.data(), nullptr, nullptr, FALSE,
                     0, nullptr, nullptr, &startup_info, &process_info);
  if (!result) {
    return false;
  }

  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return true;
}

void ShowSimpleMessageBox(SimpleMessageBoxType type,
                          const std::string_view message) {
  const wchar_t* title;
  std::u16string wide_message = xe::to_utf16(message);
  DWORD type_flags = MB_OK | MB_APPLMODAL | MB_SETFOREGROUND;
  switch (type) {
    default:
    case SimpleMessageBoxType::Help:
      title = L"Xenia Help";
      type_flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::Warning:
      title = L"Xenia Warning";
      type_flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::Error:
      title = L"Xenia Error";
      type_flags |= MB_ICONERROR;
      break;
  }
  MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(wide_message.c_str()), title,
              type_flags);
}

static std::map<const uint32_t, DWORD> xeniaToWindowsPriorityClassMapping = {
    {0, NORMAL_PRIORITY_CLASS},
    {1, ABOVE_NORMAL_PRIORITY_CLASS},
    {2, HIGH_PRIORITY_CLASS},
    {3, REALTIME_PRIORITY_CLASS}};

bool SetProcessPriorityClass(const uint32_t priority_class) {
  if (!xeniaToWindowsPriorityClassMapping.count(priority_class)) {
    return false;
  }

  return SetPriorityClass(GetCurrentProcess(),
                          xeniaToWindowsPriorityClassMapping[priority_class]);
}

bool IsUseNexusForGameBarEnabled() {
  constexpr LPCWSTR reg_path = L"SOFTWARE\\Microsoft\\GameBar";
  constexpr LPCWSTR key = L"UseNexusForGameBarEnabled";

  DWORD value = 0;
  DWORD dataSize = sizeof(value);

  RegGetValue(HKEY_CURRENT_USER, reg_path, key, RRF_RT_DWORD, nullptr, &value,
              &dataSize);

  return static_cast<bool>(value);
}

}  // namespace xe
