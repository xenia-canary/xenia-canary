/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "third_party/catch/include/catch.hpp"
#include "xenia/kernel/testing/kernel_state_test.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ordinals.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"

namespace xe {
namespace kernel {
namespace test {

class ObjectManagerTest : public KernelStateTest {
 public:
  static constexpr uint32_t kBody = 0x90010010;
  static constexpr uint32_t kType = 0x90011000;
  static constexpr uint32_t kOutput = 0x90012000;

  ObjectManagerTest() {
    xboxkrnl::XboxkrnlModule::RegisterExportTable(
        processor()->export_resolver());
    REQUIRE(memory()->LookupHeap(kBody)->AllocFixed(
        kBody - sizeof(X_OBJECT_HEADER), 0x3000, 0x1000,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite));
    memory()->Zero(kBody - sizeof(X_OBJECT_HEADER), 0x3000);
    auto* header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(
        kBody - sizeof(X_OBJECT_HEADER));
    header->object_type_ptr = kType;
    // Synthetic title metadata exercises the production Object Manager paths.
    memory()->TranslateVirtual<X_OBJECT_TYPE*>(kType)->allocate_proc =
        0x82000000;
    body()->type = EventNotificationObject;
  }

  uint32_t Call(uint16_t ordinal, std::initializer_list<uint32_t> args) {
    cpu::ppc::PPCContext context = {};
    context.virtual_membase = memory()->virtual_membase();
    context.processor = processor();
    size_t index = 3;
    for (uint32_t arg : args) {
      context.r[index++] = arg;
    }
    auto* entry = processor()->export_resolver()->GetExportByOrdinal(
        "xboxkrnl.exe", ordinal);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->function_data.trampoline != nullptr);
    entry->function_data.trampoline(&context);
    return uint32_t(context.r[3]);
  }

  X_DISPATCH_HEADER* body() {
    return memory()->TranslateVirtual<X_DISPATCH_HEADER*>(kBody);
  }

  uint32_t output() {
    return *memory()->TranslateVirtual<be<uint32_t>*>(kOutput);
  }

  X_HANDLE Open() {
    REQUIRE(Call(::ordinals::ObOpenObjectByPointer, {kBody, kOutput}) ==
            X_STATUS_SUCCESS);
    return output();
  }

  object_ref<XObject> Native() {
    return XObject::GetNativeObject(kernel_state(), body());
  }

  uint32_t EventType() {
    return kernel_state()->GetKernelGuestGlobals() +
           offsetof(KernelGuestGlobals, ExEventObjectType);
  }
};

TEST_CASE_METHOD(ObjectManagerTest,
                 "Object Manager references honor the recorded guest type",
                 "[kernel][ob]") {
  const X_HANDLE handle = Open();
  auto object = Native();
  REQUIRE(object != nullptr);
  REQUIRE(object->type() == XObject::Type::Event);
  REQUIRE(object->guest_object_type() == kType);

  CHECK(Call(::ordinals::ObReferenceObjectByHandle,
             {handle, EventType(), kOutput}) == X_STATUS_OBJECT_TYPE_MISMATCH);
  CHECK(output() == 0);
  CHECK(Call(::ordinals::ObReferenceObjectByHandle,
             {handle, kType + sizeof(X_OBJECT_TYPE), kOutput}) ==
        X_STATUS_OBJECT_TYPE_MISMATCH);
  CHECK(output() == 0);
  REQUIRE(Call(::ordinals::ObReferenceObjectByHandle,
               {handle, kType, kOutput}) == X_STATUS_SUCCESS);
  CHECK(output() == kBody);
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  REQUIRE(Call(::ordinals::ObReferenceObjectByHandle, {handle, 0, kOutput}) ==
          X_STATUS_SUCCESS);
  CHECK(output() == kBody);
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "Native kernel events retain their host type references",
                 "[kernel][ob]") {
  auto event = make_object<XEvent>(kernel_state());
  event->Initialize(true, false);
  REQUIRE(Call(::ordinals::ObReferenceObjectByHandle,
               {event->handle(), EventType(), kOutput}) == X_STATUS_SUCCESS);
  CHECK(output() == event->guest_object());
  xboxkrnl::xeObDereferenceObject(nullptr, output());
  CHECK(xboxkrnl::NtClose(event->handle()) == X_STATUS_SUCCESS);
  CHECK(event->handles().empty());
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "A tracked close completes with invalid callback metadata",
                 "[kernel][ob]") {
  const X_HANDLE handle = Open();
  auto object = Native();
  REQUIRE(object != nullptr);

  SECTION("The callback address is not executable guest code") {
    memory()->TranslateVirtual<X_OBJECT_TYPE*>(kType)->close_proc = 0x41576000;
  }
  SECTION("The recorded type becomes unreadable") {
    REQUIRE(memory()->LookupHeap(kType)->Decommit(kType, 0x1000));
  }
  REQUIRE(xboxkrnl::NtClose(handle) == X_STATUS_SUCCESS);
  CHECK_FALSE(kernel_state()->object_table()->LookupObject<XObject>(handle));
  CHECK(xboxkrnl::NtClose(handle) == X_STATUS_INVALID_HANDLE);
  CHECK(Call(::ordinals::ObReferenceObjectByHandle, {handle, 0, kOutput}) ==
        X_STATUS_INVALID_HANDLE);
  CHECK(output() == 0);
  CHECK(Native().get() == object.get());

  // The creation reference remains reachable by its guest pointer.
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  CHECK(object->handles().empty());
  CHECK(body()->wait_list.flink_ptr ==
        kBody + offsetof(X_DISPATCH_HEADER, wait_list));
  CHECK(body()->wait_list.blink_ptr == body()->wait_list.flink_ptr);
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "The native stash follows remaining duplicate handles",
                 "[kernel][ob]") {
  const X_HANDLE first = Open();
  auto object = Native();
  REQUIRE(Call(::ordinals::NtDuplicateObject, {first, kOutput, 0}) ==
          X_STATUS_SUCCESS);
  const X_HANDLE duplicate = output();
  REQUIRE(duplicate != first);
  REQUIRE(xboxkrnl::NtClose(first) == X_STATUS_SUCCESS);
  CHECK(Native().get() == object.get());
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  CHECK(body()->wait_list.blink_ptr == duplicate);
  CHECK(Native().get() == object.get());
  REQUIRE(xboxkrnl::NtClose(duplicate) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
  CHECK(body()->wait_list.flink_ptr != kXObjSignature);
  auto replacement = Native();
  REQUIRE(replacement != nullptr);
  CHECK(replacement.get() != object.get());
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "A retained guest object reopens with unreadable metadata",
                 "[kernel][ob]") {
  const X_HANDLE handle = Open();
  auto object = Native();
  REQUIRE(object != nullptr);
  REQUIRE(xboxkrnl::NtClose(handle) == X_STATUS_SUCCESS);
  CHECK_FALSE(kernel_state()->object_table()->LookupObject<XObject>(handle));
  REQUIRE(memory()->LookupHeap(kType)->Decommit(kType, 0x1000));

  REQUIRE(Open() == handle);
  CHECK(kernel_state()->object_table()->LookupObject<XObject>(handle).get() ==
        object.get());
  REQUIRE(xboxkrnl::NtClose(handle) == X_STATUS_SUCCESS);
  CHECK(xboxkrnl::NtClose(handle) == X_STATUS_INVALID_HANDLE);
  CHECK(Native().get() == object.get());
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  CHECK(object->handles().empty());
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "A stale native stash cannot resolve a different object",
                 "[kernel][ob]") {
  // Remove the kernel's bootstrap event so both allocations use the same slot.
  kernel_state()->object_table()->Reset();
  auto original = Native();
  REQUIRE(original != nullptr);
  const X_HANDLE old_handle = original->handle();
  REQUIRE(xboxkrnl::NtClose(old_handle) == X_STATUS_SUCCESS);
  kernel_state()->object_table()->Reset();
  constexpr uint32_t other_body = kBody + 0x100;
  auto* other_header =
      memory()->TranslateVirtual<X_DISPATCH_HEADER*>(other_body);
  other_header->type = EventNotificationObject;
  auto other = XObject::GetNativeObject(kernel_state(), other_header);
  REQUIRE(other != nullptr);
  REQUIRE(other->handle() == old_handle);

  body()->wait_list.flink_ptr = kXObjSignature;
  body()->wait_list.blink_ptr = old_handle;
  auto replacement = Native();
  CHECK_FALSE(replacement);
  CHECK(other->guest_object() == other_body);
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "Releasing a creation reference leaves its opened handle live",
                 "[kernel][ob]") {
  const X_HANDLE handle = Open();
  auto object = Native();
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  REQUIRE(kernel_state()->object_table()->LookupObject<XObject>(handle) !=
          nullptr);
  REQUIRE(xboxkrnl::NtClose(handle) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
  CHECK(body()->wait_list.flink_ptr != kXObjSignature);
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "Duplicate close source accepts combined option flags",
                 "[kernel][ob]") {
  const X_HANDLE first = Open();
  auto object = Native();
  REQUIRE(Call(::ordinals::NtDuplicateObject, {first, kOutput, 3}) ==
          X_STATUS_SUCCESS);
  const X_HANDLE duplicate = output();
  CHECK(duplicate == first);
  CHECK(kernel_state()->object_table()->LookupObject<XObject>(duplicate) !=
        nullptr);
  CHECK(Native().get() == object.get());
  CHECK(xboxkrnl::NtClose(duplicate) == X_STATUS_SUCCESS);
  CHECK(xboxkrnl::NtClose(first) == X_STATUS_INVALID_HANDLE);
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  CHECK(object->handles().empty());
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "Duplicate close source keeps the object without a creation "
                 "reference",
                 "[kernel][ob]") {
  const X_HANDLE first = Open();
  auto object = Native();
  REQUIRE(object != nullptr);
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);

  REQUIRE(Call(::ordinals::NtDuplicateObject, {first, kOutput, 1}) ==
          X_STATUS_SUCCESS);
  const X_HANDLE replacement = output();
  CHECK(replacement == first);
  CHECK(Native().get() == object.get());
  REQUIRE(xboxkrnl::NtClose(replacement) == X_STATUS_SUCCESS);
  CHECK(object->handles().empty());
  CHECK(body()->wait_list.flink_ptr != kXObjSignature);
}

TEST_CASE_METHOD(ObjectManagerTest,
                 "Duplicate failure and null output retain their API behavior",
                 "[kernel][ob]") {
  CHECK(Call(::ordinals::NtDuplicateObject,
             {X_INVALID_HANDLE_VALUE, kOutput, 3}) == X_STATUS_INVALID_HANDLE);
  CHECK(output() == X_INVALID_HANDLE_VALUE);
  CHECK(Call(::ordinals::NtDuplicateObject, {X_INVALID_HANDLE_VALUE, 0, 0}) ==
        X_STATUS_INVALID_HANDLE);

  const X_HANDLE first = Open();
  auto object = Native();
  REQUIRE(Call(::ordinals::NtDuplicateObject, {first, 0, 1}) ==
          X_STATUS_SUCCESS);
  REQUIRE(object->handles().size() == 1);
  CHECK(kernel_state()->object_table()->LookupObject<XObject>(first) !=
        nullptr);
  CHECK(xboxkrnl::NtClose(first) == X_STATUS_SUCCESS);
  xboxkrnl::xeObDereferenceObject(nullptr, kBody);
  CHECK(object->handles().empty());
}

}  // namespace test
}  // namespace kernel
}  // namespace xe
