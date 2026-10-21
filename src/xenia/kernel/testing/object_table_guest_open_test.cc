/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/object_table.h"

#include <barrier>
#include <cstdint>
#include <thread>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/byte_stream.h"

namespace xe {
namespace kernel {
namespace util {
namespace test {

TEST_CASE("The first guest open retains its creation reference",
          "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK_FALSE(table.LookupObject<XObject>(handle));
  REQUIRE(object->handles().size() == 1);
  CHECK(object->handle() == handle);

  // Closing the guest handle leaves the creation reference for dereference.
  REQUIRE(table.ReleaseHandle(handle) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
  CHECK(table.ReleaseHandle(handle) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("A held source reference does not count as another guest open",
          "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandle(handle) == X_STATUS_SUCCESS);
  uint64_t source_generation = 0;
  REQUIRE(table.RetainHandle(handle, &source_generation) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  CHECK(generation == source_generation);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK_FALSE(table.LookupObject<XObject>(handle));
  REQUIRE(table.OpenGuestHandle(handle, object.get(), true,
                                source_generation) == X_STATUS_SUCCESS);
  CHECK(table.LookupObject<XObject>(handle).get() == object.get());
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), source_generation) ==
          X_STATUS_SUCCESS);

  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK(table.ReleaseHandle(handle) == X_STATUS_INVALID_HANDLE);
  CHECK(object->handles().empty());
}

TEST_CASE("Untracked closes release references without claiming a guest open",
          "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.RetainHandle(handle) == X_STATUS_SUCCESS);

  for (int i = 0; i < 2; ++i) {
    int32_t count = 0;
    uint64_t generation = 0;
    REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
            X_STATUS_SUCCESS);
    CHECK(count == -1);
    REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
            X_STATUS_SUCCESS);
    CHECK(bool(table.LookupObject<XObject>(handle)) == (i == 0));
  }
  CHECK(object->handles().empty());
}

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
  CHECK_FALSE(table.LookupObject<XObject>(first));
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
  CHECK(table.ReleaseHandle(first) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
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
  REQUIRE(table.ReleaseHandle(handle) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
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
  CHECK(table.OpenGuestHandle(reused_handle, object.get(), true,
                              old_generation) == X_STATUS_INVALID_HANDLE);
  CHECK(table.ReleaseHandleOf(handle, object.get(), old_generation) ==
        X_STATUS_INVALID_HANDLE);

  uint64_t new_generation = 0;
  REQUIRE(table.ClaimGuestClose(reused_handle, object.get(), &count,
                                &new_generation) == X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(reused_handle, object.get(), new_generation) ==
          X_STATUS_SUCCESS);
}

TEST_CASE("Concurrent closes can claim a single guest open only once",
          "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);

  X_STATUS statuses[2] = {};
  int32_t counts[2] = {};
  uint64_t generations[2] = {};
  std::barrier<> start(2);
  auto close = [&](int index) {
    start.arrive_and_wait();
    statuses[index] = table.ClaimGuestClose(
        handle, object.get(), &counts[index], &generations[index]);
  };
  std::thread first(close, 0);
  std::thread second(close, 1);
  first.join();
  second.join();

  const int winner = statuses[0] == X_STATUS_SUCCESS ? 0 : 1;
  REQUIRE(statuses[winner] == X_STATUS_SUCCESS);
  CHECK(statuses[1 - winner] == X_STATUS_INVALID_HANDLE);
  CHECK(counts[winner] == 1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generations[winner]) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandle(handle) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
}

TEST_CASE("Guest open and close reject a different object in a live slot",
          "[kernel][ob]") {
  ObjectTable table;
  auto object = make_object<XObject>(XObject::Type::Event);
  auto other = make_object<XObject>(XObject::Type::Event);
  X_HANDLE handle = 0;
  REQUIRE(table.AddHandle(object.get(), &handle) == X_STATUS_SUCCESS);
  REQUIRE(table.OpenGuestHandle(handle, object.get()) == X_STATUS_SUCCESS);
  CHECK(table.OpenGuestHandle(handle, other.get()) == X_STATUS_INVALID_HANDLE);

  int32_t count = 123;
  uint64_t generation = 456;
  CHECK(table.ClaimGuestClose(handle, other.get(), &count, &generation) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(count == 123);
  CHECK(generation == 456);
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  CHECK(table.ReleaseHandleOf(handle, other.get(), generation) ==
        X_STATUS_INVALID_HANDLE);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandle(handle) == X_STATUS_SUCCESS);
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
  CHECK_FALSE(table.LookupObject<XObject>(past_table));
  CHECK(table.ClaimGuestClose(past_table, object.get(), &count, &generation) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(table.DuplicateHandle(past_table, nullptr) == X_STATUS_INVALID_HANDLE);

  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK_FALSE(table.LookupObject<XObject>(handle));
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
  CHECK(restored_table.ReleaseHandle(first) == X_STATUS_SUCCESS);
  CHECK(restored_object->handles().empty());
}

TEST_CASE("Unknown object table save versions are rejected", "[kernel][ob]") {
  uint32_t words[] = {UINT32_MAX, 3};
  ByteStream reader(reinterpret_cast<uint8_t*>(words), sizeof(words));
  ObjectTable table;
  CHECK_FALSE(table.Restore(&reader));
}

TEST_CASE("Version one object table counts remain readable", "[kernel][ob]") {
  std::vector<uint8_t> bytes(32);
  ByteStream writer(bytes.data(), bytes.size());
  writer.Write<uint32_t>(UINT32_MAX);
  writer.Write<uint32_t>(1);
  writer.Write<uint32_t>(0);  // Host table capacity.
  writer.Write<uint32_t>(1);  // Guest table capacity.
  writer.Write<int32_t>(2);   // Creation reference and one guest open.
  writer.Write<int32_t>(1);

  ObjectTable table;
  ByteStream reader(bytes.data(), bytes.size());
  REQUIRE(table.Restore(&reader));
  auto object = make_object<XObject>(XObject::Type::Event);
  const X_HANDLE handle = XObject::kHandleBase;
  object->handles().push_back(handle);
  REQUIRE(table.RestoreHandle(handle, object.get()) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK_FALSE(table.LookupObject<XObject>(handle));
  REQUIRE(table.ReleaseHandle(handle) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
}

TEST_CASE("Resaving a legacy table preserves unknown and new exact counts",
          "[kernel][ob]") {
  std::vector<uint8_t> legacy_bytes(32);
  ByteStream legacy_writer(legacy_bytes.data(), legacy_bytes.size());
  legacy_writer.Write<uint32_t>(0);
  legacy_writer.Write<uint32_t>(1);
  legacy_writer.Write<int32_t>(2);

  const X_HANDLE legacy_handle = XObject::kHandleBase;
  X_HANDLE duplicate = 0;
  X_HANDLE exact_handle = 0;
  std::vector<uint8_t> bytes(1 << 20);
  {
    ObjectTable table;
    ByteStream legacy_reader(legacy_bytes.data(), legacy_bytes.size());
    REQUIRE(table.Restore(&legacy_reader));
    auto legacy_object = make_object<XObject>(XObject::Type::Event);
    legacy_object->handles().push_back(legacy_handle);
    REQUIRE(table.RestoreHandle(legacy_handle, legacy_object.get()) ==
            X_STATUS_SUCCESS);
    REQUIRE(table.OpenGuestHandle(legacy_handle, legacy_object.get()) ==
            X_STATUS_SUCCESS);
    REQUIRE(table.DuplicateHandle(legacy_handle, &duplicate) ==
            X_STATUS_SUCCESS);

    auto exact_object = make_object<XObject>(XObject::Type::Event);
    REQUIRE(table.AddHandle(exact_object.get(), &exact_handle) ==
            X_STATUS_SUCCESS);
    REQUIRE(table.OpenGuestHandle(exact_handle, exact_object.get()) ==
            X_STATUS_SUCCESS);
    ByteStream writer(bytes.data(), bytes.size());
    REQUIRE(table.Save(&writer));
  }

  ByteStream format_reader(bytes.data(), bytes.size());
  CHECK(format_reader.Read<uint32_t>() == UINT32_MAX);
  CHECK(format_reader.Read<uint32_t>() == 2);

  ObjectTable table;
  ByteStream reader(bytes.data(), bytes.size());
  REQUIRE(table.Restore(&reader));
  auto legacy_object = make_object<XObject>(XObject::Type::Event);
  legacy_object->handles().push_back(legacy_handle);
  legacy_object->handles().push_back(duplicate);
  REQUIRE(table.RestoreHandle(legacy_handle, legacy_object.get()) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.RestoreHandle(duplicate, legacy_object.get()) ==
          X_STATUS_SUCCESS);
  auto exact_object = make_object<XObject>(XObject::Type::Event);
  exact_object->handles().push_back(exact_handle);
  REQUIRE(table.RestoreHandle(exact_handle, exact_object.get()) ==
          X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(legacy_handle, legacy_object.get(), &count,
                                &generation) == X_STATUS_SUCCESS);
  CHECK(count == -1);
  REQUIRE(table.ReleaseHandleOf(legacy_handle, legacy_object.get(),
                                generation) == X_STATUS_SUCCESS);
  REQUIRE(table.ClaimGuestClose(duplicate, legacy_object.get(), &count,
                                &generation) == X_STATUS_SUCCESS);
  CHECK(count == -1);
  REQUIRE(table.ReleaseHandleOf(duplicate, legacy_object.get(), generation) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ClaimGuestClose(exact_handle, exact_object.get(), &count,
                                &generation) == X_STATUS_SUCCESS);
  CHECK(count == 1);
  REQUIRE(table.ReleaseHandleOf(exact_handle, exact_object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK_FALSE(table.LookupObject<XObject>(exact_handle));
  REQUIRE(table.ReleaseHandle(exact_handle) == X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandle(legacy_handle) == X_STATUS_SUCCESS);
  REQUIRE(table.ReleaseHandle(legacy_handle) == X_STATUS_SUCCESS);
  CHECK(legacy_object->handles().empty());
  CHECK(exact_object->handles().empty());
}

TEST_CASE("Legacy guest counts remain unknown after opening and duplication",
          "[kernel][ob]") {
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
  X_HANDLE duplicate = 0;
  REQUIRE(table.DuplicateHandle(handle, &duplicate) == X_STATUS_SUCCESS);

  int32_t count = 0;
  uint64_t generation = 0;
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == -1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  REQUIRE(table.ClaimGuestClose(handle, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == -1);
  REQUIRE(table.ReleaseHandleOf(handle, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK(table.LookupObject<XObject>(handle) == nullptr);
  REQUIRE(table.ClaimGuestClose(duplicate, object.get(), &count, &generation) ==
          X_STATUS_SUCCESS);
  CHECK(count == -1);
  REQUIRE(table.ReleaseHandleOf(duplicate, object.get(), generation) ==
          X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
}

}  // namespace test
}  // namespace util
}  // namespace kernel
}  // namespace xe
