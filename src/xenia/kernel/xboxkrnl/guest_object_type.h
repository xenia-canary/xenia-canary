/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_GUEST_OBJECT_TYPE_H_
#define XENIA_KERNEL_XBOXKRNL_GUEST_OBJECT_TYPE_H_

#include <cstdint>
#include <span>

namespace xe {
namespace kernel {
namespace xboxkrnl {

// An ObCreateObject type has an allocate procedure and is distinct from the
// types xenia places in its kernel guest globals.
bool IsTitleObjectType(uint32_t type_ptr, bool type_readable,
                       uint32_t allocate_proc, uint32_t xenia_types_begin,
                       uint32_t xenia_types_end);

// Result of comparing an ObReferenceObjectByHandle request with a wrapper.
enum class ObjectTypeCheck {
  kHostTypeMatches,
  kGuestTypeMatches,
  kMismatch,
  kUnknown,
};

// A type recorded by ObOpenObjectByPointer takes precedence over the host type.
ObjectTypeCheck CheckObjectType(uint32_t requested_type, uint32_t host_type,
                                uint32_t header_type);

enum class CloseProcedureAction {
  kNone,
  kRun,
  kRefuse,
};

// The close procedure receives the number of guest opens before this close.
CloseProcedureAction PlanCloseProcedure(uint32_t close_proc,
                                        bool close_proc_is_code,
                                        int32_t handle_count);

// Repair the handle stashed in a dispatch header after a slot is released.
enum class StashedHandleAction {
  kKeep,
  kRepoint,
  kReset,
};

struct StashedHandleDecision {
  StashedHandleAction action;
  uint32_t handle;
};

// Leave a stash belonging to a new wrapper of the same guest object alone.
StashedHandleDecision DecideStashedHandle(
    uint32_t closed_handle, std::span<const uint32_t> remaining_handles,
    bool header_has_stash, uint32_t stashed_handle,
    bool stashed_handle_wraps_same_object);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_GUEST_OBJECT_TYPE_H_
