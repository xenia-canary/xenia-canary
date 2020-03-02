/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xdbf/xdbf.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xdbf {

struct X_PROFILEENUMRESULT {
  xe::be<uint64_t> xuid_offline;  // E0.....
  X_XAMACCOUNTINFO account;
  xe::be<uint32_t> device_id;
};
#if XE_PLATFORM_WIN32
static_assert_size(X_PROFILEENUMRESULT, 0x188);
#endif

dword_result_t XamProfileCreateEnumerator(dword_t device_id,
                                          lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto e =
      new XStaticEnumerator(kernel_state(), 1, sizeof(X_PROFILEENUMRESULT));

  e->Initialize();

  auto profiles = UserProfile::Enumerate(kernel_state(), false);
  for (auto profile : profiles) {
    auto result = (X_PROFILEENUMRESULT*)e->AppendItem();
    memset(result, 0, sizeof(X_PROFILEENUMRESULT));
    result->xuid_offline = profile.first;
    auto& acc = std::get<1>(profile.second);
    memcpy(&result->account, &acc, sizeof(X_XAMACCOUNTINFO));

    // tag was swapped when account got loaded in, swap it back
    xe::copy_and_swap<wchar_t>(result->account.gamertag,
                               result->account.gamertag, 0x10);

    result->device_id = 0x00000001;
  }

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileCreateEnumerator, kUserProfiles, kImplemented);

dword_result_t XamProfileEnumerate(dword_t handle, dword_t flags,
                                   lpvoid_t buffer,
                                   pointer_t<XAM_OVERLAPPED> overlapped) {
  assert_true(flags == 0);

  auto e = kernel_state()->object_table()->LookupObject<XEnumerator>(handle);
  if (!e) {
    if (overlapped) {
      kernel_state()->CompleteOverlappedImmediateEx(
          overlapped, X_ERROR_INVALID_HANDLE, X_ERROR_INVALID_HANDLE, 0);
      return X_ERROR_IO_PENDING;
    } else {
      return X_ERROR_INVALID_HANDLE;
    }
  }

  buffer.Zero(sizeof(X_PROFILEENUMRESULT));

  X_RESULT result;

  if (e->current_item() >= e->item_count()) {
    result = X_ERROR_NO_MORE_FILES;
  } else {
    auto item_buffer = buffer.as<uint8_t*>();
    if (!e->WriteItem(item_buffer)) {
      result = X_ERROR_NO_MORE_FILES;
    } else {
      result = X_ERROR_SUCCESS;
    }
  }

  // Return X_ERROR_NO_MORE_FILES in HRESULT form.
  X_HRESULT extended_result = result != 0 ? X_HRESULT_FROM_WIN32(result) : 0;
  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped, result, extended_result, result == X_ERROR_SUCCESS ? 1 : 0);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamProfileEnumerate, kUserProfiles, kImplemented);

X_HRESULT_result_t XamUserGetXUID(dword_t user_index, dword_t unk,
                                  lpqword_t xuid_ptr) {
  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_E_NO_SUCH_USER;
  }
  if (xuid_ptr) {
    *xuid_ptr = user_profile->xuid();
  }
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetXUID, kUserProfiles, kImplemented);

dword_result_t XamUserGetSigninState(dword_t user_index) {
  // Yield, as some games spam this.
  xe::threading::MaybeYield();

  // Lie and say we are signed in, but local-only.
  // This should keep games from asking us to sign in and also keep them
  // from initializing the network.

  auto user_profile = kernel_state()->user_profile(user_index);
  return user_profile ? user_profile->signin_state() : 0;
}
DECLARE_XAM_EXPORT2(XamUserGetSigninState, kUserProfiles, kImplemented,
                    kHighFrequency);

typedef struct {
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> unk08;  // maybe zero?
  xe::be<uint32_t> signin_state;
  xe::be<uint32_t> unk10;  // ?
  xe::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

X_HRESULT_result_t XamUserGetSigninInfo(dword_t user_index, dword_t flags,
                                        pointer_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));

  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_E_NO_SUCH_USER;
  }

  info->xuid = user_profile->xuid();
  info->signin_state = user_profile->signin_state();
  std::strncpy(info->name, user_profile->name().data(), 15);
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetSigninInfo, kUserProfiles, kImplemented);

dword_result_t XamUserGetName(dword_t user_index, lpstring_t buffer,
                              dword_t buffer_len) {
  if (!buffer_len) {
    return X_ERROR_SUCCESS;
  }

  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_name = user_profile->name();

  // Real XAM will only copy a maximum of 15 characters out.
  size_t copy_length = std::min(
      {size_t(15), user_name.size(), static_cast<size_t>(buffer_len) - 1});
  std::memcpy(buffer, user_name.data(), copy_length);
  buffer[copy_length] = '\0';
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetName, kUserProfiles, kImplemented);

dword_result_t XamUserGetGamerTag(dword_t user_index, lpwstring_t buffer,
                                  dword_t buffer_len) {
  if (!buffer_len) {
    return X_ERROR_SUCCESS;
  }

  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_NO_SUCH_USER;
  }

  const auto& user_name = xe::to_wstring(user_profile->name());

  size_t copy_length = std::min({size_t(buffer_len * 2), user_name.size(),
                                 static_cast<size_t>(buffer_len * 2) - 1});

  kernel_memory()->Fill(buffer, buffer_len * 2, 0x00);
  xe::copy_and_swap<wchar_t>(buffer, user_name.c_str(), copy_length);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserGetGamerTag, kUserProfiles, kImplemented);

typedef struct {
  xe::be<uint32_t> setting_count;
  xe::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

typedef struct {
  xe::be<uint32_t> from;
  union {
    xe::be<uint32_t> user_index;
    xe::be<uint64_t> user_xuid;
  } user;
  xdbf::X_XDBF_GPD_SETTING setting;
} X_USER_READ_PROFILE_SETTING;
static_assert_size(X_USER_READ_PROFILE_SETTING, 40);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
dword_result_t XamUserReadProfileSettings(
    dword_t title_id, dword_t user_index, dword_t num_xuids, lpqword_t xuids,
    dword_t setting_count, lpdword_t setting_ids, lpdword_t buffer_size_ptr,
    lpvoid_t buffer_ptr, dword_t overlapped_ptr) {
  if (!setting_count || setting_count > 0x20 || !setting_ids ||
      !buffer_size_ptr) {
    assert_always();
    // XAM doesn't seem to set the overlapped error here
    return X_ERROR_INVALID_PARAMETER;
  }

  uint64_t xuid = 0;
  if (num_xuids && xuids) {
    xuid = *xuids;
  }

  // TODO(gibbed): why is this a thing?
  uint32_t actual_user_index = user_index;
  if (actual_user_index == 255) {
    actual_user_index = 0;
  }

  const auto& user_profile = kernel_state()->user_profile(actual_user_index);
  if (!user_profile) {
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_NOT_FOUND);
    }
    return X_ERROR_NOT_FOUND;
  }

  auto gpd = user_profile->GetDashboardGpd();
  if (title_id != kDashboardID) {
    gpd = user_profile->GetTitleGpd(title_id);
  }

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // Compute required base size.
  uint32_t base_size_needed = sizeof(X_USER_READ_PROFILE_SETTINGS);
  base_size_needed += setting_count * sizeof(X_USER_READ_PROFILE_SETTING);

  // Compute required extra size.
  uint32_t size_needed = base_size_needed;
  for (uint32_t n = 0; n < setting_count; ++n) {
    auto setting_id = (xdbf::X_XDBF_SETTING_ID)(uint32_t)setting_ids[n];
    xdbf::Setting setting;
    setting.id = setting_id;

    auto this_gpd = gpd;
    // TODO: should we really be doing this for every non-specific setting?
    if (!setting.IsTitleSpecific()) {
      this_gpd = user_profile->GetDashboardGpd();
    }

    if (this_gpd && this_gpd->GetSetting(setting_id, &setting)) {
      size_needed += (uint32_t)setting.extraData.size();
    } else {
      XELOGE("XamUserReadProfileSettings requested unimplemented setting %.8X",
             setting_id);
    }
  }

  uint32_t buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size < size_needed) {
    *buffer_size_ptr = size_needed;
  }

  if (!buffer_ptr || buffer_size < size_needed) {
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_INSUFFICIENT_BUFFER);
    }
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  auto out_header = buffer_ptr.as<X_USER_READ_PROFILE_SETTINGS*>();
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = buffer_ptr.guest_address() + 8;

  auto out_setting =
      reinterpret_cast<X_USER_READ_PROFILE_SETTING*>(buffer_ptr + 8);

  uint32_t buffer_offset = base_size_needed;
  for (uint32_t n = 0; n < setting_count; ++n) {
    auto setting_id = (xdbf::X_XDBF_SETTING_ID)(uint32_t)setting_ids[n];
    xdbf::Setting setting;
    setting.id = setting_id;

    auto this_gpd = gpd;
    // TODO: should we really be doing this for every non-specific setting?
    if (!setting.IsTitleSpecific()) {
      this_gpd = user_profile->GetDashboardGpd();
    }

    bool exists = this_gpd && this_gpd->GetSetting(setting_id, &setting);

    // TODO: fix this setting causing dash.xex to crash
    // (probably makes it call into avatar code)
    if (setting.id == xdbf::XPROFILE_GAMERCARD_AVATAR_INFO_1) {
      exists = false;
    }

    std::memset(out_setting, 0, sizeof(X_USER_READ_PROFILE_SETTING));
    out_setting->from = !exists ? 0 : setting.IsTitleSpecific() ? 2 : 1;
    out_setting->setting.setting_id = setting_id;

    if (num_xuids && xuids) {
      out_setting->user.user_xuid = xuid;
    } else {
      out_setting->user.user_index = actual_user_index;
    }

    if (exists) {
      memcpy(&out_setting->setting.value, &setting.value,
             sizeof(xdbf::X_XUSER_DATA));

      if (setting.value.type == xdbf::X_XUSER_DATA_TYPE::kBinary) {
        memcpy(buffer_ptr.as<uint8_t*>() + buffer_offset,
               setting.extraData.data(), setting.extraData.size());

        out_setting->setting.value.binary.cbData =
            (uint32_t)setting.extraData.size();
        out_setting->setting.value.binary.pbData =
            buffer_ptr.guest_address() + buffer_offset;

        buffer_offset += (uint32_t)setting.extraData.size();
      } else if (setting.value.type == xdbf::X_XUSER_DATA_TYPE::kUnicode) {
        memcpy(buffer_ptr.as<uint8_t*>() + buffer_offset,
               setting.extraData.data(), setting.extraData.size());

        out_setting->setting.value.string.cbData =
            (uint32_t)setting.extraData.size();
        out_setting->setting.value.string.pwszData =
            buffer_ptr.guest_address() + buffer_offset;

        buffer_offset += (uint32_t)setting.extraData.size();
      }
    }
    ++out_setting;
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserReadProfileSettings, kUserProfiles, kImplemented);

typedef struct {
  xe::be<uint32_t> from;
  union {
    xe::be<uint32_t> user_index;
    xe::be<uint64_t> user_xuid;
  } user;

  xdbf::X_XDBF_GPD_SETTING setting;
} X_USER_WRITE_PROFILE_SETTING;
static_assert_size(X_USER_WRITE_PROFILE_SETTING, 0x28);

dword_result_t XamUserWriteProfileSettings(
    dword_t title_id, dword_t user_index, dword_t setting_count,
    pointer_t<X_USER_WRITE_PROFILE_SETTING> settings, dword_t overlapped_ptr) {
  if (!setting_count || !settings) {
    assert_always();
    // XAM doesn't seem to set the overlapped error here
    return X_ERROR_INVALID_PARAMETER;
  }

  // Update and save settings.
  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_NOT_FOUND);
    }
    return X_ERROR_NOT_FOUND;
  }

  auto gpd = user_profile->GetDashboardGpd();
  if (title_id != kDashboardID) {
    gpd = user_profile->GetTitleGpd(title_id);
  }

  if (!gpd) {
    // TODO: find out proper error code for this condition!
    if (overlapped_ptr) {
      kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                  X_ERROR_INVALID_PARAMETER);
    }
    return X_ERROR_INVALID_PARAMETER;
  }

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_WRITE_PROFILE_SETTING& settings_data = settings[n];
    XELOGD(
        "XamUserWriteProfileSettings: setting index [%d]:"
        " from=%d setting_id=%.8X data.type=%d",
        n, (uint32_t)settings_data.from,
        (uint32_t)settings_data.setting.setting_id,
        settings_data.setting.value.type);

    xdbf::Setting setting;
    setting.id = settings_data.setting.setting_id;
    setting.value.type = settings_data.setting.value.type;

    auto this_gpd = gpd;
    // TODO: should we really be doing this for every non-specific setting?
    if (!setting.IsTitleSpecific()) {
      this_gpd = user_profile->GetDashboardGpd();
    }

    // Retrieve any existing setting data if we can
    this_gpd->GetSetting(setting.id, &setting);

    // ... and then overwrite it
    memcpy(&setting.value, &settings_data.setting.value,
           sizeof(xdbf::X_XUSER_DATA));

    if (settings_data.setting.value.type == xdbf::X_XUSER_DATA_TYPE::kBinary) {
      if (settings_data.setting.value.binary.pbData) {
        setting.extraData.resize(settings_data.setting.value.binary.cbData);
        auto* data_ptr = kernel_memory()->TranslateVirtual(
            settings_data.setting.value.binary.pbData);
        memcpy(setting.extraData.data(), data_ptr,
               settings_data.setting.value.binary.cbData);
      }
    } else if (settings_data.setting.value.type ==
               xdbf::X_XUSER_DATA_TYPE::kUnicode) {
      if (settings_data.setting.value.string.pwszData) {
        setting.extraData.resize(settings_data.setting.value.string.cbData);
        auto* data_ptr = kernel_memory()->TranslateVirtual(
            settings_data.setting.value.string.pwszData);
        memcpy(setting.extraData.data(), data_ptr,
               settings_data.setting.value.string.cbData);
      }
    }

    this_gpd->UpdateSetting(setting);
  }

  user_profile->UpdateAllGpds();

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserWriteProfileSettings, kUserProfiles, kImplemented);

dword_result_t XamUserCheckPrivilege(dword_t user_index, dword_t mask,
                                     lpdword_t out_value) {
  auto* user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_NO_SUCH_USER;
  }

  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCheckPrivilege, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionGetFlags(dword_t user_index,
                                                 lpdword_t out_flags) {
  auto* user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionGetFlags, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionGetRating(dword_t user_index,
                                                  dword_t unk1,
                                                  lpdword_t out_unk2,
                                                  lpdword_t out_unk3) {
  auto* user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionGetRating, kUserProfiles, kStub);

dword_result_t XamUserContentRestrictionCheckAccess(dword_t user_index,
                                                    dword_t unk1, dword_t unk2,
                                                    dword_t unk3, dword_t unk4,
                                                    lpdword_t out_unk5,
                                                    dword_t overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserContentRestrictionCheckAccess, kUserProfiles, kStub);

dword_result_t XamUserAreUsersFriends(dword_t user_index, dword_t unk1,
                                      dword_t unk2, lpdword_t out_value,
                                      dword_t overlapped_ptr) {
  *out_value = 0;

  X_RESULT result = X_ERROR_SUCCESS;

  const auto& user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    result = X_ERROR_NOT_LOGGED_ON;
  } else {
    if (!user_profile->signin_state()) {
      result = X_ERROR_NOT_LOGGED_ON;
    } else {
      // No friends!
      *out_value = 0;
    }
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamUserAreUsersFriends, kUserProfiles, kStub);

dword_result_t XamShowSigninUI(dword_t unk, dword_t unk_mask) {
  // Mask values vary. Probably matching user types? Local/remote?
  // Games seem to sit and loop until we trigger this notification.
  kernel_state()->BroadcastNotification(0x00000009, 0);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kStub);

#pragma pack(push, 1)
struct X_XACHIEVEMENT_DETAILS {
  xe::be<uint32_t> id;
  xe::be<uint32_t> label_ptr;
  xe::be<uint32_t> description_ptr;
  xe::be<uint32_t> unachieved_ptr;
  xe::be<uint32_t> image_id;
  xe::be<uint32_t> gamerscore;
  xe::be<uint64_t> unlock_time;
  xe::be<uint32_t> flags;
};
static_assert_size(X_XACHIEVEMENT_DETAILS, 36);
#pragma pack(pop)

dword_result_t XamUserCreateAchievementEnumerator(dword_t title_id,
                                                  dword_t user_index,
                                                  dword_t xuid, dword_t flags,
                                                  dword_t offset, dword_t count,
                                                  lpdword_t buffer_size_ptr,
                                                  lpdword_t handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    // TODO: this should also happen if user_index >= 4, but dash seems to use
    // 0xFF for some reason.. maybe a problem with some other piece of code?
    return X_ERROR_INVALID_PARAMETER;
  }

  auto user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_FUNCTION_FAILED;  // TODO: proper error code!
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(X_XACHIEVEMENT_DETAILS) * count;
  }

  auto e = new XStaticEnumerator(kernel_state(), count,
                                 sizeof(X_XACHIEVEMENT_DETAILS));
  e->Initialize();

  *handle_ptr = e->handle();

  // Copy achievements into the enumerator if game GPD is loaded
  auto* game_gpd = user_profile->GetTitleGpd(title_id);
  if (!game_gpd) {
    XELOGE(
        "XamUserCreateAchievementEnumerator failed to find GPD for title %X!",
        title_id);
    return X_ERROR_FUNCTION_FAILED;
  }

  std::vector<xdbf::Achievement> achievements;
  game_gpd->GetAchievements(&achievements);

  // TODO: sort by achieved date, and so achieved come before unachieved
  // (maybe only if flags == -1?)

  for (auto ach : achievements) {
    auto* details = (X_XACHIEVEMENT_DETAILS*)e->AppendItem();
    details->id = ach.id;
    details->image_id = ach.image_id;
    details->gamerscore = ach.gamerscore;
    details->unlock_time = ach.unlock_time;
    details->flags = ach.flags;

    // TODO: these, allocating guest mem for them every CreateEnum call would be
    // very bad...

    // maybe we could alloc these in guest when the title GPD is first loaded?
    // Only the 1888 dashboard reallocates them every time
    // Newer dashes allocates this only once
    details->label_ptr =
        kernel_memory()->AllocSpaceForWStringInSystemHeap(ach.label);
    details->description_ptr =
        kernel_memory()->AllocSpaceForWStringInSystemHeap(ach.description);
    details->unachieved_ptr =
        kernel_memory()->AllocSpaceForWStringInSystemHeap(ach.unachieved_desc);
  }

  XELOGD("XamUserCreateAchievementEnumerator: added %d items to enumerator",
         e->item_count());

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateAchievementEnumerator, kUserProfiles,
                    kSketchy);

dword_result_t XamParseGamerTileKey(lpdword_t key_ptr, lpdword_t out1_ptr,
                                    lpdword_t out2_ptr, lpdword_t out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamParseGamerTileKey, kUserProfiles, kStub);

dword_result_t XamReadTileToTexture(dword_t unk1, dword_t unk2, dword_t unk3,
                                    dword_t unk4, lpvoid_t buffer_ptr,
                                    dword_t stride, dword_t height,
                                    dword_t overlapped_ptr) {
  // unk1: const?
  // unk2: out0 from XamParseGamerTileKey
  // unk3: some variant of out1/out2
  // unk4: const?

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamReadTileToTexture, kUserProfiles, kStub);

dword_result_t XamWriteGamerTile(dword_t arg1, dword_t arg2, dword_t arg3,
                                 dword_t arg4, dword_t arg5,
                                 dword_t overlapped_ptr) {
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr,
                                                X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamWriteGamerTile, kUserProfiles, kStub);

dword_result_t XamSessionCreateHandle(lpdword_t handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSessionCreateHandle, kUserProfiles, kStub);

dword_result_t XamSessionRefObjByHandle(dword_t handle, lpdword_t obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSessionRefObjByHandle, kUserProfiles, kStub);

dword_result_t XamUserCreateTitlesPlayedEnumerator(
    dword_t user_index, dword_t xuid, dword_t flags, dword_t offset,
    dword_t games_count, lpdword_t buffer_size_ptr, lpdword_t handle_ptr) {
  // + 128 bytes for the 64-char titlename
  const uint32_t kEntrySize = sizeof(xdbf::X_XDBF_GPD_TITLEPLAYED) + 128;

  auto user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return X_ERROR_INVALID_PARAMETER;  // TODO: proper error code!
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = kEntrySize * games_count;
  }

  std::vector<xdbf::TitlePlayed> titles;
  user_profile->GetDashboardGpd()->GetTitles(&titles);

  // Sort titles by date played
  std::sort(titles.begin(), titles.end(),
            [](const xdbf::TitlePlayed& first, const xdbf::TitlePlayed& second)
                -> bool { return first.last_played > second.last_played; });

  auto e = new XStaticEnumerator(kernel_state(), games_count, kEntrySize);
  e->Initialize();

  *handle_ptr = e->handle();

  for (auto title : titles) {
    if (e->item_count() >= games_count) {
      break;
    }

    // For some reason dashboard gpd stores info about itself
    if (title.title_id == kDashboardID) continue;

    // TODO: Look for better check to provide information about demo title
    // or system title
    if (!title.gamerscore_total || !title.achievements_possible) continue;

    auto* details = (xdbf::X_XDBF_GPD_TITLEPLAYED*)e->AppendItem();
    title.WriteGPD(details);
  }

  XELOGD("XamUserCreateTitlesPlayedEnumerator: added %d items to enumerator",
         e->item_count());

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamUserCreateTitlesPlayedEnumerator, kUserProfiles,
                    kImplemented);

dword_result_t XamReadTile(dword_t tile_type, dword_t game_id, qword_t item_id,
                           dword_t user_index, lpdword_t output_ptr,
                           lpdword_t buffer_size_ptr, dword_t overlapped_ptr) {
  // Wrap function in a lambda func so we can use return to exit out when
  // needed, but still always be able to set the xoverlapped value
  // this way we don't need a bunch of if/else nesting to accomplish the same
  auto main_fn = [tile_type, game_id, item_id, user_index, output_ptr,
                  buffer_size_ptr]() {
    uint64_t image_id = item_id;

    uint8_t* data = nullptr;
    size_t data_len = 0;
    std::unique_ptr<MappedMemory> mmap;

    if (!output_ptr || !buffer_size_ptr) {
      return X_ERROR_FILE_NOT_FOUND;
    }

    auto type = (XTileType)tile_type.value();
    if (kTileFileNames.count(type)) {
      // image_id = XUID of profile to retrieve from

      auto user_profile = kernel_state()->user_profile(image_id);
      if (!user_profile) {
        return X_ERROR_FILE_NOT_FOUND;  // TODO: proper error code!
      }

      auto file_path = user_profile->path();
      file_path += kTileFileNames.at(type);

      mmap = MappedMemory::Open(file_path, MappedMemory::Mode::kRead);
      if (!mmap) {
        return X_ERROR_FILE_NOT_FOUND;
      }
      data = mmap->data();
      data_len = mmap->size();
    } else {
      auto user_profile = kernel_state()->user_profile(user_index);
      if (!user_profile) {
        return X_ERROR_FILE_NOT_FOUND;  // TODO: proper error code!
      }

      auto gpd = user_profile->GetTitleGpd(game_id.value());

      if (!gpd) {
        return X_ERROR_FILE_NOT_FOUND;
      }

      auto entry = gpd->GetEntry(
          static_cast<uint16_t>(xdbf::GpdSection::kImage), image_id);

      if (!entry) {
        return X_ERROR_FILE_NOT_FOUND;
      }

      data = entry->data.data();
      data_len = entry->data.size();
    }

    if (!data || !data_len) {
      return X_ERROR_FILE_NOT_FOUND;
    }

    auto passed_size = *buffer_size_ptr;
    *buffer_size_ptr = (uint32_t)data_len;

    auto ret_val = X_ERROR_INSUFFICIENT_BUFFER;

    if (passed_size >= *buffer_size_ptr) {
      std::memcpy(output_ptr, data, data_len);
      ret_val = X_ERROR_SUCCESS;
    }

    if (mmap) {
      mmap->Close();
    }

    return ret_val;
  };

  auto retval = main_fn();

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, retval);
    return X_ERROR_IO_PENDING;
  }
  return retval;
}
DECLARE_XAM_EXPORT1(XamReadTile, kUserProfiles, kSketchy);

dword_result_t XamReadTileEx(dword_t tile_type, dword_t game_id,
                             qword_t item_id, dword_t offset, dword_t unk1,
                             dword_t unk2, lpdword_t output_ptr,
                             lpdword_t buffer_size_ptr) {
  return XamReadTile(tile_type, game_id, item_id, offset, output_ptr,
                     buffer_size_ptr, 0);
}
DECLARE_XAM_EXPORT1(XamReadTileEx, kUserProfiles, kSketchy);

dword_result_t XamUserIsOnlineEnabled(dword_t user_index) {
  // 0 - Offline
  // 1 - Online

  auto* user_profile = kernel_state()->user_profile(user_index);
  if (!user_profile) {
    return 0;
  }

  return user_profile->signin_state() == 2;
}
DECLARE_XAM_EXPORT1(XamUserIsOnlineEnabled, kUserProfiles, kStub);

dword_result_t XamUserGetIndexFromXUID(qword_t xuid, dword_t r4,
                                       lpdword_t user_index) {
  for (uint32_t i = 0; i < kernel_state()->num_profiles(); i++) {
    auto profile = kernel_state()->user_profile(i);
    if (!profile) {
      continue;
    }
    if (profile->xuid() == xuid) {
      *user_index = i;
      return X_E_SUCCESS;
    }
  }

  return X_E_NO_SUCH_USER;
}
DECLARE_XAM_EXPORT1(XamUserGetIndexFromXUID, kUserProfiles, kStub);

dword_result_t XamProfileCreate(dword_t flags, lpdword_t device_id,
                                qword_t xuid,
                                pointer_t<X_XAMACCOUNTINFO> account, dword_t r7,
                                dword_t r8, dword_t r9, dword_t r10) {
  *device_id = 0x00000001;

  X_XAMACCOUNTINFO swapped;
  memcpy(&swapped, account, sizeof(X_XAMACCOUNTINFO));
  xe::copy_and_swap<wchar_t>(swapped.gamertag, swapped.gamertag, 16);

  if (xuid != 0) {
    // Why is this param even included?
    return X_E_INVALIDARG;
  }

  xam::UserProfile profile(kernel_state());
  profile.Create(&swapped, false);
  auto new_xuid = profile.xuid_offline();

  // TODO: r10 seems to be some kind of output?

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamProfileCreate, kUserProfiles, kStub);

#pragma pack(push, 1)
struct X_USER_INFO {
  xe::be<uint64_t> xuid;
  char name[16];
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unk;
  xe::be<uint32_t> title_id;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> unk3;
};
static_assert_size(X_USER_INFO, 44);

typedef struct {
  xe::be<uint32_t> user_count;
  X_USER_INFO users_info[7];
} X_USER_PARTY_LIST;
static_assert_size(X_USER_PARTY_LIST, 4+sizeof(X_USER_INFO)*7);
#pragma pack(pop)

dword_result_t XamPartyGetUserListInternal(
    pointer_t<X_USER_PARTY_LIST> party_struct_ptr) {

  party_struct_ptr->user_count = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamPartyGetUserListInternal, kUserProfiles, kStub);

}  // namespace xdbf
}  // namespace xam
}  // namespace kernel
}  // namespace xe

void xe::kernel::xam::RegisterUserExports(
    xe::cpu::ExportResolver* export_resolver, KernelState* kernel_state) {}
