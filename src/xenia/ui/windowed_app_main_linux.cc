/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <signal.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

#include <wx/wx.h>

#include "xenia/base/platform.h"
#include "xenia/ui/windowed_app_wx.h"

// XeniaWxApp owns OnInit / OnExit. Cvar/logging init and command-line parsing
// happen in XeniaWxApp::OnInit after the WindowedApp is constructed and can
// supply its positional options.
wxIMPLEMENT_APP_NO_MAIN(xe::ui::XeniaWxApp);

int main(int argc, char** argv) {
  // If spawned by an outgoing xenia process, wait for it to exit before we
  // open our own window so the compositor doesn't briefly show both.
  if (const char* wait_pid_str = secure_getenv("XENIA_WAIT_FOR_PID")) {
    pid_t wait_pid = static_cast<pid_t>(std::atoi(wait_pid_str));
    if (wait_pid > 0) {
      for (int waited_us = 0; waited_us < 5'000'000; waited_us += 20'000) {
        if (kill(wait_pid, 0) != 0) {
          break;
        }
        usleep(20'000);
      }
    }
    unsetenv("XENIA_WAIT_FOR_PID");
  }

  // Force GTK onto the X11 backend by default — under XWayland the X11 path
  // is more stable for our render-surface lifecycle than the native Wayland
  // one. Respect an explicit user override.
  if (!secure_getenv("GDK_BACKEND")) {
    setenv("GDK_BACKEND", "x11", 1);
  }

  xe::ui::g_argc = argc;
  xe::ui::g_argv = argv;

  return wxEntry(argc, argv);
}
