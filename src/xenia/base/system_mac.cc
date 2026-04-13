/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cstring>
#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/system.h"

// Use headers in third party to not depend on system sdl headers for building
#include "third_party/SDL2/include/SDL.h"

extern char** environ;

namespace xe {

namespace {

// Spawns /usr/bin/open with a single argument, bypassing the shell so URLs
// and paths containing metacharacters cannot be reinterpreted.
void SpawnOpen(const char* arg) {
  const char* program = "/usr/bin/open";
  const char* argv[] = {program, arg, nullptr};
  pid_t pid;
  if (posix_spawn(&pid, program, nullptr, nullptr, const_cast<char**>(argv),
                  environ) == 0) {
    int status;
    waitpid(pid, &status, 0);
  }
}

}  // namespace

void LaunchWebBrowser(const std::string_view url) {
  std::string url_str(url);
  SpawnOpen(url_str.c_str());
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  SpawnOpen(path.string().c_str());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  void* libsdl2 = dlopen("libSDL2.dylib", RTLD_LAZY | RTLD_LOCAL);
  if (!libsdl2) {
    libsdl2 = dlopen("libSDL2-2.0.0.dylib", RTLD_LAZY | RTLD_LOCAL);
  }
  assert_not_null(libsdl2);
  if (libsdl2) {
    auto* pSDL_ShowSimpleMessageBox =
        reinterpret_cast<decltype(SDL_ShowSimpleMessageBox)*>(
            dlsym(libsdl2, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      Uint32 flags;
      const char* title;
      std::string message_copy(message);

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
      pSDL_ShowSimpleMessageBox(flags, title, message_copy.c_str(), nullptr);
    }
    dlclose(libsdl2);
  }
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
