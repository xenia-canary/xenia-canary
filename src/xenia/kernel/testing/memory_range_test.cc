/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/memory.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace test {

TEST_CASE("Memory ranges require committed accessible pages", "[memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());

  using xe::memory::PageAccess;
  constexpr uint32_t address = 0x90010000;
  constexpr uint32_t page_size = 0x1000;
  auto* heap = memory.LookupHeap(address);
  REQUIRE(heap != nullptr);
  REQUIRE(heap->AllocFixed(address, 3 * page_size, page_size,
                           kMemoryAllocationReserve,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(memory.QueryRangeAccess(address, page_size) == PageAccess::kNoAccess);

  REQUIRE(heap->AllocFixed(address, 2 * page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(memory.QueryRangeAccess(address, 2 * page_size) ==
        PageAccess::kReadWrite);
  CHECK(memory.QueryRangeAccess(address, 3 * page_size) ==
        PageAccess::kNoAccess);
  // An unaligned range must include both partially covered pages.
  CHECK(memory.QueryRangeAccess(address + 2 * page_size - 1, 2) ==
        PageAccess::kNoAccess);

  REQUIRE(heap->Protect(address, page_size, kMemoryProtectRead));
  CHECK(memory.QueryRangeAccess(address, page_size) == PageAccess::kReadOnly);
  CHECK(memory.QueryRangeAccess(address, 2 * page_size) ==
        PageAccess::kReadOnly);
  CHECK(memory.QueryRangeAccess(address + page_size, page_size) ==
        PageAccess::kReadWrite);
  CHECK(memory.QueryRangeAccess(address + page_size - 1, 2) ==
        PageAccess::kReadOnly);

  REQUIRE(heap->Protect(address, page_size,
                        kMemoryProtectRead | kMemoryProtectWriteCombine));
  CHECK(memory.QueryRangeAccess(address, 2 * page_size) ==
        PageAccess::kReadWrite);

  REQUIRE(heap->Protect(address, page_size, kMemoryProtectNoAccess));
  CHECK(memory.QueryRangeAccess(address, 2 * page_size) ==
        PageAccess::kNoAccess);

  REQUIRE(heap->Decommit(address + page_size, page_size));
  CHECK(memory.QueryRangeAccess(address + page_size, page_size) ==
        PageAccess::kNoAccess);
  // Decommit retains protection bits, but they must not imply accessibility.
  uint32_t protect = 0;
  REQUIRE(heap->QueryProtect(address + page_size, &protect));
  CHECK(protect == (kMemoryProtectRead | kMemoryProtectWrite));
}

TEST_CASE("Memory ranges validate length and heap boundaries", "[memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());

  using xe::memory::PageAccess;
  constexpr uint32_t boundary = 0x40000000;
  constexpr uint32_t page_size = 0x1000;
  auto* low_heap = memory.LookupHeap(boundary - page_size);
  auto* high_heap = memory.LookupHeap(boundary);
  REQUIRE(low_heap != nullptr);
  REQUIRE(high_heap != nullptr);
  REQUIRE(low_heap != high_heap);
  REQUIRE(
      low_heap->AllocFixed(boundary - page_size, page_size, page_size,
                           kMemoryAllocationReserve | kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(high_heap->AllocFixed(
      boundary, high_heap->page_size(), high_heap->page_size(),
      kMemoryAllocationReserve | kMemoryAllocationCommit,
      kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(memory.QueryRangeAccess(boundary - page_size, page_size) ==
        PageAccess::kReadWrite);
  CHECK(memory.QueryRangeAccess(boundary - 1, 1) == PageAccess::kReadWrite);
  CHECK(memory.QueryRangeAccess(boundary, 1) == PageAccess::kReadWrite);
  CHECK(memory.QueryRangeAccess(boundary - 1, 2) == PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(boundary - page_size, page_size + 1) ==
        PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(boundary - page_size, 0) ==
        PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(boundary - page_size, UINT32_MAX) ==
        PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(UINT32_MAX, 2) == PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(0x7FC80000, 1) == PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(0xFFD00000, 1) == PageAccess::kNoAccess);

  // Address zero follows page protection; null-pointer policy is caller-owned.
  REQUIRE(memory.LookupHeap(0)->Protect(0, page_size, kMemoryProtectRead));
  CHECK(memory.QueryRangeAccess(0, 1) == PageAccess::kReadOnly);
}

TEST_CASE("Physical range access follows guest commitment", "[memory]") {
  Memory memory;
  REQUIRE(memory.Initialize());

  using xe::memory::PageAccess;
  constexpr uint32_t address = 0xE1000000;
  constexpr uint32_t page_size = 0x1000;
  auto* heap = static_cast<PhysicalHeap*>(memory.LookupHeap(address));
  REQUIRE(heap != nullptr);
  REQUIRE(heap->heap_type() == HeapType::kGuestPhysical);
  auto* physical_heap = memory.GetPhysicalHeap();
  const uint32_t physical_address = heap->GetPhysicalAddress(address);
  REQUIRE(heap->AllocFixed(address, 3 * page_size, page_size,
                           kMemoryAllocationReserve,
                           kMemoryProtectRead | kMemoryProtectWrite));
  // Physical host backing is committed by Initialize independently of the
  // guest page tables queried by XFile and the GPU upload endpoint guards.
  CHECK(memory.QueryRangeAccess(address, page_size) == PageAccess::kNoAccess);
  CHECK(physical_heap->QueryRangeAccess(physical_address, physical_address) ==
        PageAccess::kNoAccess);

  REQUIRE(heap->AllocFixed(address, page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(heap->AllocFixed(address + 2 * page_size, page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(memory.QueryRangeAccess(address, page_size) == PageAccess::kReadWrite);
  // The GPU guards query only the endpoints. A reserved interior page does not
  // make those individual queries fail, while a full-range query must fail.
  CHECK(physical_heap->QueryRangeAccess(physical_address, physical_address) ==
        PageAccess::kReadWrite);
  CHECK(physical_heap->QueryRangeAccess(physical_address + 2 * page_size,
                                        physical_address + 2 * page_size) ==
        PageAccess::kReadWrite);
  CHECK(physical_heap->QueryRangeAccess(physical_address,
                                        physical_address + 3 * page_size - 1) ==
        PageAccess::kNoAccess);
  CHECK(memory.QueryRangeAccess(address, 3 * page_size) ==
        PageAccess::kNoAccess);

  REQUIRE(heap->AllocFixed(address + page_size, page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(memory.QueryRangeAccess(address, 3 * page_size) ==
        PageAccess::kReadWrite);
  CHECK(heap->QueryRangeAccess(address, address + 3 * page_size - 1) ==
        PageAccess::kReadWrite);
  CHECK(physical_heap->QueryRangeAccess(physical_address,
                                        physical_address + 3 * page_size - 1) ==
        PageAccess::kReadWrite);

  REQUIRE(heap->Protect(address, page_size, kMemoryProtectRead));
  CHECK(memory.QueryRangeAccess(address, 3 * page_size) ==
        PageAccess::kReadOnly);
  CHECK(physical_heap->QueryRangeAccess(physical_address, physical_address) ==
        PageAccess::kReadOnly);
  REQUIRE(heap->Decommit(address, page_size));
  CHECK(heap->QueryRangeAccess(address, address) == PageAccess::kNoAccess);
  CHECK(physical_heap->QueryRangeAccess(physical_address, physical_address) ==
        PageAccess::kNoAccess);
}

}  // namespace test
}  // namespace xe
