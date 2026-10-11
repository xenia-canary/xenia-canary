/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/object_table.h"

#include <cstdint>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_stream.h"

namespace xe {
namespace kernel {
namespace util {
namespace test {

TEST_CASE("Guest close counts opens across duplicate slots", "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE first = 0;
  X_HANDLE duplicate = 0;
  REQUIRE(table.AddHandle(object.get(), &first) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(first, object.get()) == X_STATUS_SUCCESS);
  REQUIRE(table.RetainHandle(first) == X_STATUS_SUCCESS);  // Pointer reference.
  REQUIRE(table.DuplicateHandle(first, &duplicate) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(first, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 2);
  CHECK_FALSE(table.IsGuestHandleOpen(first, object.get()));
  CHECK(table.ClaimGuestClose(first, object.get(), &count, &generation) ==
        X_STATUS_INVALID_HANDLE);
  REQUIRE(table.ReleaseHandleOf(first, object.get(), generation) ==
          X_STATUS_SUCCESS);
  X_HANDLE rejected_handle = X_INVALID_HANDLE_VALUE;
  CHECK(table.DuplicateHandle(first, &rejected_handle) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(rejected_handle == X_INVALID_HANDLE_VALUE);

  REQUIRE(table.ClaimGuestClose(duplicate, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(duplicate, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK(table.ReleaseHandle(first) == X_STATUS_SUCCESS);
}

TEST_CASE("Two opens of one slot are each claimed once", "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t first_generation = 0;
  uint64_t second_generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count,
                                &first_generation) == X_STATUS_SUCCESS);
  CHECK(count == 2);
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count,
                                &second_generation) == X_STATUS_SUCCESS);
  CHECK(count == 1);
  CHECK(table.ClaimGuestClose(handle, object.get(), &count,
                              &second_generation) == X_STATUS_INVALID_HANDLE);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), second_generation) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), first_generation) ==
          X_STATUS_SUCCESS);
}

TEST_CASE("A close cannot release a recycled slot", "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t old_generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count,
                                &old_generation) == X_STATUS_SUCCESS);
  table.Reset();

  X_HANDLE reused_handle = 0;
  REQUIRE(table.AddHandle(object.get(), &reused_handle) == X_STATUS_SUCCESS);
  REQUIRE(reused_handle == handle);
  REQUIRE(table.OpenGuestHandle(reused_handle, object.get()) ==
          X_STATUS_SUCCESS);
  CHECK(table.ReleaseHandleOf(handle, object.get(), old_generation) ==
        X_STATUS_INVALID_HANDLE);

  uint64_t new_generation = 0;
  REQUIRE(table.ClaimGuestClose(reused_handle, object.get(), &count,
                                &new_generation) == X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandleOf(reused_handle, object.get(), new_generation) ==
          X_STATUS_SUCCESS);
}

TEST_CASE("Guest handle validation rejects free and out-of-range slots",
          "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);

  const X_HANDLE past_table = XObject::kHandleBase + (16 * 1024 << 2);
  int32_t count = 0;
  uint64_t generation = 0;
  CHECK_FALSE(table.IsGuestHandleOpen(past_table, object.get()));
  CHECK(table.ClaimGuestClose(past_table, object.get(), &count, &generation) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(table.DuplicateHandle(past_table, nullptr) == X_STATUS_INVALID_HANDLE);

  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK_FALSE(table.IsGuestHandleOpen(handle, object.get()));
  CHECK(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
        X_STATUS_INVALID_HANDLE);
}

TEST_CASE("Guest open counts survive an object table save", "[kernel][ob]") {
  std::vector<uint8_t> bytes(1 << 20);
  X_HANDLE first = 0;
  X_HANDLE duplicate = 0;
  {
    ObjectTable table;
    auto object = make_object<XObject>(XObject::Type::Event);
    REQUIRE(table.AddHandle(object.get(), &first) == X_STATUS_SUCCESS);
    REQUIRE(table.OpenGuestHandle(first, object.get()) == X_STATUS_SUCCESS);
    REQUIRE(table.RetainHandle(first) == X_STATUS_SUCCESS);
    REQUIRE(table.DuplicateHandle(first, &duplicate) == X_STATUS_SUCCESS);
    ByteStream writer(bytes.data(), bytes.size());
    REQUIRE(table.Save(&writer));
  }

  ObjectTable restored_table;
  auto restored_object = make_object<XObject>(XObject::Type::Event);
  ByteStream reader(bytes.data(), bytes.size());
  REQUIRE(restored_table.Restore(&reader));
  restored_object->handles().push_back(first);
  restored_object->handles().push_back(duplicate);
  REQUIRE(restored_table.RestoreHandle(first, restored_object.get()) ==
          X_STATUS_SUCCESS);
  REQUIRE(restored_table.RestoreHandle(duplicate, restored_object.get()) ==
          X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(restored_table.ClaimGuestClose(first, restored_object.get(), &count,
                                         &generation) == X_STATUS_SUCCESS);
  CHECK(count == 2);
  REQUIRE(restored_table.ReleaseHandleOf(first, restored_object.get(),
                                         generation) == X_STATUS_SUCCESS);
  REQUIRE(restored_table.ClaimGuestClose(duplicate, restored_object.get(),
                                         &count,
                                         &generation) == X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(restored_table.ReleaseHandleOf(duplicate, restored_object.get(),
                                         generation) == X_STATUS_SUCCESS);
  CHECK(restored_table.ReleaseHandle(first) == X_STATUS_SUCCESS);
}

TEST_CASE("Unknown object table save versions are rejected", "[kernel][ob]") {
  uint32_t words[] = {UINT32_MAX, 2};
  ByteStream reader(reinterpret_cast<uint8_t*>(words), sizeof(words));
  ObjectTable table;
  CHECK_FALSE(table.Restore(&reader));
}

TEST_CASE("Legacy object table saves remain readable", "[kernel][ob]") {
  std::vector<uint8_t> bytes(32);
  ByteStream writer(bytes.data(), bytes.size());
  writer.Write<uint32_t>(0);  // Host table capacity.
  writer.Write<uint32_t>(1);  // Guest table capacity.
  writer.Write<int32_t>(1);   // One slot reference, no guest-open field.

  ObjectTable table;
  ByteStream reader(bytes.data(), bytes.size());
  REQUIRE(table.Restore(&reader));
  auto object = make_object<XObject>(XObject::Type::Event);
  const X_HANDLE handle = XObject::kHandleBase;
  object->handles().push_back(handle);
  REQUIRE(table.RestoreHandle(handle, object.get()) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
}

}  // namespace test
}  // namespace util
}  // namespace kernel
}  // namespace xe
