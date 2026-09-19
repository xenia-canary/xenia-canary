/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// XamPartyPartyCreate, XamPartyLeave, XamPartySendInvite, XamPartyJoinEx, &
// XamPartyRemoveLocalUsers share one function

dword_result_t XamPartyGetUserList_entry(dword_t caller,
                                         lpvoid_t party_list_ptr) {
  if (caller != 1) {
    return X_E_NOT_IMPLEMENTED;
  }
  // 5345085D, 45410923
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserList, kNone, kStub);

// Shares same function as XamPartyAddLocalUsers, XamPartyKickUser,
// XamPartyGetJoinable, XamPartySetJoinable, & XamPartyGetInfoEx
dword_result_t XamPartySendGameInvites_entry(
    dword_t caller, dword_t user_index, dword_t unknown,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamPartySendGameInvites, kNone, kStub);

dword_result_t XamPartySetCustomData_entry(dword_t caller, dword_t user_index,
                                           lpvoid_t custom_data_ptr) {
  if (caller != 1) {
    return X_E_NOT_IMPLEMENTED;
  } else if (user_index >= XUserMaxUserCount) {
    return 0x807D0002;
  }
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartySetCustomData, kNone, kStub);

dword_result_t XamPartyGetBandwidth_entry(dword_t bandwidth_type,
                                          lpqword_t bandwidth_statstic) {
  *bandwidth_statstic = 0;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamPartyGetBandwidth, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Party);
