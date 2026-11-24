/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/entry_table.h"

#include <chrono>
#include <future>

#include "third_party/catch/include/catch.hpp"

TEST_CASE("EntryTable wakes concurrent compilation waiters", "[cpu]") {
  using namespace std::chrono_literals;

  xe::cpu::EntryTable table;
  xe::cpu::Entry* compiling_entry = nullptr;
  REQUIRE(table.GetOrCreate(0x1000, &compiling_entry) ==
          xe::cpu::Entry::STATUS_NEW);

  std::promise<void> started;
  auto started_future = started.get_future();
  auto waiter = std::async(std::launch::async, [&] {
    started.set_value();
    xe::cpu::Entry* resolved_entry = nullptr;
    auto status = table.GetOrCreate(0x1000, &resolved_entry);
    return std::make_pair(status, resolved_entry);
  });

  started_future.wait();
  const bool waited_for_compilation =
      waiter.wait_for(20ms) == std::future_status::timeout;

  SECTION("successful compilation") {
    table.MarkReady(compiling_entry, nullptr, 0x1004);
    auto [status, entry] = waiter.get();
    REQUIRE(waited_for_compilation);
    REQUIRE(status == xe::cpu::Entry::STATUS_READY);
    REQUIRE(entry == compiling_entry);
    REQUIRE(entry->end_address == 0x1004);
  }

  SECTION("failed compilation") {
    table.MarkFailed(compiling_entry);
    auto [status, entry] = waiter.get();
    REQUIRE(waited_for_compilation);
    REQUIRE(status == xe::cpu::Entry::STATUS_FAILED);
    REQUIRE(entry == compiling_entry);
  }
}
