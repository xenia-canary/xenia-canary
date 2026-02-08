/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XNET_H_
#define XENIA_KERNEL_XNET_H_

#include "xenia/base/byte_order.h"
#include "xenia/base/literals.h"
#include "xenia/kernel/util/xfiletime.h"

namespace xe {
using namespace xe::literals;

// clang-format off

// https://github.com/davispuh/XLiveServices/blob/master/lib/xlive_services/hresult.rb

#define X_ONLINE_E_BASE                                     static_cast<X_HRESULT>(0x80150000L)

#define X_ONLINE_S_LOGON_DISCONNECTED                       static_cast<X_HRESULT>(0x001510F1L)
#define X_ONLINE_E_LOGON_NOT_LOGGED_ON                      static_cast<X_HRESULT>(0x80151802L) // ERROR_CONNECTION_INVALID
#define X_ONLINE_E_MESSAGE_PROPERTY_NOT_FOUND               static_cast<X_HRESULT>(0x80155a03L)
#define X_ONLINE_E_NOTIFICATION_NOT_INITIALIZED             static_cast<X_HRESULT>(0x8015200DL)

#define X_PARTY_E_NOT_IN_PARTY                              static_cast<X_HRESULT>(0x807D0003L)

#define X_ONLINE_MAX_FRIENDS                                100
#define X_MAX_RICHPRESENCE_SIZE                             64

// clang-format on

namespace kernel {

struct XNKID {
  uint8_t ab[8];
  uint64_t as_uint64() { return *reinterpret_cast<uint64_t*>(&ab); }
  uint64_t as_uintBE64() { return xe::byte_swap(as_uint64()); }
};
static_assert_size(XNKID, 0x8);

#pragma region XLiveBase

struct X_ARGUMENT_ENTRY {
  xe::be<uint32_t> native_size;  // 4
  xe::be<uint64_t> argument_value_ptr;
};
static_assert_size(X_ARGUMENT_ENTRY, 0x10);

#pragma pack(push, 4)

struct X_MUTE_SET_STATE {
  xe::be<uint32_t> user_index;
  xe::be<uint64_t> remote_xuid;
  xe::be<uint32_t> set_muted;
};

struct X_PRESENCE_CREATE_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY num_peers;
  X_ARGUMENT_ENTRY peer_xuids_ptr;
  X_ARGUMENT_ENTRY starting_index;
  X_ARGUMENT_ENTRY max_peers;
  X_ARGUMENT_ENTRY buffer_length_ptr;      // output
  X_ARGUMENT_ENTRY enumerator_handle_ptr;  // output
  X_ARGUMENT_ENTRY reserved[25];
  xe::be<uint32_t> argument_count;
};
static_assert_size(X_PRESENCE_CREATE_ENUMERATOR, 0x204);

struct X_ONLINE_PRESENCE {
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> state;
  XNKID session_id;
  xe::be<uint32_t> title_id;
  X_FILETIME state_change_time;
  xe::be<uint32_t> cchRichPresence;
  xe::be<char16_t> wszRichPresence[X_MAX_RICHPRESENCE_SIZE];
};
static_assert_size(X_ONLINE_PRESENCE, 0xA4);

struct X_CREATE_FRIENDS_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY friends_starting_index;
  X_ARGUMENT_ENTRY friends_amount;
  X_ARGUMENT_ENTRY buffer_ptr;
  X_ARGUMENT_ENTRY handle_ptr;
  X_ARGUMENT_ENTRY reserved[27];
  xe::be<uint32_t> argument_count;
};
static_assert_size(X_CREATE_FRIENDS_ENUMERATOR, 0x204);

struct X_ONLINE_FRIEND {
  xe::be<uint64_t> xuid;
  char Gamertag[16];
  xe::be<uint32_t> state;
  XNKID session_id;
  xe::be<uint32_t> title_id;
  X_FILETIME ftUserTime;
  XNKID xnkidInvite;
  X_FILETIME gameinviteTime;
  xe::be<uint32_t> cchRichPresence;
  xe::be<char16_t> wszRichPresence[X_MAX_RICHPRESENCE_SIZE];
};
static_assert_size(X_ONLINE_FRIEND, 0xC4);

#pragma pack(pop)

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XNET_H_
