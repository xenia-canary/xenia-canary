/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/memory.h"

#include "third_party/catch/include/catch.hpp"

#include "xenia/base/memory.h"

namespace xe {
namespace test {

// All tests use kMemoryAllocationReserve which only touches the page table,
// not host memory. This lets us pass nullptr for membase and Memory*.

TEST_CASE("PhysicalHeap::GetPhysicalAddress", "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  SECTION("heap with no offset returns heap-relative address") {
    PhysicalHeap heap;
    heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                    0x20000000, 64 * 1024, &parent);

    REQUIRE(heap.host_address_offset() == 0);
    REQUIRE(heap.GetPhysicalAddress(0xA0000000) == 0);
    REQUIRE(heap.GetPhysicalAddress(0xA0010000) == 0x10000);
    REQUIRE(heap.GetPhysicalAddress(0xA1000000) == 0x1000000);
  }

  SECTION("0xE0000000 heap always has 0x1000 physical offset") {
    PhysicalHeap heap;
    heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xE0000000,
                    0x1FD00000, 4096, &parent);

    // The 0x1000 physical offset is baked into the view mapping
    // (map_info target_address), not derived from host_address_offset.
    REQUIRE(heap.GetPhysicalAddress(0xE0000000) == 0x1000);
    REQUIRE(heap.GetPhysicalAddress(0xE0001000) == 0x2000);
    REQUIRE(heap.GetPhysicalAddress(0xE0010000) == 0x11000);
  }
}

TEST_CASE("PhysicalHeap::Alloc alignment", "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  SECTION("returned address is page-aligned") {
    PhysicalHeap heap;
    heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                    0x20000000, 64 * 1024, &parent);

    uint32_t addr = 0;
    bool ok = heap.Alloc(0x10000, 0x10000, kMemoryAllocationReserve,
                         kMemoryProtectRead, false, &addr);
    REQUIRE(ok);
    REQUIRE(addr != 0);
    REQUIRE(addr % 0x10000 == 0);
    REQUIRE(addr >= 0xA0000000);
    REQUIRE(addr < 0xC0000000);
  }

  SECTION("multiple allocations with different alignments") {
    PhysicalHeap heap;
    heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                    0x20000000, 64 * 1024, &parent);

    for (uint32_t alignment : {0x10000u, 0x20000u, 0x40000u, 0x100000u}) {
      uint32_t addr = 0;
      bool ok = heap.Alloc(alignment, alignment, kMemoryAllocationReserve,
                           kMemoryProtectRead, false, &addr);
      REQUIRE(ok);
      REQUIRE(addr % alignment == 0);
    }
  }
}

TEST_CASE("PhysicalHeap::AllocRange alignment", "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  SECTION("returned address respects alignment within range") {
    PhysicalHeap heap;
    heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                    0x20000000, 64 * 1024, &parent);

    uint32_t addr = 0;
    bool ok = heap.AllocRange(0xA0000000, 0xBFFFFFFF, 0x10000, 0x10000,
                              kMemoryAllocationReserve, kMemoryProtectRead,
                              false, &addr);
    REQUIRE(ok);
    REQUIRE(addr % 0x10000 == 0);
    REQUIRE(addr >= 0xA0000000);
    REQUIRE(addr <= 0xBFFFFFFF);
  }

  SECTION("large alignment preserved through translation") {
    PhysicalHeap heap;
    heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xC0000000,
                    0x20000000, 16 * 1024 * 1024, &parent);

    uint32_t addr = 0;
    bool ok = heap.AllocRange(0xC0000000, 0xDFFFFFFF, 0x1000000, 0x1000000,
                              kMemoryAllocationReserve, kMemoryProtectRead,
                              false, &addr);
    REQUIRE(ok);
    REQUIRE(addr % 0x1000000 == 0);
  }
}

TEST_CASE("PhysicalHeap::AllocFixed alignment", "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  PhysicalHeap heap;
  heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                  0x20000000, 64 * 1024, &parent);

  // AllocFixed at a specific aligned address must succeed
  bool ok = heap.AllocFixed(0xA0100000, 0x10000, 0x10000,
                            kMemoryAllocationReserve, kMemoryProtectRead);
  REQUIRE(ok);
}

TEST_CASE("PhysicalHeap vE0000000 alignment", "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  PhysicalHeap heap;
  heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xE0000000,
                  0x1FD00000, 4096, &parent);

  // The 0xE0000000 heap always has a 0x1000 physical offset, so the
  // translation offset is 0xE0000000 - 0x1000 = 0xDFFFF000, which is
  // 4KB-aligned but not 64KB-aligned. This is true on all platforms.
  uint32_t physical_base = heap.GetPhysicalAddress(heap.heap_base());
  REQUIRE(physical_base == 0x1000);

  SECTION("page-size allocation preserves alignment") {
    uint32_t addr = 0;
    bool ok = heap.Alloc(0x1000, 0x1000, kMemoryAllocationReserve,
                         kMemoryProtectRead, false, &addr);
    REQUIRE(ok);
    REQUIRE(addr % 0x1000 == 0);
    REQUIRE(addr >= 0xE0000000);
  }

  SECTION("translation offset is 4KB-aligned") {
    uint32_t translation_offset = heap.heap_base() - physical_base;
    REQUIRE(translation_offset % heap.page_size() == 0);
  }

  SECTION("alloc with alignment larger than page_size follows host offset") {
    // The parent allocation is physically aligned. Whether the translated
    // address is host-aligned depends on the host offset compensating for the
    // 0x1000 physical translation offset.
    uint32_t alignment = 0x10000;  // 64KB
    uint32_t addr = 0;
    bool ok = heap.Alloc(0x10000, alignment, kMemoryAllocationReserve,
                         kMemoryProtectRead, false, &addr);
    if (heap.host_address_offset() == physical_base) {
      REQUIRE(ok);
      REQUIRE(heap.GetPhysicalAddress(addr) % alignment == 0);
      REQUIRE((addr + heap.host_address_offset()) % alignment == 0);
    } else {
      REQUIRE_FALSE(ok);
      REQUIRE(addr == 0);
    }
  }
}

TEST_CASE("PhysicalHeap vE0000000 AllocRange alignment", "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  PhysicalHeap heap;
  heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xE0000000,
                  0x1FD00000, 4096, &parent);

  SECTION("page-aligned AllocRange succeeds") {
    uint32_t addr = 0;
    bool ok = heap.AllocRange(0xE0000000, 0xFFFCFFFF, 0x1000, 0x1000,
                              kMemoryAllocationReserve, kMemoryProtectRead,
                              false, &addr);
    REQUIRE(ok);
    REQUIRE(addr % 0x1000 == 0);
  }

  SECTION("AllocRange with large alignment follows host offset") {
    // The parent address is physically aligned, but the translated child
    // address is host-aligned only when the host offset compensates for the
    // physical translation offset.
    uint32_t alignment = 0x10000;
    uint32_t addr = 0;
    bool ok = heap.AllocRange(0xE0000000, 0xFFFCFFFF, 0x10000, alignment,
                              kMemoryAllocationReserve, kMemoryProtectRead,
                              false, &addr);
    if (heap.host_address_offset() ==
        heap.GetPhysicalAddress(heap.heap_base())) {
      REQUIRE(ok);
      REQUIRE(addr >= 0xE0000000);
      REQUIRE(heap.GetPhysicalAddress(addr) % alignment == 0);
      REQUIRE((addr + heap.host_address_offset()) % alignment == 0);
    } else {
      REQUIRE_FALSE(ok);
      REQUIRE(addr == 0);
    }
  }
}

TEST_CASE("PhysicalHeap::AllocRange stays within the requested range",
          "[memory]") {
  VirtualHeap parent;
  parent.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0x00000000,
                    0x20000000, 4096);

  PhysicalHeap heap;
  heap.Initialize(nullptr, nullptr, HeapType::kGuestPhysical, 0xA0000000,
                  0x20000000, 64 * 1024, &parent);

  // A ceiling that is not a multiple of the alignment must not be rounded up
  // to the next one, which would place the allocation above it.
  const uint32_t size = 0x10000;
  const uint32_t alignment = 0x10000;
  const uint32_t high_address = 0xA0FF8000;
  uint32_t addr = 0;
  bool ok = heap.AllocRange(0xA0000000, high_address, size, alignment,
                            kMemoryAllocationReserve, kMemoryProtectRead, true,
                            &addr);
  REQUIRE(ok);
  REQUIRE(heap.GetPhysicalAddress(addr) % alignment == 0);
  REQUIRE(addr + size - 1 <= high_address);
}

}  // namespace test
}  // namespace xe
