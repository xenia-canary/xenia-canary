/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_mac.h"

int main(int argc, char** argv) {
  int result = 0;

  {
    xe::ui::MacWindowedAppContext app_context;

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    cvar::ParseLaunchArguments(argc, argv,
                               app->GetPositionalOptionsUsage(),
                               app->GetPositionalOptions());

    // Initialize logging. Needs parsed cvars.
    xe::InitializeLogging(app->GetName());

    if (app->OnInitialize()) {
      // Loop until the application requests a quit (e.g., from the emulator thread).
      while (!app_context.HasQuitFromUIThread()) {
        app_context.ExecutePendingFunctionsFromUIThread();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      app->InvokeOnDestroy();
    } else {
      result = EXIT_FAILURE;
    }
  }

  return result;
}
