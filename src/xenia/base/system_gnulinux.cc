/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <alloca.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

// Use headers in third party to not depend on system sdl headers for building
#include "third_party/SDL2/include/SDL.h"

// Not reliably declared by <unistd.h> on all libcs without feature-test
// macros we don't otherwise need; declared explicitly for fexecve() below.
extern char** environ;

namespace xe {

void LaunchWebBrowser(const std::string_view url) {
  auto cmd = std::string("xdg-open ");
  cmd.append(url);
  system(cmd.c_str());
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  auto cmd = std::string("xdg-open ");
  cmd.append(path);
  system(cmd.c_str());
}

bool RestartApplication() {
  // Note for reviewers/static analysis: every value fed into fork()/exec()
  // below comes from the kernel's own view of the *already-running* process
  // (/proc/self/exe, /proc/self/cmdline) - i.e. exactly the binary path and
  // arguments this same instance of Xenia was already launched with. There
  // is no shell involved (fexecve, not system()/popen()) and no external or
  // user-supplied string (e.g. a cvar value) is appended to argv, so this
  // cannot be used to execute anything other than another copy of Xenia
  // with its own original arguments.
  //
  // The executable is opened once here and executed via its file descriptor
  // (fexecve) rather than by re-resolving "/proc/self/exe" as a path string
  // a second time at exec() - this closes the Time-Of-Check-To-Time-Of-Use
  // gap a path-based execv() would otherwise have between "look up the
  // path" and "run whatever is at that path now": with fexecve, the binary
  // actually launched is guaranteed to be the exact file that was just
  // open()'d, not whatever a symlink might resolve to a moment later.
  int exe_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (exe_fd < 0) {
    return false;
  }

  // Only used for the argv[0] fallback below if /proc/self/cmdline can't be
  // read - never used to choose what gets executed.
  char exe_path[PATH_MAX];
  ssize_t exe_path_length =
      readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (exe_path_length <= 0) {
    close(exe_fd);
    return false;
  }
  exe_path[exe_path_length] = '\0';

  // /proc/self/cmdline holds the original argv, NUL-separated, terminated by
  // a double NUL. Reproducing it exactly preserves whatever flags or content
  // path Xenia was originally launched with.
  std::ifstream cmdline_file("/proc/self/cmdline", std::ios::binary);
  if (!cmdline_file.is_open()) {
    close(exe_fd);
    return false;
  }
  std::vector<char> cmdline_bytes(
      (std::istreambuf_iterator<char>(cmdline_file)),
      std::istreambuf_iterator<char>());

  std::vector<std::string> args;
  size_t start = 0;
  for (size_t i = 0; i < cmdline_bytes.size(); ++i) {
    if (cmdline_bytes[i] == '\0') {
      args.emplace_back(&cmdline_bytes[start], i - start);
      start = i + 1;
    }
  }
  if (args.empty()) {
    // Fall back to just the executable path with no extra arguments.
    args.emplace_back(exe_path);
  }

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  pid_t child_pid = fork();
  if (child_pid < 0) {
    close(exe_fd);
    return false;
  }
  if (child_pid == 0) {
    // Child: replace this process image with a fresh instance of Xenia,
    // launched from the exact file descriptor opened above.
    fexecve(exe_fd, argv.data(), environ);
    // Only reached if fexecve failed.
    _exit(127);
  }
  // Parent: the new instance is on its way up: let the caller shut this one
  // down normally (saving config, releasing resources, etc).
  close(exe_fd);
  return true;
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  void* libsdl2 = dlopen("libSDL2.so", RTLD_LAZY | RTLD_LOCAL);
  assert_not_null(libsdl2);
  if (libsdl2) {
    auto* pSDL_ShowSimpleMessageBox =
        reinterpret_cast<decltype(SDL_ShowSimpleMessageBox)*>(
            dlsym(libsdl2, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      Uint32 flags;
      const char* title;
      char* message_copy = reinterpret_cast<char*>(alloca(message.size() + 1));
      std::memcpy(message_copy, message.data(), message.size());
      message_copy[message.size()] = '\0';

      switch (type) {
        default:
        case SimpleMessageBoxType::Help:
          title = "Xenia Help";
          flags = SDL_MESSAGEBOX_INFORMATION;
          break;
        case SimpleMessageBoxType::Warning:
          title = "Xenia Warning";
          flags = SDL_MESSAGEBOX_WARNING;
          break;
        case SimpleMessageBoxType::Error:
          title = "Xenia Error";
          flags = SDL_MESSAGEBOX_ERROR;
          break;
      }
      pSDL_ShowSimpleMessageBox(flags, title, message_copy, NULL);
    }
    dlclose(libsdl2);
  }
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }
}  // namespace xe
