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
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <cstring>
#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

// Use headers in third party to not depend on system sdl headers for building
#include "third_party/SDL2/include/SDL.h"

extern char** environ;

namespace xe {

namespace {
// No shell, so spaces and metacharacters in the target stay literal.
void LaunchXdgOpen(const std::string& target) {
  char xdg_open[] = "xdg-open";
  char* argv[] = {xdg_open, const_cast<char*>(target.c_str()), nullptr};
  pid_t pid;
  int error = posix_spawnp(&pid, xdg_open, nullptr, nullptr, argv, environ);
  if (error) {
    XELOGE("Failed to launch xdg-open for {}: {}", target,
           std::strerror(error));
    return;
  }
  // Wait like system() did, so the child doesn't linger as a zombie.
  waitpid(pid, nullptr, 0);
}
}  // namespace

void LaunchWebBrowser(const std::string_view url) {
  LaunchXdgOpen(std::string(url));
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  LaunchXdgOpen(path.string());
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
