/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSESSION_H_
#define XENIA_KERNEL_XSESSION_H_

namespace xe {
namespace kernel {

enum SessionFlags {
  HOST = 0x01,
  PRESENCE = 0x02,
  STATS = 0x04,
  MATCHMAKING = 0x08,
  ARBITRATION = 0x10,
  PEER_NETWORK = 0x20,
  SOCIAL_MATCHMAKING_ALLOWED = 0x80,
  INVITES_DISABLED = 0x0100,
  JOIN_VIA_PRESENCE_DISABLED = 0x0200,
  JOIN_IN_PROGRESS_DISABLED = 0x0400,
  JOIN_VIA_PRESENCE_FRIENDS_ONLY = 0x0800,
  UNKNOWN = 0x1000,  // 4156091D and 5841128F sets this flag?

  SINGLEPLAYER_WITH_STATS = PRESENCE | STATS | INVITES_DISABLED |
                            JOIN_VIA_PRESENCE_DISABLED |
                            JOIN_IN_PROGRESS_DISABLED,

  LIVE_MULTIPLAYER_STANDARD = PRESENCE | STATS | MATCHMAKING | PEER_NETWORK,
  LIVE_MULTIPLAYER_RANKED = LIVE_MULTIPLAYER_STANDARD | ARBITRATION,
  SYSTEMLINK = PEER_NETWORK,
  GROUP_LOBBY = PRESENCE | PEER_NETWORK,
  GROUP_GAME = STATS | MATCHMAKING | PEER_NETWORK,

  // HELPERS
  SYSTEMLINK_FEATURES = HOST | SYSTEMLINK,
  LIVE_FEATURES = PRESENCE | STATS | MATCHMAKING | ARBITRATION
};

inline bool IsOfflineSession(const SessionFlags flags) { return !flags; }

inline bool IsXboxLiveSession(const SessionFlags flags) {
  return !IsOfflineSession(flags) && flags & SessionFlags::LIVE_FEATURES;
}

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSESSION_H_
