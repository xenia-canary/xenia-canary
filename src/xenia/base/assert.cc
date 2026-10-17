/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/assert.h"

#if !defined(NDEBUG) && !XE_PLATFORM_WIN32

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iterator>
#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"

#if XE_PLATFORM_GNU_LINUX
#include <dlfcn.h>

// Use headers in third party to not depend on system sdl headers for building
#include "third_party/SDL2/include/SDL.h"
#endif

namespace xe {
namespace {

enum class AssertAction : int { kAbort, kIgnore, kIgnoreAll };

std::atomic<bool> ignore_all_asserts = false;
// Guards against assertions failing while reporting one.
thread_local bool in_assert = false;

#if XE_PLATFORM_GNU_LINUX
// Equivalent of the MSVC debug CRT assertion dialog. SDL is loaded dynamically
// like in ShowSimpleMessageBox, as xenia-base doesn't link it.
AssertAction ShowAssertDialog(const std::string& message) {
  void* libsdl2 = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!libsdl2) {
    libsdl2 = dlopen("libSDL2.so", RTLD_LAZY | RTLD_LOCAL);
  }
  if (!libsdl2) {
    return AssertAction::kIgnore;
  }

  AssertAction action = AssertAction::kIgnore;
  auto* pSDL_ShowMessageBox = reinterpret_cast<decltype(SDL_ShowMessageBox)*>(
      dlsym(libsdl2, "SDL_ShowMessageBox"));
  if (pSDL_ShowMessageBox) {
    const SDL_MessageBoxButtonData buttons[] = {
        {0, static_cast<int>(AssertAction::kAbort), "Abort"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT |
             SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
         static_cast<int>(AssertAction::kIgnore), "Ignore"},
        {0, static_cast<int>(AssertAction::kIgnoreAll), "Ignore All"},
    };
    const SDL_MessageBoxData data = {SDL_MESSAGEBOX_ERROR,
                                     nullptr,
                                     "Xenia Assertion Failed",
                                     message.c_str(),
                                     static_cast<int>(std::size(buttons)),
                                     buttons,
                                     nullptr};
    int button_id = static_cast<int>(AssertAction::kIgnore);
    if (pSDL_ShowMessageBox(&data, &button_id) == 0 && button_id >= 0) {
      action = static_cast<AssertAction>(button_id);
    }
  }
  dlclose(libsdl2);
  return action;
}
#else
AssertAction ShowAssertDialog([[maybe_unused]] const std::string& message) {
  return AssertAction::kIgnore;
}
#endif

}  // namespace

void AssertFailed(const char* expression, const char* file, int line) {
  if (in_assert) {
    return;
  }
  in_assert = true;

  XELOGE("Assertion failed: {} ({}:{})", expression, file, line);
  FlushLog();

  if (debugging::IsDebuggerAttached()) {
    raise(SIGTRAP);
  } else if (!ignore_all_asserts) {
    // Without a debugger SIGTRAP terminates the process, ask instead like the
    // assertion dialog on Windows.
    const std::string message = fmt::format(
        "Assertion failed!\n\nFile: {}\nLine: {}\n\nExpression: {}\n\n"
        "Abort closes Xenia, Ignore continues execution.",
        file, line, expression);
    switch (ShowAssertDialog(message)) {
      case AssertAction::kAbort:
        std::abort();
      case AssertAction::kIgnoreAll:
        ignore_all_asserts = true;
        break;
      case AssertAction::kIgnore:
        break;
    }
  }

  in_assert = false;
}

}  // namespace xe

#endif  // !defined(NDEBUG) && !XE_PLATFORM_WIN32
