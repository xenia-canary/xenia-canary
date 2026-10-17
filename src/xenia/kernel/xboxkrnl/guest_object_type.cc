/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/guest_object_type.h"

#include <algorithm>

namespace xe {
namespace kernel {
namespace xboxkrnl {

bool IsTitleObjectType(uint32_t type_ptr, bool type_readable,
                       uint32_t allocate_proc, uint32_t xenia_types_begin,
                       uint32_t xenia_types_end) {
  const bool is_xenia_type =
      type_ptr >= xenia_types_begin && type_ptr < xenia_types_end;
  return type_ptr != 0 && type_readable && allocate_proc != 0 && !is_xenia_type;
}

ObjectTypeCheck CheckObjectType(uint32_t requested_type, uint32_t host_type,
                                uint32_t header_type) {
  if (header_type) {
    return header_type == requested_type ? ObjectTypeCheck::kGuestTypeMatches
                                         : ObjectTypeCheck::kMismatch;
  }
  if (host_type != 0 && host_type == requested_type) {
    return ObjectTypeCheck::kHostTypeMatches;
  }
  return host_type != 0 ? ObjectTypeCheck::kMismatch
                        : ObjectTypeCheck::kUnknown;
}

CloseProcedureAction PlanCloseProcedure(uint32_t close_proc,
                                        bool close_proc_is_code,
                                        int32_t handle_count) {
  if (close_proc == 0 || handle_count <= 0) {
    return CloseProcedureAction::kNone;
  }
  return close_proc_is_code ? CloseProcedureAction::kRun
                            : CloseProcedureAction::kRefuse;
}

StashedHandleDecision DecideStashedHandle(
    uint32_t closed_handle, std::span<const uint32_t> remaining_handles,
    bool header_has_stash, uint32_t stashed_handle,
    bool stashed_handle_wraps_same_object) {
  const bool still_open = std::ranges::find(remaining_handles, closed_handle) !=
                          remaining_handles.end();
  if (still_open || !header_has_stash || stashed_handle != closed_handle ||
      stashed_handle_wraps_same_object) {
    return {StashedHandleAction::kKeep, 0};
  }
  if (!remaining_handles.empty()) {
    return {StashedHandleAction::kRepoint, remaining_handles.front()};
  }
  return {StashedHandleAction::kReset, 0};
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
