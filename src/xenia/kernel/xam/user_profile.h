/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_USER_PROFILE_H_
#define XENIA_KERNEL_XAM_USER_PROFILE_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/kernel/xam/xdbf/xdbf.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class KernelState;
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

constexpr uint32_t kMaxNumUsers = 4;
constexpr uint32_t kDashboardID = 0xFFFE07D1;

// https://github.com/jogolden/testdev/blob/master/xkelib/xam/_xamext.h#L68
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
static const std::map<XTileType, const wchar_t*> kTileFileNames = {
    {XTileType::kPersonalGamerTile, L"tile_64.png"},
    {XTileType::kPersonalGamerTileSmall, L"tile_32.png"},
    {XTileType::kAvatarGamerTile, L"avtr_64.png"},
    {XTileType::kAvatarGamerTileSmall, L"avtr_32.png"},
};

// from https://github.com/xemio/testdev/blob/master/xkelib/xam/_xamext.h
#pragma pack(push, 4)
struct X_XAMACCOUNTINFO {
  enum AccountReservedFlags {
    kPasswordProtected = 0x10000000,
    kLiveEnabled = 0x20000000,
    kRecovering = 0x40000000,
    kVersionMask = 0x000000FF
  };

  enum AccountUserFlags {
    kPaymentInstrumentCreditCard = 1,

    kCountryMask = 0xFF00,
    kSubscriptionTierMask = 0xF00000,
    kLanguageMask = 0x3E000000,

    kParentalControlEnabled = 0x1000000,
  };

  enum AccountSubscriptionTier {
    kSubscriptionTierSilver = 3,
    kSubscriptionTierGold = 6,
    kSubscriptionTierFamilyGold = 9
  };

  // already exists inside xdbf.h??
  enum AccountLanguage {
    kNoLanguage,
    kEnglish,
    kJapanese,
    kGerman,
    kFrench,
    kSpanish,
    kItalian,
    kKorean,
    kTChinese,
    kPortuguese,
    kSChinese,
    kPolish,
    kRussian,
    kNorwegian = 15
  };

  enum AccountLiveFlags { kAcctRequiresManagement = 1 };

  xe::be<uint32_t> reserved_flags;
  xe::be<uint32_t> live_flags;
  wchar_t gamertag[0x10];
  xe::be<uint64_t> xuid_online;  // 09....
  xe::be<uint32_t> cached_user_flags;
  xe::be<uint32_t> network_id;
  char passcode[4];
  char online_domain[0x14];
  char online_kerberos_realm[0x18];
  char online_key[0x10];
  char passport_membername[0x72];
  char passport_password[0x20];
  char owner_passport_membername[0x72];

  bool IsPasscodeEnabled() {
    return (bool)(reserved_flags & AccountReservedFlags::kPasswordProtected);
  }

  bool IsLiveEnabled() {
    return (bool)(reserved_flags & AccountReservedFlags::kLiveEnabled);
  }

  bool IsRecovering() {
    return (bool)(reserved_flags & AccountReservedFlags::kRecovering);
  }

  bool IsPaymentInstrumentCreditCard() {
    return (bool)(cached_user_flags &
                  AccountUserFlags::kPaymentInstrumentCreditCard);
  }

  bool IsParentalControlled() {
    return (bool)(cached_user_flags &
                  AccountUserFlags::kParentalControlEnabled);
  }

  bool IsXUIDOffline() { return ((xuid_online >> 60) & 0xF) == 0xE; }
  bool IsXUIDOnline() { return ((xuid_online >> 48) & 0xFFFF) == 0x9; }
  bool IsXUIDValid() { return IsXUIDOffline() != IsXUIDOnline(); }
  bool IsTeamXUID() {
    return (xuid_online & 0xFF00000000000140) == 0xFE00000000000100;
  }

  uint32_t GetCountry() { return (cached_user_flags & kCountryMask) >> 8; }

  AccountSubscriptionTier GetSubscriptionTier() {
    return (AccountSubscriptionTier)(
        (cached_user_flags & kSubscriptionTierMask) >> 20);
  }

  AccountLanguage GetLanguage() {
    return (AccountLanguage)((cached_user_flags & kLanguageMask) >> 25);
  }

  std::string GetGamertagString() const;
};
// static_assert_size(X_XAMACCOUNTINFO, 0x17C);
#pragma pack(pop)

class UserProfile {
 public:
  enum class UserIndex {
    kAny = 0xFF,    // applies to any or all signed-in users
    kNone = 0xFE,   // this isn't tied to any signed-in user
    kFocus = 0xFD,  // whichever user last acted / was last in focus
  };

  static void CreateUsers(KernelState* kernel_state,
                          std::unique_ptr<UserProfile>* profiles);

  // Returns map of OfflineXuid -> Path & AccountInfo pairs
  static std::map<uint64_t, std::tuple<std::wstring, X_XAMACCOUNTINFO>>
  Enumerate(KernelState* state, bool exclude_signed_in = false);

  static uint64_t XuidFromPath(const std::wstring& path);

  static bool DecryptAccountFile(const uint8_t* data, X_XAMACCOUNTINFO* output,
                                 bool devkit = false);

  static void EncryptAccountFile(const X_XAMACCOUNTINFO* input, uint8_t* output,
                                 bool devkit = false);

  static std::wstring base_path(KernelState* state);

  UserProfile(KernelState* kernel_state);

  uint64_t xuid() const { return account_.xuid_online; }
  uint64_t xuid_offline() const { return xuid_offline_; }
  std::string name() const { return account_.GetGamertagString(); }
  std::wstring path() const;
  std::wstring path(uint64_t xuid) const;
  uint32_t signin_state() const { return signin_state_; }
  void signin_state(uint32_t state) { signin_state_ = state; }
  bool signed_in() { return signin_state_ != 0 && xuid_offline_ != 0; }

  xdbf::GpdFile* SetTitleSpaData(const xdbf::SpaFile& spa_data);
  xdbf::GpdFile* GetTitleGpd(uint32_t title_id = -1);
  xdbf::GpdFile* GetDashboardGpd();

  void GetTitles(std::vector<xdbf::GpdFile*>& titles);

  bool UpdateTitleGpd(uint32_t title_id = -1);
  bool UpdateAllGpds();

  // Tries logging this user into a profile
  // If XUID is set, will try signing into the profile it belongs to
  // Otherwise will try signing into any available (not already signed in)
  // profile
  // If no profiles are available, will use a Xenia-generated one
  bool Login(uint64_t offline_xuid = 0);
  void Logout();
  bool Create(X_XAMACCOUNTINFO* account, bool generate_gamertag = false);

 private:
  // Extracts profile package if needed - returns path to extracted folder
  std::wstring ExtractProfile(const std::wstring& path);

  bool UpdateGpd(uint32_t title_id, xdbf::GpdFile& gpd_data);

  bool AddSettingIfNotExist(xdbf::Setting&& setting);

  KernelState* kernel_state_;

  std::wstring profile_path_;
  std::wstring base_path_;

  uint64_t xuid_offline_ = 0;
  uint32_t signin_state_ = 0;
  X_XAMACCOUNTINFO account_ = {0};

  std::unordered_map<uint32_t, xdbf::GpdFile> title_gpds_;
  xdbf::GpdFile dash_gpd_;
  xdbf::GpdFile* curr_gpd_ = nullptr;
  uint32_t curr_title_id_ = -1;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_USER_PROFILE_H_
