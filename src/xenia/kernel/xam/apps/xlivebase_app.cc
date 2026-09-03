/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xnet.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR, implemented in netplay");
      return X_ONLINE_S_LOGON_DISCONNECTED;
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
      */
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      return X_ONLINE_E_LOGON_NOT_LOGGED_ON;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_ONLINE_E_LOGON_NOT_LOGGED_ON;
    }
    case 0x0005800E: {
      // 4D530A26
      XELOGD("XLiveBaseUserMuteListQuery({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return XUserMuteListQuery(buffer_ptr, buffer_length);
    }
    case 0x00058019: {
      // 54510846, 41560929
      XELOGD("XLiveBasePresenceCreateEnumerator({:08X}, {:08X} stubbed)",
             buffer_ptr, buffer_length);
      return XLiveBasePresenceCreateEnumerator(buffer_ptr, buffer_length);
    }
    case 0x0005801E: {
      // 54510846, 41560929, 55530874, 584112B0
      XELOGD(
          "XLiveBasePresenceSubscribe({:08X}, {:08X}, implemented in netplay)",
          buffer_ptr, buffer_length);
      return X_ONLINE_E_NOTIFICATION_NOT_INITIALIZED;
    }
    case 0x00058020: {
      XELOGD("XLiveBaseFriendsCreateEnumerator({:08X}, {:08X}) stubbed",
             buffer_ptr, buffer_length);
      return XLiveBaseFriendsCreateEnumerator(buffer_ptr, buffer_length);
    }
    case 0x00058023: {
      // Offline: 584107D7
      XELOGD(
          "XLiveBaseInviteGetAcceptedInfo({:08X}, {:08X}), implemented in "
          "netplay",
          buffer_ptr, buffer_length);
      return X_ONLINE_E_MESSAGE_PROPERTY_NOT_FOUND;
    }
    case 0x00058037: {
      XELOGD("XLiveBasePresenceInitializeLegacy({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Used in newer games such as Forza 4, MW3, FH2
      // Offline: 454109B6, 454109F4
      // Required to be successful for 4D530910 to detect signed-in profile
      XELOGD("XLiveBasePresenceInitialize({:08X}, {:08X}) unimplemented",
             buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

X_HRESULT XLiveBaseApp::XUserMuteListQuery(uint32_t buffer_ptr,
                                           uint32_t buffer_length) {
  if (!buffer_ptr || !buffer_length) {
    return X_E_INVALIDARG;
  }

  X_MUTE_SET_STATE* remote_player_ptr =
      memory_->TranslateVirtual<X_MUTE_SET_STATE*>(buffer_ptr);

  if (remote_player_ptr->user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  if (!IsOnlineXUID(remote_player_ptr->remote_xuid)) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state_->xam_state()->IsUserSignedIn(
          remote_player_ptr->user_index)) {
    return X_ONLINE_E_LOGON_NOT_LOGGED_ON;
  }

  auto user_profile =
      kernel_state_->xam_state()->GetUserProfile(remote_player_ptr->user_index);

  xe::be<uint32_t>* mute_list_ptr =
      memory_->TranslateVirtual<xe::be<uint32_t>*>(buffer_length);

  *mute_list_ptr = user_profile->IsPlayerMuted(remote_player_ptr->remote_xuid);

  return X_E_SUCCESS;
}

// Return presence information for a user's friends and subscribed peers.
X_HRESULT XLiveBaseApp::XLiveBasePresenceCreateEnumerator(
    uint32_t buffer_ptr, uint32_t buffer_length) {
  if (!buffer_ptr || !buffer_length) {
    return X_E_INVALIDARG;
  }

  Memory* memory = kernel_state_->memory();

  const X_PRESENCE_CREATE_ENUMERATOR* create_args =
      reinterpret_cast<X_PRESENCE_CREATE_ENUMERATOR*>(
          memory->TranslateVirtual(buffer_length));

  const uint32_t buffer_address =
      static_cast<uint32_t>(create_args->buffer_length_ptr.argument_value_ptr);
  const uint32_t handle_address = static_cast<uint32_t>(
      create_args->enumerator_handle_ptr.argument_value_ptr);
  uint32_t* handle_ptr = memory->TranslateVirtual<uint32_t*>(handle_address);
  uint32_t* buffer_size_ptr =
      memory->TranslateVirtual<uint32_t*>(buffer_address);

  if (create_args->argument_count < 7) {
    return X_E_INVALIDARG;
  }
  if (!handle_ptr || !buffer_size_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t max_peers = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
      static_cast<uint32_t>(create_args->max_peers.argument_value_ptr)));
  const uint32_t user_index =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
          static_cast<uint32_t>(create_args->user_index.argument_value_ptr)));

  auto e = make_object<XStaticEnumerator<X_ONLINE_PRESENCE>>(kernel_state_,
                                                             max_peers);
  auto result =
      e->Initialize(user_index, app_id(), 0x5801A, 0x5801B, 0, 0x330, nullptr);

  if (XFAILED(result)) {
    return result;
  }
  const uint32_t starting_index = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual(static_cast<uint32_t>(
          create_args->starting_index.argument_value_ptr)));
  const uint32_t xuid_address =
      static_cast<uint32_t>(create_args->peer_xuids_ptr.argument_value_ptr);
  const uint32_t num_peers =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(
          static_cast<uint32_t>(create_args->num_peers.argument_value_ptr)));

  auto object = memory->TranslateVirtual(e->guest_object());

  X_ENUMERATOR_ALLOC_PRESENCE_ENUM* args =
      reinterpret_cast<X_ENUMERATOR_ALLOC_PRESENCE_ENUM*>(object);
  args->enum_user_index = xe::byte_swap<uint32_t>(user_index);
  args->num_peers = xe::byte_swap<uint32_t>(num_peers);
  args->peer_xuids_ptr = xe::byte_swap<uint32_t>(xuid_address);
  args->starting_index = xe::byte_swap<uint32_t>(starting_index);
  args->max_peers = xe::byte_swap<uint32_t>(max_peers);

  if (starting_index > X_ONLINE_MAX_FRIENDS) {
    return X_E_INVALIDARG;
  }
  if (max_peers > X_ONLINE_MAX_FRIENDS) {
    return X_E_INVALIDARG;
  }

  const uint32_t presence_buffer_size =
      static_cast<uint32_t>(e->items_per_enumerate() * e->item_size());

  *buffer_size_ptr = xe::byte_swap<uint32_t>(presence_buffer_size);

  *handle_ptr = xe::byte_swap<uint32_t>(e->handle());

  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::XLiveBaseFriendsCreateEnumerator(
    uint32_t buffer_ptr, uint32_t buffer_length) {
  if (!buffer_ptr || !buffer_length) {
    return X_E_INVALIDARG;
  }

  Memory* memory = kernel_state_->memory();
  X_CREATE_FRIENDS_ENUMERATOR* friends_enumerator =
      memory->TranslateVirtual<X_CREATE_FRIENDS_ENUMERATOR*>(buffer_length);

  const uint32_t buffer_address =
      static_cast<uint32_t>(friends_enumerator->buffer_ptr.argument_value_ptr);
  uint32_t* buffer_size_ptr =
      memory->TranslateVirtual<uint32_t*>(buffer_address);
  const uint32_t handle_address =
      static_cast<uint32_t>(friends_enumerator->handle_ptr.argument_value_ptr);
  uint32_t* handle_ptr = memory->TranslateVirtual<uint32_t*>(handle_address);

  // 41560834 and 45410923 expect invalid handle of 0 (not -1) for failure,
  // therefore set as soon as possible.
  *handle_ptr = 0;

  if (friends_enumerator->argument_count < 5) {
    return X_E_INVALIDARG;
  }
  if (!handle_address || !buffer_size_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t friends_amount = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual(static_cast<uint32_t>(
          friends_enumerator->friends_amount.argument_value_ptr)));

  auto e = make_object<XStaticEnumerator<X_ONLINE_FRIEND>>(kernel_state_,
                                                           friends_amount);
  auto result = e->Initialize(-1, app_id(), 0x58021, 0x58022, 0, 0x10, nullptr);

  if (XFAILED(result)) {
    return result;
  }

  const uint32_t user_index = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual(static_cast<uint32_t>(
          friends_enumerator->user_index.argument_value_ptr)));

  if (user_index >= XUserMaxUserCount) {
    return X_E_INVALIDARG;
  }

  uint32_t friends_starting_index = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual(static_cast<uint32_t>(
          friends_enumerator->friends_starting_index.argument_value_ptr)));

  if (friends_starting_index >= X_ONLINE_MAX_FRIENDS) {
    return X_E_INVALIDARG;
  }

  if (friends_amount > X_ONLINE_MAX_FRIENDS) {
    return X_E_INVALIDARG;
  }

  if (!kernel_state_->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  auto const profile = kernel_state_->xam_state()->GetUserProfile(user_index);

  auto object = memory->TranslateVirtual(e->guest_object());

  X_ENUMERATOR_ALLOC_FRIENDS_ENUM* args =
      reinterpret_cast<X_ENUMERATOR_ALLOC_FRIENDS_ENUM*>(object);
  args->xuid = xe::byte_swap<uint64_t>(profile->xuid());  // online
  args->friends_amount = xe::byte_swap<uint32_t>(friends_amount);
  args->friends_starting_index =
      xe::byte_swap<uint32_t>(friends_starting_index);

  const uint32_t friends_buffer_size =
      static_cast<uint32_t>(e->items_per_enumerate() * e->item_size());

  *buffer_size_ptr = xe::byte_swap<uint32_t>(friends_buffer_size);

  *handle_ptr = xe::byte_swap<uint32_t>(e->handle());
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
