/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/guest_range.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace util {
namespace test {

TEST_CASE("Guest ranges require committed accessible pages",
          "[kernel][memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());

  constexpr uint32_t address = 0x90010000;
  constexpr uint32_t page_size = 0x1000;
  auto* heap = memory.LookupHeap(address);
  REQUIRE(heap != nullptr);
  REQUIRE(heap->AllocFixed(address, 3 * page_size, page_size,
                           kMemoryAllocationReserve,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK_FALSE(IsGuestRangeReadable(&memory, address, page_size));
  CHECK_FALSE(IsGuestRangeWritable(&memory, address, page_size));

  REQUIRE(heap->AllocFixed(address, page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(IsGuestRangeReadable(&memory, address, page_size));
  CHECK(IsGuestRangeWritable(&memory, address, page_size));
  CHECK_FALSE(IsGuestRangeReadable(&memory, address, 2 * page_size));

  REQUIRE(heap->AllocFixed(address + page_size, page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(IsGuestRangeReadable(&memory, address, 2 * page_size));
  CHECK_FALSE(IsGuestRangeWritable(&memory, address, 3 * page_size));

  REQUIRE(heap->Protect(address, page_size, kMemoryProtectRead));
  CHECK(IsGuestRangeReadable(&memory, address, page_size));
  CHECK_FALSE(IsGuestRangeWritable(&memory, address, page_size));

  REQUIRE(heap->Decommit(address + page_size, page_size));
  CHECK_FALSE(IsGuestRangeReadable(&memory, address, 2 * page_size));
  CHECK_FALSE(IsGuestRangeReadable(&memory, address + page_size, page_size));
  CHECK_FALSE(IsGuestRangeWritable(&memory, address + page_size, page_size));
  CHECK_FALSE(IsGuestRangeReadable(&memory, 0, page_size));
  CHECK_FALSE(IsGuestRangeReadable(&memory, address, 0));
  CHECK_FALSE(IsGuestRangeReadable(&memory, UINT32_MAX - 1, page_size));
}

}  // namespace test
}  // namespace util
}  // namespace kernel
}  // namespace xe
