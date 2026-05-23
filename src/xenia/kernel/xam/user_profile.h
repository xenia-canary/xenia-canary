/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_USER_PROFILE_H_
#define XENIA_KERNEL_XAM_USER_PROFILE_H_

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/kernel/xam/user_property.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_info_title.h"
#include "xenia/kernel/xam/xdbf/xdbf_io.h"

namespace xe {
namespace kernel {
namespace xam {

enum class X_USER_PROFILE_SETTING_SOURCE : uint32_t {
  NO_VALUE = 0,
  DEFAULT = 1,  // Default value taken from default OS values.
  TITLE = 2,    // Value written by title or OS.
  PERMISSION_DENIED = 3,
};

struct X_USER_PROFILE_SETTING {
  xe::be<X_USER_PROFILE_SETTING_SOURCE> source;
  union {
    xe::be<uint32_t> user_index;
    xe::be<uint64_t> xuid;
  };
  xe::be<uint32_t> setting_id;
  union {
    uint8_t data_bytes[sizeof(X_USER_DATA)];
    X_USER_DATA data;
  };
};
static_assert_size(X_USER_PROFILE_SETTING, 40);

enum class X_USER_PROFILE_GAMERCARD_ZONE_OPTIONS {
  GAMERCARD_ZONE_NONE,
  GAMERCARD_ZONE_RR,
  GAMERCARD_ZONE_PRO,
  GAMERCARD_ZONE_FAMILY,
  GAMERCARD_ZONE_UNDERGROUND
};

enum class XTileType {
  kAchievement,
  kGameIcon,
  kGamerTile,
  kGamerTileSmall,
  kLocalGamerTile,
  kLocalGamerTileSmall,
  kBkgnd,
  kAwardedGamerTile,
  kAwardedGamerTileSmall,
  kGamerTileByImageId,
  kPersonalGamerTile,
  kPersonalGamerTileSmall,
  kGamerTileByKey,
  kAvatarGamerTile,
  kAvatarGamerTileSmall,
  kAvatarFullBody
};

// TODO: find filenames of other tile types that are stored in profile
inline const std::map<XTileType, std::string> kTileFileNames = {
    {XTileType::kGamerTile, "tile_64.png"},
    {XTileType::kGamerTileSmall, "tile_32.png"},
    {XTileType::kPersonalGamerTile, "tile_64.png"},
    {XTileType::kPersonalGamerTileSmall, "tile_32.png"},
    {XTileType::kAvatarGamerTile, "avtr_64.png"},
    {XTileType::kAvatarGamerTileSmall, "avtr_32.png"},
};

static constexpr std::pair<uint16_t, uint16_t> kProfileIconSize = {64, 64};
static constexpr std::pair<uint16_t, uint16_t> kProfileIconSizeSmall = {32, 32};

enum class SignInState : uint32_t {
  NotSignedIn,
  SignedInLocally,  // Offline
  SignedInToLive,   // Online
};

class UserProfile {
 public:
  UserProfile(const uint64_t xuid, const X_XAMACCOUNTINFO* account_info);

  uint64_t xuid() const { return xuid_; }
  std::string name() const { return account_info_.GetGamertagString(); }
  uint32_t signin_state() const {
    return static_cast<uint32_t>(SignInState::SignedInLocally);
  };
  uint32_t type() const { return 1 | 2; /* local | online profile? */ }

  uint32_t GetReservedFlags() const {
    return account_info_.GetReservedFlags();
  };
  uint32_t GetCachedFlags() const { return account_info_.GetCachedFlags(); };
  uint32_t GetCountry() const {
    return static_cast<uint32_t>(account_info_.GetCountry());
  };
  uint32_t GetSubscriptionTier() const {
    return account_info_.GetSubscriptionTier();
  }
  uint32_t GetLanguage() const {
    return static_cast<uint32_t>(account_info_.GetLanguage());
  };

  bool IsParentalControlled() const {
    return account_info_.IsParentalControlled();
  };
  bool IsLiveEnabled() const { return account_info_.IsLiveEnabled(); }

  void ClearProfileIcon(XTileType icon_type) {
    profile_images_.erase(icon_type);
  }

  std::span<const uint8_t> GetProfileIcon(XTileType icon_type) {
    // First check if the requested type exists
    if (profile_images_.find(icon_type) != profile_images_.cend()) {
      return {profile_images_[icon_type].data(),
              profile_images_[icon_type].size()};
    }

    // If personal/local tile requested but not found, fall back to regular tile
    if (icon_type == XTileType::kPersonalGamerTile ||
        icon_type == XTileType::kLocalGamerTile) {
      icon_type = XTileType::kGamerTile;
    } else if (icon_type == XTileType::kPersonalGamerTileSmall ||
               icon_type == XTileType::kLocalGamerTileSmall) {
      icon_type = XTileType::kGamerTileSmall;
    }

    // Try again with the fallback type
    if (profile_images_.find(icon_type) == profile_images_.cend()) {
      return {};
    }

    return {profile_images_[icon_type].data(),
            profile_images_[icon_type].size()};
  }

  void GetPasscode(uint16_t* passcode) const {
    std::memcpy(passcode, account_info_.passcode,
                sizeof(account_info_.passcode));
  };

  // Public accessor for dashboard GPD (for getting title paths)
  const GpdInfoProfile& dashboard_gpd() const { return dashboard_gpd_; }

  // Public accessor for getting title icon from title GPD
  std::vector<uint8_t> GetTitleIcon(uint32_t title_id) const {
    const GpdInfo* gpd = GetGpd(title_id);
    if (!gpd) {
      return {};
    }
    auto icon_data = gpd->GetImage(kXdbfIdTitle);
    return std::vector<uint8_t>(icon_data.begin(), icon_data.end());
  }

  // Public accessor for getting achievement stats from title GPD
  struct TitleAchievementStats {
    uint32_t achievements_total = 0;
    uint32_t achievements_unlocked = 0;
    uint32_t gamerscore_total = 0;
    uint32_t gamerscore_earned = 0;
  };

  TitleAchievementStats GetTitleAchievementStats(uint32_t title_id) const {
    TitleAchievementStats stats;

    // Get the GPD for this title
    auto it = games_gpd_.find(title_id);
    if (it == games_gpd_.end()) {
      return stats;
    }

    const GpdInfoTitle& title_gpd = it->second;
    if (!title_gpd.IsValid()) {
      return stats;
    }

    stats.achievements_total = title_gpd.GetAchievementCount();
    stats.achievements_unlocked = title_gpd.GetUnlockedAchievementCount();
    stats.gamerscore_total = title_gpd.GetTotalGamerscore();
    stats.gamerscore_earned = title_gpd.GetGamerscore();

    return stats;
  }

  friend class UserTracker;
  friend class GpdAchievementBackend;
  friend class ProfileManager;

 private:
  uint64_t xuid_;
  X_XAMACCOUNTINFO account_info_;

  GpdInfoProfile dashboard_gpd_;
  std::map<uint32_t, GpdInfoTitle> games_gpd_;
  std::vector<Property> properties_;  // Includes contexts!

  std::map<XTileType, std::vector<uint8_t>> profile_images_;

  GpdInfo* GetGpd(const uint32_t title_id);
  const GpdInfo* GetGpd(const uint32_t title_id) const;

  void LoadProfileGpds();
  void LoadProfileIcon(XTileType tile_type);
  void WriteProfileIcon(XTileType tile_type,
                        std::span<const uint8_t> icon_data);
  std::vector<uint8_t> LoadGpd(const uint32_t title_id);
  bool WriteGpd(const uint32_t title_id);
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_USER_PROFILE_H_
