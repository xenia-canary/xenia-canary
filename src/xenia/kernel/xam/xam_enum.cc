/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/xbox.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {
namespace xam {

uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, lpvoid_t buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned,
                        uint32_t overlapped_ptr) {
  auto e = kernel_state()->object_table()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr, buffer_size, overlapped_ptr](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result =
          e->WriteItems(buffer_ptr.as<uint8_t*>(), buffer_size, &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

dword_result_t XamEnumerate_entry(dword_t handle, dword_t flags,
                                  lpvoid_t buffer, dword_t buffer_length,
                                  lpdword_t items_returned,
                                  pointer_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result = xeXamEnumerate(handle, flags, buffer, buffer_length,
                               !overlapped ? &dummy : nullptr, overlapped);
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamEnumerate, kNone, kImplemented);

static uint32_t XMPCreateUserPlaylistEnumeratorHandle(
    uint32_t user_index, uint32_t app_id, uint32_t open_message,
    uint32_t close_message, uint32_t extra_size, uint32_t item_count,
    uint32_t flags, uint32_t& out_enum_handle) {
  auto e = object_ref<XMPCreateUserPlaylistEnumerator>(
      new XMPCreateUserPlaylistEnumerator(kernel_state(), item_count));

  auto result = e->Initialize(user_index, app_id, open_message, close_message,
                              flags, extra_size, nullptr);

  if (XFAILED(result)) {
    return result;
  }

  std::vector<xam::XMP_USER_PLAYLIST_INFO> user_playlist_info = {};

  for (const auto& playlist_info : user_playlist_info) {
    e->AppendItem(playlist_info);
  }

  XELOGI("{}: added {} items to enumerator", __func__, e->item_count());

  out_enum_handle = e->handle();
  return X_ERROR_SUCCESS;
}

constexpr uint32_t XMPCreateUserPlaylistEnumeratorMessage = 0x70026;

dword_result_t XamCreateEnumeratorHandle_entry(
    dword_t user_index, dword_t app_id, dword_t open_message,
    dword_t close_message, dword_t extra_size, dword_t item_count,
    dword_t flags, lpdword_t out_handle) {
  uint32_t enum_handle = 0;
  X_STATUS result = 0;

  switch (open_message) {
    case XMPCreateUserPlaylistEnumeratorMessage: {
      result = XMPCreateUserPlaylistEnumeratorHandle(
          user_index, app_id, open_message, close_message, extra_size,
          item_count, flags, enum_handle);
    } break;
    default: {
      auto e = object_ref<XStaticUntypedEnumerator>(
          new XStaticUntypedEnumerator(kernel_state(), item_count, extra_size));

      result = e->Initialize(user_index, app_id, open_message, close_message,
                             flags, extra_size, nullptr);
      enum_handle = e->handle();
    } break;
  }

  if (XFAILED(result)) {
    return result;
  }

  *out_handle = enum_handle;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCreateEnumeratorHandle, kNone, kImplemented);

dword_result_t XamGetPrivateEnumStructureFromHandle_entry(
    dword_t handle, lpdword_t out_object_ptr) {
  auto e = kernel_state()->object_table()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetPrivateEnumStructureFromHandle, kNone, kImplemented);

dword_result_t XamProfileCreateEnumerator_entry(dword_t device_id,
                                                lpdword_t handle_ptr) {
  if (!handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto e = new XStaticEnumerator<X_PROFILEENUMRESULT>(kernel_state(), 1);

  auto result =
      e->Initialize(XUserIndexAny, 0xFE, 0x23001, 0x23003, 0, 0x28, nullptr);

  if (XFAILED(result)) {
    return result;
  }

  const auto& accounts =
      kernel_state()->xam_state()->profile_manager()->GetAccounts();

  for (const auto& [xuid, account] : *accounts) {
    X_PROFILEENUMRESULT* profile = e->AppendItem();

    profile->xuid_offline = xuid;
    profile->device_id = 1;
    memcpy(&profile->account, &account, sizeof(X_XAMACCOUNTINFO));
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileCreateEnumerator, kNone, kImplemented);

dword_result_t XamProfileEnumerate_entry(dword_t handle, dword_t flags,
                                         lpvoid_t buffer,
                                         pointer_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy = 0;
  auto result = xeXamEnumerate(handle, flags, buffer, 0,
                               !overlapped ? &dummy : nullptr, overlapped);

  return result;
}
DECLARE_XAM_EXPORT1(XamProfileEnumerate, kNone, kImplemented);

dword_result_t EnumerateMediaObjects_entry() { return X_E_NOT_IMPLEMENTED; }
DECLARE_XAM_EXPORT1(EnumerateMediaObjects, kNone, kStub);

dword_result_t EnumerateMediaObjects__entry() { return X_E_NOT_IMPLEMENTED; }
DECLARE_XAM_EXPORT1(EnumerateMediaObjects_, kNone, kStub);

dword_result_t EnumerateMediaObjects_0_entry() { return X_E_NOT_IMPLEMENTED; }
DECLARE_XAM_EXPORT1(EnumerateMediaObjects_0, kNone, kStub);

dword_result_t EnumerateMediaObjects_1_entry() { return X_E_NOT_IMPLEMENTED; }
DECLARE_XAM_EXPORT1(EnumerateMediaObjects_1, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Enum);
