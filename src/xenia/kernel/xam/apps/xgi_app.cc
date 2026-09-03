/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xsession.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {
/*
 * Most of the structs below were found in the Source SDK, provided as stubs.
 * Specifically, they can be found in the Source 2007 SDK and the Alien Swarm
 * Source SDK. Both are available on Steam for free. A GitHub mirror of the
 * Alien Swarm SDK can be found here:
 * https://github.com/NicolasDe/AlienSwarm/blob/master/src/common/xbox/xboxstubs.h
 */

struct XGI_WRITEACHIEVEMENT {
  xe::be<uint32_t> num_achievements;
  xe::be<uint32_t> achievements_ptr;  // X_USER_ACHIEVEMENT*
};
static_assert_size(XGI_WRITEACHIEVEMENT, 0x8);

struct X_USER_AVATAR_ASSET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> award_id;
};
static_assert_size(X_USER_AVATAR_ASSET, 0x8);

struct XGI_AWARD_AVATAR_ASSETS {
  xe::be<uint32_t> num_assets;
  xe::be<uint32_t> assets_ptr;  // X_USER_AVATAR_ASSET*
};
static_assert_size(XGI_AWARD_AVATAR_ASSETS, 0x8);

struct XGI_USER_ACHIEVEMENT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> achievement_id;
};
static_assert_size(XGI_USER_ACHIEVEMENT, 0x8);

struct XGI_USER_GET_ACHIEVEMENT {
  xe::be<uint32_t> achievement_count;
  xe::be<uint32_t> achievements_ptr;  // XGI_USER_ACHIEVEMENT*
};
static_assert_size(XGI_USER_GET_ACHIEVEMENT, 0x8);

struct XGI_USER_GET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;  // If xuid is 0 then user_index is used.
  xe::be<uint32_t>
      property_size_ptr;  // Normally filled with sizeof(XUSER_PROPERTY), with
                          // exception of binary and wstring type.
  xe::be<uint32_t> context_address;
  xe::be<uint32_t> property_address;
};
static_assert_size(XGI_USER_GET_PROPERTY, 0x20);

struct XGI_USER_SET_CONTEXT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  XUSER_CONTEXT context;
};
static_assert_size(XGI_USER_SET_CONTEXT, 0x18);

struct XGI_USER_SET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> property_id;
  xe::be<uint32_t> data_size;
  xe::be<uint32_t> data_address;
};
static_assert_size(XGI_USER_SET_PROPERTY, 0x20);

struct XGI_USER_STATS_VIEW {
  xe::be<uint32_t> ViewId;
  xe::be<uint32_t> TotalViewRows;
  xe::be<uint32_t> NumRows;
  xe::be<uint32_t> pRows;
};

struct XGI_USER_STATS_COLUMN {
  xe::be<uint16_t> ColumnId;
  X_USER_DATA Value;
};

struct XGI_USER_STATS_RESET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> view_id;
};

// ANID = Anonymous user id
struct XGI_USER_ANID {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> AnId_buffer_size;
  xe::be<uint32_t> AnId_buffer_ptr;  // char*
  xe::be<uint32_t> block;            // 1
};
static_assert_size(XGI_USER_ANID, 0x10);

struct XGI_USER_READ_STATS {
  xe::be<uint32_t> title_id;
  xe::be<uint32_t> xuids_count;
  xe::be<uint32_t> xuids_ptr;
  xe::be<uint32_t> specs_count;
  xe::be<uint32_t> specs_ptr;
  xe::be<uint32_t> results_size;
  xe::be<uint32_t> results_ptr;
};
static_assert_size(XGI_USER_READ_STATS, 0x1C);

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_SET_CONTEXT));
      const XGI_USER_SET_CONTEXT* xgi_context =
          reinterpret_cast<const XGI_USER_SET_CONTEXT*>(buffer);

      XELOGD("XGIUserSetContext({:08X}, ID: {:08X}, Value: {:08X})",
             xgi_context->user_index.get(),
             xgi_context->context.context_id.get(),
             xgi_context->context.value.get());

      UserProfile* user = nullptr;
      if (xgi_context->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_context->xuid);
      } else {
        user =
            kernel_state_->xam_state()->GetUserProfile(xgi_context->user_index);
      }

      if (user) {
        kernel_state_->xam_state()->user_tracker()->UpdateContext(
            user->xuid(), xgi_context->context.context_id,
            xgi_context->context.value);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_SET_PROPERTY));
      const XGI_USER_SET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_USER_SET_PROPERTY*>(buffer);

      XELOGD("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})",
             xgi_property->user_index.get(), xgi_property->property_id.get(),
             xgi_property->data_size.get(), xgi_property->data_address.get());

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (user) {
        Property property(
            xgi_property->property_id,
            Property::get_valid_data_size(xgi_property->property_id,
                                          xgi_property->data_size),
            memory_->TranslateVirtual<uint8_t*>(xgi_property->data_address));

        kernel_state_->xam_state()->user_tracker()->AddProperty(user->xuid(),
                                                                &property);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_ACHIEVEMENT));

      const XGI_WRITEACHIEVEMENT* write_achievements =
          reinterpret_cast<const XGI_WRITEACHIEVEMENT*>(buffer);

      const XGI_USER_ACHIEVEMENT* achievements =
          memory_->TranslateVirtual<XGI_USER_ACHIEVEMENT*>(
              write_achievements->achievements_ptr);

      XELOGD("XGIUserWriteAchievements({:08X}, {:08X})",
             write_achievements->num_achievements.get(),
             write_achievements->achievements_ptr.get());

      if (achievements->user_index >= XUserMaxUserCount) {
        return X_E_NOTFOUND;
      }

      for (uint32_t i = 0; i < write_achievements->num_achievements; i++) {
        const XGI_USER_ACHIEVEMENT& achievement = achievements[i];

        kernel_state_->achievement_manager()->EarnAchievement(
            achievement.user_index, kernel_state_->title_id(),
            achievement.achievement_id);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_CREATE));
      XGI_SESSION_CREATE* data = reinterpret_cast<XGI_SESSION_CREATE*>(buffer);

      XELOGD(
          "XGISessionCreate({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, "
          "{:08X}), implemented in netplay",
          data->obj_ptr.get(), static_cast<uint32_t>(data->flags),
          data->num_slots_public.get(), data->num_slots_private.get(),
          data->user_index.get(), data->session_info_ptr.get(),
          data->nonce_ptr.get());

      // 584107FB expects offline session creation using flags 0 to succeed
      // while offline.
      // 58410889 expects stats session creation failure while offline.
      //
      // Allow offline session creation, but do not allow Xbox Live featured
      // session creation.

      if (IsXboxLiveSession(static_cast<SessionFlags>(data->flags))) {
        return 0x80155209;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON
      }

      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == sizeof(XGI_SESSION_STATE));
      XGI_SESSION_STATE* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);

      XELOGD(
          "XGISessionDelete({:08X}, {:08X}, {:016X}), implemented in netplay",
          static_cast<uint32_t>(data->obj_ptr),
          static_cast<uint32_t>(data->flags),
          static_cast<uint32_t>(data->session_nonce));
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_MANAGE));
      XGI_SESSION_MANAGE* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);

      XELOGI(
          "XGISessionJoin({:08X}, {:08X}, {:08X}, {:08X}, {:08X}), implemented "
          "in netplay",
          data->obj_ptr.get(), data->array_count.get(),
          data->xuid_array_ptr.get(), data->indices_array_ptr.get(),
          data->private_slots_array_ptr.get());
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      // Gets 584107FB in game.
      // get high score table?
      assert_true(!buffer_length || buffer_length == sizeof(XGI_SESSION_STATE));
      XGI_SESSION_STATE* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);

      XELOGD("XGISessionStart({:08X}, {:08X}, {:016X}), implemented in netplay",
             data->obj_ptr.get(), data->flags.get(), data->session_nonce.get());
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == sizeof(XGI_SESSION_STATE));
      XGI_SESSION_STATE* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);

      XELOGD("XGISessionEnd({:08X}, {:08X}, {:016X}), implemented in netplay",
             data->obj_ptr.get(), data->flags.get(), data->session_nonce.get());
      return X_STATUS_SUCCESS;
    }
    case 0x000B0021: {
      /* XGIUserReadStats:
          - Games Used In (offfline): 5841083A, 534507F4
      */
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_READ_STATS));
      XGI_USER_READ_STATS* data =
          reinterpret_cast<XGI_USER_READ_STATS*>(buffer);

      XELOGD(
          "XGIUserReadStats({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {:08X}, "
          "{:08X})",
          data->title_id.get(), data->xuids_count.get(), data->xuids_ptr.get(),
          data->specs_count.get(), data->specs_ptr.get(),
          data->results_size.get(), data->results_ptr.get());

      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      XELOGD("XGIInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      // XGIUserGetCachedANID
      // Used in 5451082A, 5553081E
      assert_true(!buffer_length || buffer_length == sizeof(XGI_USER_ANID));
      XGI_USER_ANID* data = reinterpret_cast<XGI_USER_ANID*>(buffer);

      XELOGI("XGIUserGetANID({:08X}, {:08X}, {:08X}, {:08X})",
             data->user_index.get(), data->AnId_buffer_size.get(),
             data->AnId_buffer_ptr.get(), data->block.get());

      if (!kernel_state_->xam_state()->IsUserSignedIn(data->user_index)) {
        return X_E_NOTFOUND;
      }

      uint8_t* AnIdBuffer =
          memory_->TranslateVirtual<uint8_t*>(data->AnId_buffer_ptr);

      // Game calls HexDecodeDigit on AnIdBuffer
      for (uint32_t i = 0; i < data->AnId_buffer_size - 1; i++) {
        AnIdBuffer[i] = i % 16;
      }

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_USER_GET_PROPERTY));
      const XGI_USER_GET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_USER_GET_PROPERTY*>(buffer);

      XELOGI("XGIUserGetProperty({:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
             xgi_property->user_index.get(), xgi_property->xuid.get(),
             xgi_property->property_size_ptr.get(),
             xgi_property->context_address.get(),
             xgi_property->property_address.get());

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (!user) {
        XELOGD(
            "XGIUserGetProperty - Invalid user provided: Index: {:08X} XUID: "
            "{:16X}",
            xgi_property->user_index.get(), xgi_property->xuid.get());
        return X_E_NOTFOUND;
      }

      // Process context
      if (xgi_property->context_address) {
        XUSER_CONTEXT* context = memory_->TranslateVirtual<XUSER_CONTEXT*>(
            xgi_property->context_address);

        XELOGD("XGIUserGetProperty - Context requested: {:08X} XUID: {:16X}",
               context->context_id.get(), user->xuid());

        auto context_value =
            kernel_state_->xam_state()->user_tracker()->GetUserContext(
                user->xuid(), context->context_id);

        if (!context_value) {
          return X_E_INVALIDARG;
        }

        context->value = context_value.value();
        return X_E_SUCCESS;
      }

      if (!xgi_property->property_size_ptr || !xgi_property->property_address) {
        return X_E_INVALIDARG;
      }

      // Process property
      XUSER_PROPERTY* property = memory_->TranslateVirtual<XUSER_PROPERTY*>(
          xgi_property->property_address);

      XELOGD("XGIUserGetProperty - Property requested: {:08X} XUID: {:16X}",
             property->property_id.get(), user->xuid());

      return kernel_state_->xam_state()->user_tracker()->GetProperty(
          user->xuid(),
          memory_->TranslateVirtual<uint32_t*>(xgi_property->property_size_ptr),
          property);
    }
    case 0x000B0071: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_AWARD_AVATAR_ASSETS));
      const XGI_AWARD_AVATAR_ASSETS* award_avatar_assets =
          reinterpret_cast<const XGI_AWARD_AVATAR_ASSETS*>(buffer);

      XELOGD("XUserAwardAvatarAssets({:08X}, {:08X})",
             award_avatar_assets->num_assets.get(),
             award_avatar_assets->assets_ptr.get());

      const X_USER_AVATAR_ASSET* avatar_assets =
          memory_->TranslateVirtual<X_USER_AVATAR_ASSET*>(
              award_avatar_assets->assets_ptr);

      for (uint32_t i = 0; i < award_avatar_assets->num_assets; i++) {
        const X_USER_AVATAR_ASSET& avatar_asset = avatar_assets[i];

        const auto user =
            kernel_state_->xam_state()->GetUserProfile(avatar_asset.user_index);

        if (user) {
          XELOGI("Player: {} Unlocked Avatar Award Asset ID: {}", user->name(),
                 avatar_asset.award_id.get());
        }
      }

      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
