/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamProfileFindAccount_entry(
    qword_t offline_xuid, pointer_t<X_XAMACCOUNTINFO> account_ptr,
    lpdword_t device_id) {
  if (!account_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  account_ptr.Zero();

  const auto& account =
      kernel_state()->xam_state()->profile_manager()->GetAccount(offline_xuid);

  if (!account) {
    return X_ERROR_NO_SUCH_USER;
  }

  std::memcpy(account_ptr, &account, sizeof(X_XAMACCOUNTINFO));

  xe::string_util::copy_and_swap_truncating(
      account_ptr->gamertag, account->gamertag, sizeof(account->gamertag));

  if (device_id) {
    *device_id = 1;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileFindAccount, kUserProfiles, kImplemented);

dword_result_t XamProfileOpen_entry(
    qword_t xuid, lpstring_t mount_path, dword_t flags,
    pointer_t<XCONTENT_AGGREGATE_DATA> content_data_ptr) {
  /* Notes:
      - If xuid is not local then returns X_ERROR_INVALID_PARAMETER
  */
  if (!kernel_state()->xam_state()->profile_manager()->MountProfile(
          xuid, mount_path.value())) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (content_data_ptr) {
    content_data_ptr->content_type = XContentType::kProfile;
    content_data_ptr->device_id = 1;
    content_data_ptr->set_file_name(xe::string_util::to_hex_string(xuid));
    content_data_ptr->title_id = kDashboardID;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileOpen, kNone, kImplemented);

dword_result_t XamProfileCreate_entry(
    dword_t flags, lpdword_t device_id, qword_t xuid,
    pointer_t<X_XAMACCOUNTINFO> account,
    pointer_t<X_USER_PAYMENT_INFO> payment_info,
    pointer_t<X_PASSPORT_SESSION_TOKEN> user_token,
    pointer_t<X_PASSPORT_SESSION_TOKEN> owner_token,
    pointer_t<X_PROFILE_CREATION_INFO> profile_info_ptr) {
  if ((flags & 0x80000000) == 0x80000000) {
    profile_info_ptr->flags = flags & 0x7fffffff;
    profile_info_ptr->unk2 = 1;
  } else {
    profile_info_ptr->flags = flags;
    profile_info_ptr->unk2 = 0;
  }

  if (device_id) {
    *device_id = 0x1;
    profile_info_ptr->device_id = *device_id;
  }

  profile_info_ptr->offline_xuid = xuid;
  profile_info_ptr->account_info = *account;

  X_XAMACCOUNTINFO account_info_data;
  memcpy(&account_info_data, account, sizeof(X_XAMACCOUNTINFO));
  xe::copy_and_swap<char16_t>(account_info_data.gamertag,
                              account_info_data.gamertag, 16);

  if (payment_info) {
    profile_info_ptr->user_payment_info = *payment_info;
  }
  if (user_token) {
    profile_info_ptr->user_token = *user_token;
  }
  if (owner_token) {
    profile_info_ptr->owner_token = *owner_token;
  }

  // calls XamTaskSchedule

  bool result = kernel_state()->xam_state()->profile_manager()->CreateProfile(
      &account_info_data, xuid);

  return result ? X_ERROR_SUCCESS : X_ERROR_INVALID_PARAMETER;
}
DECLARE_XAM_EXPORT1(XamProfileCreate, kNone, kSketchy);

dword_result_t XamProfileClose_entry(lpstring_t mount_name) {
  std::string guest_name = mount_name.value();
  const bool result =
      kernel_state()->file_system()->UnregisterDevice(guest_name + ':');

  return result ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamProfileClose, kNone, kStub);

dword_result_t XamProfileGetCreationStatus_entry(
    pointer_t<X_PROFILE_CREATION_INFO> profile_info, lpqword_t offline_xuid) {
  // This is a rough outline of what's meant to happen here.
  // None of this can be done until XamTaskSchedule is handled in
  // XamProfileCreate. X_RESULT result =
  // XamTaskGetStatus(profile_info->task_handle_ptr);
  // if (result == 0) {
  // result = XamTaskGetCompletionStatus(profile_info->task_handle_ptr)
  *offline_xuid = profile_info->offline_xuid;
  // XamTaskCloseHandle(profile_info->task_handle_ptr);
  //}
  return X_ERROR_SUCCESS;  // result
}
DECLARE_XAM_EXPORT1(XamProfileGetCreationStatus, kNone, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Profile);
