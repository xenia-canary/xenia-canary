/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/guest_object_type.h"

#include <cstdint>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace test {

// The NUI runtime linked into 4D5308ED defines its image stream type
// at 0x8381E234 (pool tag "Nuio"), with its allocate procedure at 0x82D29DB0
// and its close procedure at 0x82D2AB00, and creates its stream objects with
// ObCreateObject.
constexpr uint32_t kNuiStreamType = 0x8381E234;
constexpr uint32_t kNuiStreamAllocateProc = 0x82D29DB0;
constexpr uint32_t kNuiStreamCloseProc = 0x82D2AB00;
// Any other type a title could define.
constexpr uint32_t kOtherTitleType = 0x83000100;
// Where the kernel's guest globals hold xenia's ten object types, 0x1C bytes
// each, and a type among them, standing for ExEventObjectType, with the
// trampoline its allocate procedure is.
constexpr uint32_t kXeniaTypesBegin = 0x80040000;
constexpr uint32_t kXeniaTypesEnd = kXeniaTypesBegin + 10 * 0x1C;
constexpr uint32_t kXeniaEventType = kXeniaTypesBegin + 0x1C;
constexpr uint32_t kXeniaAllocateTrampoline = 0x80070000;
// The private type xenia writes into a header it allocated itself.
constexpr uint32_t kXeniaPrivateType = 0x80041000;

bool IsTitleType(uint32_t type_ptr, bool type_readable,
                 uint32_t allocate_proc) {
  return IsTitleObjectType(type_ptr, type_readable, allocate_proc,
                           kXeniaTypesBegin, kXeniaTypesEnd);
}

TEST_CASE("A type the title defined is told from xenia's own", "[kernel][ob]") {
  CHECK(IsTitleType(kNuiStreamType, true, kNuiStreamAllocateProc));
  // No readable header, so no type.
  CHECK_FALSE(IsTitleType(0, false, 0));
  // A header naming a type that cannot be read is not one ObCreateObject
  // wrote.
  CHECK_FALSE(IsTitleType(kNuiStreamType, false, kNuiStreamAllocateProc));
  // The zero-filled type of a header xenia allocated itself is none of the
  // types in the kernel's guest globals, and has no allocate procedure.
  CHECK_FALSE(IsTitleType(kXeniaPrivateType, true, 0));
}

TEST_CASE("Every one of xenia's own object types keeps today's behaviour",
          "[kernel][ob]") {
  // All ten, not only the six xenia maps its host types to: each has an
  // allocate procedure of its own.
  for (uint32_t type = kXeniaTypesBegin; type < kXeniaTypesEnd; type += 0x1C) {
    CHECK_FALSE(IsTitleType(type, true, kXeniaAllocateTrampoline));
  }
  CHECK_FALSE(IsTitleType(kXeniaEventType, true, kXeniaAllocateTrampoline));
  // The first address past the block is not xenia's.
  CHECK(IsTitleType(kXeniaTypesEnd, true, kNuiStreamAllocateProc));
  CHECK(IsTitleType(kXeniaTypesBegin - 0x1C, true, kNuiStreamAllocateProc));
}

TEST_CASE("A recorded guest type takes precedence over the host wrapper",
          "[kernel][ob]") {
  // A host-native event, referenced as one.
  CHECK(CheckObjectType(kXeniaEventType, kXeniaEventType, 0) ==
        ObjectTypeCheck::kHostTypeMatches);
  // Wrapping a title-defined stream as an event must not make it an event.
  CHECK(CheckObjectType(kXeniaEventType, kXeniaEventType, kNuiStreamType) ==
        ObjectTypeCheck::kMismatch);

  // A NUI stream object: xenia wraps it as an event, and the runtime asks for
  // its own type. Its header is what names that type.
  CHECK(CheckObjectType(kNuiStreamType, kXeniaEventType, kNuiStreamType) ==
        ObjectTypeCheck::kGuestTypeMatches);
  // The same with a host type xenia has no guest type for.
  CHECK(CheckObjectType(kNuiStreamType, 0, kNuiStreamType) ==
        ObjectTypeCheck::kGuestTypeMatches);

  // Asked for a type that is neither xenia's nor the header's.
  CHECK(CheckObjectType(kOtherTitleType, kXeniaEventType, kNuiStreamType) ==
        ObjectTypeCheck::kMismatch);
  // Without a header the answer is today's.
  CHECK(CheckObjectType(kNuiStreamType, kXeniaEventType, 0) ==
        ObjectTypeCheck::kMismatch);
  CHECK(CheckObjectType(kNuiStreamType, 0, 0) == ObjectTypeCheck::kUnknown);
  CHECK(CheckObjectType(kOtherTitleType, 0, kNuiStreamType) ==
        ObjectTypeCheck::kMismatch);
}

TEST_CASE("NtClose runs a close procedure on every close of a live handle",
          "[kernel][ob]") {
  // The last handle: the NUI runtime's close procedure tests for exactly 1
  // before it tears the stream down.
  CHECK(PlanCloseProcedure(kNuiStreamCloseProc, true, 1) ==
        CloseProcedureAction::kRun);
  // Not the last: the console calls it all the same, and the procedure
  // returns early by itself.
  CHECK(PlanCloseProcedure(kNuiStreamCloseProc, true, 2) ==
        CloseProcedureAction::kRun);
  // A type without a close procedure.
  CHECK(PlanCloseProcedure(0, false, 1) == CloseProcedureAction::kNone);
  // A handle that no longer names the object.
  CHECK(PlanCloseProcedure(kNuiStreamCloseProc, true, 0) ==
        CloseProcedureAction::kNone);
  CHECK(PlanCloseProcedure(kNuiStreamCloseProc, true, -1) ==
        CloseProcedureAction::kNone);
}

TEST_CASE("NtClose refuses a close procedure that is not guest code",
          "[kernel][ob]") {
  // A header read in front of an object ObCreateObject did not make can name
  // anything; its "close procedure" is only called if it is code.
  CHECK(PlanCloseProcedure(0x41576000, false, 1) ==
        CloseProcedureAction::kRefuse);
  CHECK(PlanCloseProcedure(0x41576000, false, 2) ==
        CloseProcedureAction::kRefuse);
  // With nothing to close there is nothing to refuse either.
  CHECK(PlanCloseProcedure(0x41576000, false, 0) ==
        CloseProcedureAction::kNone);
}

constexpr uint32_t kClosedHandle = 0xF8000288;
constexpr uint32_t kOtherHandle = 0xF8000290;

TEST_CASE("A closed handle's stash is left alone while it is not stale",
          "[kernel][ob]") {
  const std::vector<uint32_t> still_open = {kClosedHandle};
  const std::vector<uint32_t> none;
  // The handle still has references.
  CHECK(
      DecideStashedHandle(kClosedHandle, still_open, true, kClosedHandle, false)
          .action == StashedHandleAction::kKeep);
  // The title re-initialised the object in place, so the header carries no
  // stash of xenia's any more.
  CHECK(DecideStashedHandle(kClosedHandle, none, false, 0, false).action ==
        StashedHandleAction::kKeep);
  // The stash already names another handle.
  CHECK(DecideStashedHandle(kClosedHandle, none, true, kOtherHandle, false)
            .action == StashedHandleAction::kKeep);
  // The slot went to a new wrapper of the same object, which stashed the same
  // value: that stash is live.
  CHECK(DecideStashedHandle(kClosedHandle, none, true, kClosedHandle, true)
            .action == StashedHandleAction::kKeep);
}

TEST_CASE("A stale stash is pointed at a live handle or cleared",
          "[kernel][ob]") {
  const std::vector<uint32_t> duplicate = {kOtherHandle};
  const std::vector<uint32_t> none;
  // The object still has a handle, from NtDuplicateObject: the stash follows
  // it.
  const auto repoint =
      DecideStashedHandle(kClosedHandle, duplicate, true, kClosedHandle, false);
  CHECK(repoint.action == StashedHandleAction::kRepoint);
  CHECK(repoint.handle == kOtherHandle);
  // The last handle is gone, and the slot is free or belongs to another
  // object: the next kernel call wraps the object afresh.
  CHECK(DecideStashedHandle(kClosedHandle, none, true, kClosedHandle, false)
            .action == StashedHandleAction::kReset);
}

}  // namespace test
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
