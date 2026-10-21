/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xfile.h"

#include <algorithm>
#include <array>

#include "xenia/kernel/testing/kernel_state_test.h"

namespace xe {
namespace kernel {
namespace test {
namespace {

class ReadProbeFile : public vfs::File {
 public:
  ReadProbeFile() : vfs::File(0, nullptr) {}

  void Destroy() override { delete this; }

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                    size_t* out_bytes_read) override {
    ++read_count;
    last_offset = byte_offset;
    const size_t length = std::min(buffer.size(), payload.size());
    std::copy_n(payload.begin(), length, buffer.begin());
    *out_bytes_read = length;
    return X_STATUS_SUCCESS;
  }

  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override {
    return X_STATUS_NOT_IMPLEMENTED;
  }

  const std::array<uint8_t, 4> payload = {0x12, 0x34, 0x56, 0x78};
  uint32_t read_count = 0;
  size_t last_offset = 0;
};

}  // namespace

TEST_CASE_METHOD(KernelStateTest,
                 "XFile physical reads require committed writable pages",
                 "[kernel][memory][io]") {
  constexpr uint32_t address = 0xE1000000;
  constexpr uint32_t page_size = 0x1000;
  auto* heap = static_cast<PhysicalHeap*>(memory()->LookupHeap(address));
  REQUIRE(heap != nullptr);
  REQUIRE(heap->AllocFixed(address, 2 * page_size, page_size,
                           kMemoryAllocationReserve,
                           kMemoryProtectRead | kMemoryProtectWrite));

  auto* probe = new ReadProbeFile();
  auto file = make_object<XFile>(kernel_state(), probe, true, true);
  uint32_t bytes_read = UINT32_MAX;
  bool pending = true;
  CHECK(file->Read(address, 4, 7, &bytes_read, 0, false, &pending) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(bytes_read == 0);
  CHECK_FALSE(pending);
  CHECK(probe->read_count == 0);
  CHECK(file->position() == 0);

  // Rejection happens before asynchronous buffering/device decisions, too.
  auto* async_probe = new ReadProbeFile();
  auto async_file =
      make_object<XFile>(kernel_state(), async_probe, false, false);
  pending = true;
  bytes_read = UINT32_MAX;
  CHECK(async_file->Read(address, 4, 7, &bytes_read, 0, false, &pending) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(bytes_read == 0);
  CHECK_FALSE(pending);
  CHECK(async_probe->read_count == 0);

  REQUIRE(heap->AllocFixed(address, page_size, page_size,
                           kMemoryAllocationCommit,
                           kMemoryProtectRead | kMemoryProtectWrite));
  CHECK(file->Read(address, 4, 7, &bytes_read, 0, false, &pending) ==
        X_STATUS_SUCCESS);
  CHECK(bytes_read == probe->payload.size());
  CHECK_FALSE(pending);
  CHECK(probe->read_count == 1);
  CHECK(probe->last_offset == 7);
  CHECK(file->position() == 11);
  auto* destination =
      memory()->TranslatePhysical(heap->GetPhysicalAddress(address));
  CHECK(std::equal(probe->payload.begin(), probe->payload.end(), destination));

  // Both partially covered pages must allow a physical destination write.
  CHECK(file->Read(address + page_size - 2, 4, 0, &bytes_read, 0, false) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(bytes_read == 0);
  CHECK(probe->read_count == 1);
  CHECK(file->position() == 11);

  REQUIRE(heap->Protect(address, page_size, kMemoryProtectRead));
  CHECK(file->Read(address, 4, 0, &bytes_read, 0, false) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(bytes_read == 0);
  CHECK(probe->read_count == 1);

  REQUIRE(heap->Protect(address, page_size,
                        kMemoryProtectRead | kMemoryProtectWrite));
  REQUIRE(heap->Decommit(address, page_size));
  pending = true;
  CHECK(file->Read(address, 4, 0, &bytes_read, 0, false, &pending) ==
        X_STATUS_ACCESS_VIOLATION);
  CHECK(bytes_read == 0);
  CHECK_FALSE(pending);
  CHECK(probe->read_count == 1);
  CHECK(file->position() == 11);

  CHECK(file->Read(address, 0, 0, &bytes_read, 0, false) == X_STATUS_SUCCESS);
  CHECK(bytes_read == 0);
  CHECK(probe->read_count == 1);
  CHECK(file->position() == 11);
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
