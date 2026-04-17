/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_H_
#define XENIA_KERNEL_XAM_XAM_H_

#include <string_view>

#include "xenia/base/byte_order.h"
#include "xenia/base/string.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

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
    kSubscriptionTierNone = 0,
    kSubscriptionTierSilver = 3,
    kSubscriptionTierGold = 6,
    kSubscriptionTierFamilyGold = 9
  };

  enum AccountLiveFlags { kAcctRequiresManagement = 1 };

  be<uint32_t> reserved_flags;
  be<uint32_t> live_flags;
  char16_t gamertag[0x10];
  be<uint64_t> xuid_online;  // 09....
  be<uint32_t> cached_user_flags;
  be<uint32_t> network_id;
  char passcode[4];
  char online_domain[0x14];
  char online_kerberos_realm[0x18];
  char online_key[0x10];
  char passport_membername[0x72];
  char passport_password[0x20];
  char owner_passport_membername[0x72];

  bool IsPasscodeEnabled() const {
    return static_cast<bool>(reserved_flags &
                             AccountReservedFlags::kPasswordProtected);
  }

  bool IsLiveEnabled() const {
    return static_cast<bool>(reserved_flags &
                             AccountReservedFlags::kLiveEnabled);
  }

  uint64_t GetOnlineXUID() const { return xuid_online; }

  std::string_view GetOnlineDomain() const {
    return std::string_view(online_domain);
  }

  uint32_t GetReservedFlags() const { return reserved_flags; };
  uint32_t GetCachedFlags() const { return cached_user_flags; };

  XOnlineCountry GetCountry() const {
    return static_cast<XOnlineCountry>((cached_user_flags & kCountryMask) >> 8);
  }

  AccountSubscriptionTier GetSubscriptionTier() const {
    return static_cast<AccountSubscriptionTier>(
        (cached_user_flags & kSubscriptionTierMask) >> 20);
  }

  bool IsParentalControlled() const {
    return static_cast<bool>((cached_user_flags & kLanguageMask) >> 24);
  }

  XLanguage GetLanguage() const {
    return static_cast<XLanguage>((cached_user_flags & kLanguageMask) >> 25);
  }

  std::string GetGamertagString() const {
    return xe::to_utf8(xe::string_util::read_u16string_and_swap(gamertag));
  }

  void ToggleLiveFlag(bool is_live) {
    reserved_flags = reserved_flags & ~AccountReservedFlags::kLiveEnabled;

    if (is_live) {
      reserved_flags = reserved_flags | AccountReservedFlags::kLiveEnabled;
    }
  }

  void SetCountry(XOnlineCountry country) {
    cached_user_flags = cached_user_flags & ~kCountryMask;
    cached_user_flags = cached_user_flags |
                        (static_cast<uint32_t>(country) << 8) & kCountryMask;
  }

  void SetLanguage(XLanguage language) {
    cached_user_flags = cached_user_flags & ~kLanguageMask;

    cached_user_flags = cached_user_flags |
                        (static_cast<uint32_t>(language) << 25) & kLanguageMask;
  }

  void SetSubscriptionTier(AccountSubscriptionTier sub_tier) {
    cached_user_flags = cached_user_flags & ~kSubscriptionTierMask;

    cached_user_flags =
        cached_user_flags |
        (static_cast<uint32_t>(sub_tier) << 20) & kSubscriptionTierMask;
  }
};
static_assert_size(X_XAMACCOUNTINFO, 0x17C);

#define X_USER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY 0x00000001
#define X_USER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY 0x00000002

#define MAX_FIRSTNAME_SIZE 64
#define MAX_LASTNAME_SIZE 64
#define MAX_EMAIL_SIZE 129
#define MAX_STREET_SIZE 128
#define MAX_CITY_SIZE 64
#define MAX_DISTRICT_SIZE 64
#define MAX_STATE_SIZE 64
#define MAX_POSTALCODE_SIZE 16
#define MAX_PHONE_PREFIX_SIZE 12
#define MAX_PHONE_NUMBER_SIZE 12
#define MAX_PHONE_EXTENSION_SIZE 12
#define MAX_CC_NAME_SIZE 64
#define MAX_CC_NUMBER_SIZE 24
#define MAX_DD_BANK_CODE_SIZE 64
#define MAX_DD_BRANCH_CODE_SIZE 64
#define MAX_DD_CHECK_DIGITS_SIZE 64
#define MAX_VOUCHER_SIZE 26

struct X_USER_PAYMENT_INFO {
  char16_t FirstName[MAX_FIRSTNAME_SIZE];
  char16_t LastName[MAX_LASTNAME_SIZE];
  char16_t Street1[MAX_STREET_SIZE];
  char16_t Street2[MAX_STREET_SIZE];
  char16_t District[MAX_STREET_SIZE];
  char16_t City[MAX_CITY_SIZE];
  char16_t State[MAX_STATE_SIZE];
  uint8_t CountryId;
  uint16_t LanguageId;
  char16_t PostalCode[MAX_POSTALCODE_SIZE];
  char16_t PhonePrefix[MAX_PHONE_PREFIX_SIZE];
  char16_t PhoneNumber[MAX_PHONE_NUMBER_SIZE];
  char16_t PhoneExtension[MAX_PHONE_EXTENSION_SIZE];

  uint8_t PaymentTypeId;
  char16_t CardHolder[MAX_CC_NAME_SIZE];
  uint8_t CardTypeId;
  char16_t CardNumber[MAX_CC_NUMBER_SIZE];
  be<uint64_t> ftCardExpiration;

  char16_t Email[MAX_EMAIL_SIZE];
  char16_t BankCode[MAX_DD_BANK_CODE_SIZE];
  char16_t BranchCode[MAX_DD_BRANCH_CODE_SIZE];
  char16_t CheckDigits[MAX_DD_CHECK_DIGITS_SIZE];

  char16_t Voucher[MAX_VOUCHER_SIZE];

  uint8_t MsftOptIn;
  uint8_t PartnerOptIn;
  uint64_t OfferId;
  be<uint64_t> ftBirthdate;
};
static_assert_size(X_USER_PAYMENT_INFO, 0x8F0);

struct X_PROFILEENUMRESULT {
  xe::be<uint64_t> xuid_offline;  // E0.....
  X_XAMACCOUNTINFO account;
  xe::be<uint32_t> device_id;
};
static_assert_size(X_PROFILEENUMRESULT, 0x188);

struct X_DASH_APP_INFO {
  uint64_t unk1;
  uint32_t unk2;
};
static_assert_size(X_DASH_APP_INFO, 0xC);

struct X_DASH_BACKSTACK_DATA {
  uint8_t unk1[0x314];
};
static_assert_size(X_DASH_BACKSTACK_DATA, 0x314);

struct X_GUID {
  xe::be<uint32_t> Data1;
  xe::be<uint16_t> Data2;
  xe::be<uint16_t> Data3;
  uint8_t Data4[8];
};
static_assert_size(X_GUID, 0x10);

struct X_PASSPORT_SESSION_TOKEN {
  uint8_t SessionToken[28];
};
static_assert_size(X_PASSPORT_SESSION_TOKEN, 0x1C);

#pragma pack(pop)

struct X_USER_SIGNIN_INFO {
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> signin_state;
  xe::be<uint32_t> guest_num;
  xe::be<uint32_t> sponsor_user_index;
  char name[16];
};
static_assert_size(X_USER_SIGNIN_INFO, 40);

struct X_USER_READ_PROFILE_SETTINGS {
  xe::be<uint32_t> setting_count;
  xe::be<uint32_t> settings_ptr;
};
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// clang-format off
#define XMBox_NOICON                0x00000000
#define XMBox_ERRORICON             0x00000001
#define XMBox_WARNINGICON           0x00000002
#define XMBox_ALERTICON             0x00000003

#define XMBox_PASSCODEMODE          0x00010000
#define XMBox_VERIFYPASSCODEMODE    0x00020000

#define XMBox_WAITANIMATION         0x00001000
#define XMBox_LIVEPASSCODEMODE      0x00030000
#define XMBox_MODEMASK              0x00030000

#define XMBox_OK                    1
#define XMBox_CANCEL                2

#define X_BUTTON_PASSCODE           0x00005802
#define Y_BUTTON_PASSCODE           0x00005803
#define RIGHT_BUMPER_PASSCODE       0x00005804
#define LEFT_BUMPER_PASSCODE        0x00005805
#define LEFT_TRIGGER_PASSCODE       0x00005806
#define RIGHT_TRIGGER_PASSCODE      0x00005807
#define DPAD_UP_PASSCODE            0x00005810
#define DPAD_DOWN_PASSCODE          0x00005811
#define DPAD_LEFT_PASSCODE          0x00005812
#define DPAD_RIGHT_PASSCODE         0x00005813
// clang-format on

enum class CreateProfileUiFlags : uint32_t {
  CreateNewProfile = 0x00000000,
  SignUpForLive = 0x00000001,   // Used in NXE and kinect profile page
  RecoverProfile = 0x00000002,  // Used in NXE and kinect profile Select
};

enum class SigninUiFlags : uint32_t {
  ALL = 0x00000000,  // show all profiles with option to create one or download
  LocalSignInOnly = 0x00000001,
  ShowOnlineEnabledOnly = 0x00000002,
  AllowSignout = 0x00000004,
  NUI = 0x00000008,  // always set by XamShowNuiSigninUI
  DisallowPlayAs = 0x00000010,
  AddUser = 0x00010000,
  CompleteSignIn = 0x00020000,
  ShowParentalControlledOnly = 0x00040000,
  EnableGuest = 0x00080000,
  DisallowReload = 0x00100000,
  ConvertOfflineToGuest = 0x00400000,
  Unk_1 = 0x00800000,  // used by XamShowSigninUIEx
  DisallowGuest = 0x01000000,
  Unk_2 = 0x02000000,  // used by XamShowSigninUIEx
  Unk_3 = 0x04000000,  // used by XamShowSigninUIp
  Unk_4 = 0x20000000,  // used by XamShowSigninUIp
  /* Known examples:
    - 0x04000001 // used by XamShowSigninUI
    - 0x04030000 // used by XamShowSigninUIp to login to a specific account for
    NXE and Kinect
    - 0x24030000 // used by XamShowSigninUIp
    - 0x02230002 // used by XamShowSigninUIEx
    - 0x01000002 // used by XamShowSigninUIEx
    - 0x01010002 // used by XamShowSigninUIEx
    - xbox live controls in family settings
  */
};

struct X_PROFILE_CREATION_INFO {
  uint32_t flags;
  uint32_t device_id;
  X_XAMACCOUNTINFO account_info;
  X_USER_PAYMENT_INFO user_payment_info;
  uint32_t unk;
  uint64_t offline_xuid;
  X_PASSPORT_SESSION_TOKEN user_token;
  X_PASSPORT_SESSION_TOKEN owner_token;
  uint32_t task_handle_ptr;
  uint32_t profile_creation_ptr;
};
static_assert_size(X_PROFILE_CREATION_INFO, 0xAC0);

enum class UserLogonFlags : uint32_t {
  OfflineOnly = 0x00000001,
  ForceLiveLogOff = 0x00000002,
  AddUser = 0x00000004,
  RemoveUser = 0x00000008,
  ForegroundPriority = 0x00000010,
  NoPopupNotification = 0x00000020,
  DontWaitForCompletion = 0x00000040,  // overlap related
  AllowMachineAccountOnly = 0x00000080,
  CheckOnlineTicketsOnly = 0x00000100,
  AllowDefaultUser = 0x00000200,
  AllowUsersWithRequiredMessage = 0x00000400,
  RestrictPopupNotification = 0x00000800,
  unknown_1 = 0x00002000,
  InvalidFlag = 0x00004000,  // return X_E_INVALIDARG
  /* Known examples:
    - 0x00000008 blades 6717 log out
    - 0x00000017 Blades 6717 log on
    - 0x00000023
    - 0x00000048 Blades
    - 0x00000013 Testing Network
    - 0x00000424
    - 0x00000025 NXE
    - 0x00000014 Blades OOBE profile creation
  */
};

enum class UserContextDevice : uint32_t {
  BigButton = 3,
  Microphone = 4,
};

constexpr uint32_t XMP_MAX_METADATA_STRING = 40;
constexpr uint32_t XMP_MAX_USER_PLAYLIST_ID = 572;
constexpr uint32_t XMP_USER_PLAYLIST_RESERVED_FIELD_SIZE = 168;

struct XMP_USER_PLAYLIST_INFO {
  uint8_t id[XMP_MAX_USER_PLAYLIST_ID];
  xe::be<char16_t> title[XMP_MAX_METADATA_STRING];
  uint8_t reserved[XMP_USER_PLAYLIST_RESERVED_FIELD_SIZE];
};
static_assert_size(XMP_USER_PLAYLIST_INFO, 0x334);

constexpr uint8_t kStatsMaxAmount = 64;

struct X_STATS_DETAILS {
  xe::be<uint32_t> id;
  xe::be<uint32_t> stats_amount;
  xe::be<uint16_t> stats[kStatsMaxAmount];
};
static_assert_size(X_STATS_DETAILS, 8 + kStatsMaxAmount * 2);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_H_
