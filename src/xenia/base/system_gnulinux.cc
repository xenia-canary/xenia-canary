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
#include <stdlib.h>

#include <cstring>
#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

#include <SDL3/SDL.h>

namespace xe {

void* LoadSDL3Library() {
  // The versioned soname is what the runtime package installs, the unversioned
  // name only exists with the development package.
  void* libsdl3 = dlopen("libSDL3.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!libsdl3) {
    libsdl3 = dlopen("libSDL3.so", RTLD_LAZY | RTLD_LOCAL);
  }
  return libsdl3;
}

namespace {
void OpenUrl(const std::string& url) {
  void* libsdl3 = LoadSDL3Library();
  if (!libsdl3) {
    XELOGE("Failed to open {}: {}", url, dlerror());
    return;
  }
  auto* pSDL_OpenURL =
      reinterpret_cast<decltype(SDL_OpenURL)*>(dlsym(libsdl3, "SDL_OpenURL"));
  auto* pSDL_GetError =
      reinterpret_cast<decltype(SDL_GetError)*>(dlsym(libsdl3, "SDL_GetError"));
  if (!pSDL_OpenURL) {
    XELOGE("Failed to open {}: SDL_OpenURL is unavailable", url);
  } else if (!pSDL_OpenURL(url.c_str())) {
    XELOGE("Failed to open {}: {}", url,
           pSDL_GetError ? pSDL_GetError() : "unknown error");
  }
  dlclose(libsdl3);
}
}  // namespace

void LaunchWebBrowser(const std::string_view url) { OpenUrl(std::string(url)); }

void LaunchFileExplorer(const std::filesystem::path& path) {
  std::error_code ec;
  OpenUrl(std::filesystem::absolute(path, ec).string());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  void* libsdl3 = LoadSDL3Library();
  assert_not_null(libsdl3);
  if (libsdl3) {
    auto* pSDL_ShowSimpleMessageBox =
        reinterpret_cast<decltype(SDL_ShowSimpleMessageBox)*>(
            dlsym(libsdl3, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      SDL_MessageBoxFlags flags;
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
    dlclose(libsdl3);
  }
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }
}  // namespace xe
