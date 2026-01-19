/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include "gtk/gtk.h"
#else
#include <gtk/gtk.h>
#endif
#include <cstdio>
#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_gtk.h"

int main(int argc_pre_gtk, char** argv_pre_gtk) {
  // Before touching anything GTK+, make sure that when running on Wayland,
  // we'll still get an X11 (Xwayland) window
  // also allow users to override this
#ifndef __APPLE__
  if (!secure_getenv("GDK_BACKEND")) {
    setenv("GDK_BACKEND", "x11", 1);
  }
#endif

  // Initialize GTK+, which will handle and remove its own arguments from argv.
  // Both GTK+ and Xenia use --option=value argument format (see man
  // gtk-options), however, it's meaningless to try to parse the same argument
  // both as a GTK+ one and as a cvar. Make GTK+ options take precedence in case
  // of a name collision, as there's an alternative way of setting Xenia options
  // (the config).
  int argc_post_gtk = argc_pre_gtk;
  char** argv_post_gtk = argv_pre_gtk;
  if (!gtk_init_check(&argc_post_gtk, &argv_post_gtk)) {
    // Logging has not been initialized yet.
    std::fputs("Failed to initialize GTK+\n", stderr);
    return EXIT_FAILURE;
  }

#ifdef __APPLE__
  {
    fprintf(stderr,
            "[windowed_app_main_posix] Early masOS activation to avoid app "
            "stalling...\n");
    typedef void* id_val;
    typedef void* SEL_val;
    typedef id_val (*shared_app_fn)(id_val, SEL_val);
    typedef void (*set_policy_fn)(id_val, SEL_val, long);
    typedef void (*activate_fn)(id_val, SEL_val, signed char);

    id_val ns_app = reinterpret_cast<shared_app_fn>(objc_msgSend)(
        reinterpret_cast<id_val>(objc_getClass("NSApplication")),
        sel_registerName("sharedApplication"));
    if (ns_app) {
      reinterpret_cast<set_policy_fn>(objc_msgSend)(
          ns_app, sel_registerName("setActivationPolicy:"), 0L);
      reinterpret_cast<activate_fn>(objc_msgSend)(
          ns_app, sel_registerName("activateIgnoringOtherApps:"),
          static_cast<signed char>(1));
      fprintf(
          stderr,
          "[windowed_app_main_posix] Early macOS app activation complete.\n");
    }
  }
#endif

  int result;

  {
    xe::ui::GTKWindowedAppContext app_context;
#ifdef __APPLE__
    fprintf(stderr,
            "[windowed_app_main_posix] GTKWindowedAppContext created\n");
    fprintf(stderr,
            "[windowed_app_main_posix] Creating WindowedApp under "
            "GTKWindowedAppContext...\n");
#endif
    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);
#ifdef __APPLE__
    fprintf(stderr, "[windowed_app_main_posix] WindowedApp created: %s\n",
            app->GetName().c_str());
#endif

    cvar::ParseLaunchArguments(argc_post_gtk, argv_post_gtk,
                               app->GetPositionalOptionsUsage(),
                               app->GetPositionalOptions());

    // Initialize logging. Needs parsed cvars.
    xe::InitializeLogging(app->GetName());
#ifdef __APPLE__
    fprintf(stderr, "[windowed_app_main_posix] Logging initialized\n");
#endif

    if (app->OnInitialize()) {
      fprintf(stderr,
              "[windowed_app_main_posix] App initialized, entering "
              "RunMainGTKLoop...\n");
      app_context.RunMainGTKLoop();
      fprintf(stderr,
              "[windowed_app_main_posix] RunMainGTKLoop is accessible, now "
              "returning EXIT_SUCCESS.\n");
      result = EXIT_SUCCESS;
    } else {
      fprintf(stderr,
              "[windowed_app_main_posix] app pointer to OnInitialize() failed. "
              "Consider if you've broken this crucial pointer\n");
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  xe::ShutdownLogging();

  return result;
}
