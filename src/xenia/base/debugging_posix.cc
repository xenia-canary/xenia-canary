/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/debugging.h"

#include <csignal>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"

#if XE_PLATFORM_MAC
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#endif  // XE_PLATFORM_MAC

namespace xe {
namespace debugging {

bool IsDebuggerAttached() {
#if XE_PLATFORM_MAC
  // https://developer.apple.com/library/archive/qa/qa1361/_index.html
  kinfo_proc info = {};
  int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  size_t size = sizeof(info);
  if (sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, nullptr, 0) != 0) {
    return false;
  }
  return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
  std::ifstream proc_status_stream("/proc/self/status");
  if (!proc_status_stream.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(proc_status_stream, line)) {
    std::istringstream line_stream(line);
    std::string key;
    line_stream >> key;
    if (key == "TracerPid:") {
      uint32_t tracer_pid;
      line_stream >> tracer_pid;
      return tracer_pid != 0;
    }
  }
  return false;
#endif  // XE_PLATFORM_MAC
}

void Break() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    // Install handler for sigtrap only once
    std::signal(SIGTRAP, [](int) {
      // Forward signal to default handler after being caught
      std::signal(SIGTRAP, SIG_DFL);
    });
  });
  std::raise(SIGTRAP);
}

namespace internal {
void DebugPrint(const char* s) { std::clog << s << std::endl; }
}  // namespace internal

}  // namespace debugging
}  // namespace xe
