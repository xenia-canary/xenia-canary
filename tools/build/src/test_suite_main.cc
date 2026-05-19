/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <locale>
#include <string>
#include <vector>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"

DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, false, "Enable cache mount", "Storage");
DEFINE_bool(mount_memory_unit, false, "Enable memory unit (MU) mount",
            "Storage");

// xenia-core's Emulator::SetupSubsystems references these cvars; provide
// stubs so test suites that pull in xenia-core link without xenia_main.cc.
DEFINE_string(apu, "nop", "Audio system stub for tests.", "APU");
DEFINE_string(gpu, "null", "Graphics system stub for tests.", "GPU");

#define CATCH_CONFIG_RUNNER
#include "third_party/catch/single_include/catch2/catch.hpp"

namespace xe {
namespace test_suite {

int test_suite_main(const std::vector<std::string>& args) {
  // Catch doesn't expose a way to pass a vector of strings, despite building a
  // vector internally.
  int argc = 0;
  std::vector<const char*> argv;
  for (const auto& arg : args) {
    argv.push_back(arg.c_str());
    argc++;
  }

  // Run Catch with compact reporter that shows dots
  Catch::Session session;

  // Use the compact reporter which shows dots for progress
  session.configData().reporterName = "compact";

  return session.run(argc, argv.data());
}

}  // namespace test_suite
}  // namespace xe

#ifndef XE_TEST_SUITE_NAME
#error XE_TEST_SUITE_NAME is undefined!
#endif

XE_DEFINE_CONSOLE_APP_TRANSPARENT(XE_TEST_SUITE_NAME,
                                  xe::test_suite::test_suite_main);
