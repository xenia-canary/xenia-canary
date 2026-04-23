/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/avatars/asset_pack.h"
#include "xenia/avatars/guest_asset.h"
#include "xenia/avatars/guest_load_animation.h"
#include "xenia/avatars/guest_load_asset.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

using namespace xe::avatars;

bool LoadAvatarAssetPack(KernelState* kernel_state) {
  bool is_loaded = kernel_state->avatar_asset_pack()->is_loaded();
  bool legacy_is_loaded = kernel_state->legacy_avatar_asset_pack()->is_loaded();

  if (is_loaded && legacy_is_loaded) {
    return true;
  }

  auto content_manager = kernel_state->content_manager();
  XCONTENT_AGGREGATE_DATA content_data = {};
  content_data.title_id = kSystemDataID;
  content_data.content_type = XContentType::kSUStoragePack;
  content_data.set_file_name("FFFE07DF00000002");
  uint32_t license = 0;
  if (content_manager->OpenContent("AvatarAssetPack", 0, content_data,
                                   license) == X_ERROR_SUCCESS) {
    auto load_asset_pack = [&](std::string_view path) -> std::vector<uint8_t> {
      vfs::File* file;
      vfs::FileAction file_action;
      X_STATUS result = kernel_state->file_system()->OpenFile(
          nullptr, path, vfs::FileDisposition::kOpen,
          vfs::FileAccess::kGenericRead, false, true, &file, &file_action);
      if (result != X_STATUS_SUCCESS) {
        return {};
      }
      std::vector<uint8_t> data(file->entry()->size());
      size_t read_bytes = 0;
      file->ReadSync(std::span<uint8_t>(data.data(), file->entry()->size()), 0,
                     &read_bytes);
      file->Destroy();
      return data;
    };

    // Find and load AvatarAssetPack.toc
    auto data = load_asset_pack("AvatarAssetPack:\\AvatarAssetPack.toc");
    if (!data.empty()) {
      is_loaded = kernel_state->avatar_asset_pack()->Load(data);
      if (!is_loaded) {
        XELOGW("Failed to load avatar asset pack!");
      }
    }

    // Find and load AvatarAssetPackLegacyV1.toc
    data = load_asset_pack("AvatarAssetPack:\\AvatarAssetPackLegacyV1.toc");
    if (!data.empty()) {
      legacy_is_loaded = kernel_state->legacy_avatar_asset_pack()->Load(data);
      if (!legacy_is_loaded) {
        XELOGW("Failed to load legacy avatar asset pack!");
      }
    }
    content_manager->CloseContent("AvatarAssetPack");
  }
  return is_loaded && legacy_is_loaded;
}

static uint32_t gTitleVersion = 0;
static uint32_t gCoordinateSystem = 0;

dword_result_t XamAvatarInitialize_entry(
    dword_t version,                  // 1, 4, etc
    dword_t coordinate_system,        // 0 or 1
    dword_t load_processor_number,    // for thread creation?
    lpdword_t texture_function_ptrs,  // 20b, 5 pointers
    lpvoid_t work_buffer_ptr, dword_t work_buffer_size) {
  // texture_function_ptrs order
  // 0 - TileTexture
  // 1 - UntileTexture
  // 2 - GetDimensions
  // 3 - CalculateMipSize
  // 4 - CalculateMipOffset
  if (!LoadAvatarAssetPack(kernel_state())) {
    // Negative to fail. Game should immediately call XamAvatarShutdown.
    return X_ERROR_FUNCTION_FAILED;
  }
  gTitleVersion = version;
  gCoordinateSystem = coordinate_system;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamAvatarInitialize, kAvatars, kStub, kSketchy);

void XamAvatarShutdown_entry() {
  // No-op.
}
DECLARE_XAM_EXPORT1(XamAvatarShutdown, kAvatars, kStub);

struct X_AVATAR_COMPONENT_ASSET_INFO {
  uint32_t cpu_size;
  uint32_t gpu_size;
};

const X_AVATAR_COMPONENT_ASSET_INFO avatar_component_asset_infos[] = {
    {0x00000800, 0x00053800},  // avatars::ComponentCategory::kHead
    {0x00000400, 0x00012C00},  // avatars::ComponentCategory::kBody
    {0x00000800, 0x0000E000},  // avatars::ComponentCategory::kHair
    {0x00000800, 0x00014400},  // avatars::ComponentCategory::kTop
    {0x00000800, 0x0000F000},  // avatars::ComponentCategory::kBottom
    {0x00000800, 0x0000D400},  // avatars::ComponentCategory::kShoes
    {0x00000800, 0x0000A400},  // avatars::ComponentCategory::kHat
    {0x00000800, 0x00006800},  // avatars::ComponentCategory::kGloves
    {0x00000800, 0x00008400},  // avatars::ComponentCategory::kGlasses
    {0x00000800, 0x00005C00},  // avatars::ComponentCategory::kWristwear
    {0x00000800, 0x00005800},  // avatars::ComponentCategory::kEarrings
    {0x00000800, 0x00005C00},  // avatars::ComponentCategory::kRing
    {0x00000800, 0x00019000},  // avatars::ComponentCategory::kProp
};

dword_result_t XamAvatarGetAssetsResultSize_entry(dword_t category_mask,
                                                  lpdword_t out_cpu_size,
                                                  lpdword_t out_gpu_size) {
  uint32_t cpu_size =
      !(category_mask & avatars::ComponentCategory::kProp) ? 0x1F80 : 0x3147C;
  uint32_t gpu_size = 0;
  for (size_t i = 0; i < countof(avatar_component_asset_infos); ++i) {
    auto category = 1 << i;
    const auto& avatar_component_asset_info = avatar_component_asset_infos[i];
    if (!!(category & category_mask)) {
      cpu_size += xe::align(avatar_component_asset_info.cpu_size, 16u);
      gpu_size += xe::align(avatar_component_asset_info.gpu_size, 4096u);
    }
  }
  *out_cpu_size = cpu_size;
  *out_gpu_size = gpu_size;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetsResultSize, kAvatars, kImplemented);

dword_result_t XamAvatarGetMetadataLocalUser_entry(
    dword_t user_index, pointer_t<X_AVATAR_METADATA> avatar_metadata_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    if (user_index >= XUserMaxUserCount) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }

    if (!avatar_metadata_ptr) {
      extended_error = X_E_INVALIDARG;
      return X_ERROR_INVALID_PARAMETER;
    }

    const auto user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user_profile) {
      extended_error = X_E_NO_SUCH_USER;
      return X_ERROR_FUNCTION_FAILED;
    }

    const uint32_t avatar_info_id =
        static_cast<uint32_t>(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1);

    X_USER_PROFILE_SETTING avatar_info_setting_data = {};

    avatar_info_setting_data.user_index = static_cast<uint32_t>(user_index);
    avatar_info_setting_data.setting_id = avatar_info_id;
    avatar_info_setting_data.data.type = X_USER_DATA_TYPE::BINARY;
    avatar_info_setting_data.data.data.binary.size = kMaxUserDataSize;
    avatar_info_setting_data.data.data.binary.ptr = 0;

    uint32_t avatar_metadata_address = avatar_metadata_ptr.guest_address();

    const bool has_avatar_info_setting =
        kernel_state()->xam_state()->user_tracker()->GetUserSetting(
            user_profile->xuid(), kDashboardID, avatar_info_id,
            &avatar_info_setting_data, avatar_metadata_address);

    // Profile doesn't have avatar info setting
    if (!avatar_info_setting_data.data.data.binary.ptr) {
      extended_error = X_E_FAIL;
      return X_ERROR_FUNCTION_FAILED;
    }

    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataLocalUser, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataRandom_entry(
    dword_t category_mask, dword_t metadata_count,
    pointer_t<X_AVATAR_METADATA> metadatas,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto run = [metadata_count, metadatas]() -> X_RESULT {
    for (uint32_t i = 0; i < metadata_count; ++i) {
      auto& entry = metadatas[i];
    }
    return X_ERROR_SUCCESS;
  };
  if (!overlapped) {
    return run();
  } else {
    kernel_state()->CompleteOverlappedDeferred(run, overlapped);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataRandom, kAvatars, kStub);

dword_result_t XamAvatarMetadataGetBodyType_entry(
    pointer_t<X_AVATAR_METADATA> metadata) {
  const AssetId male_body_asset_id = {
      2, 0, 1, {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  const AssetId female_body_asset_id = {
      2, 0, 2, {0xC1, 0xC8, 0xF1, 0x09, 0xA1, 0x9C, 0xB2, 0xE0}};
  if (metadata->body_component.asset_id == male_body_asset_id) {
    return 1;
  }
  if (metadata->body_component.asset_id == female_body_asset_id) {
    return 2;
  }
  return 0;
}
DECLARE_XAM_EXPORT1(XamAvatarMetadataGetBodyType, kAvatars, kStub);

dword_result_t XamAvatarGetAssets_entry(
    pointer_t<X_AVATAR_METADATA> metadata_ptr, dword_t category_mask,
    dword_t flags, lpvoid_t cpu_buffer, lpvoid_t gpu_buffer,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  const auto& metadata = *metadata_ptr;
  auto run = [metadata, category_mask, flags, cpu_buffer,
              gpu_buffer]() -> X_RESULT {
    assert_true(gTitleVersion > 0);
    uint32_t skeleton_version;
    AssetPack* asset_pack;
    if (gTitleVersion >= 3) {
      asset_pack = kernel_state()->avatar_asset_pack();
      skeleton_version = 2;
    } else {
      asset_pack = kernel_state()->legacy_avatar_asset_pack();
      skeleton_version = 1;
    }
    avatars::MemoryBlock cpu_memory(cpu_buffer, 16);
    avatars::MemoryBlock gpu_memory(gpu_buffer, 4096);
    auto result = avatars::LoadAssetsToGuest(
        metadata, category_mask, flags, asset_pack, &cpu_memory, &gpu_memory,
        skeleton_version, gCoordinateSystem);
    if (result) {
      cpu_memory.ResolvePointers(cpu_buffer.guest_address());
      gpu_memory.ResolvePointers(gpu_buffer.guest_address());
      return X_ERROR_SUCCESS;
    } else {
      return X_ERROR_FUNCTION_FAILED;
    }
  };
  if (!overlapped) {
    return run();
  } else {
    kernel_state()->CompleteOverlappedDeferred(run, overlapped);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssets, kAvatars, kStub);

dword_result_t XamAvatarLoadAnimation_entry(
    pointer_t<AssetId> asset_id, dword_t flags,
    pointer_t<X_AVATAR_ANIMATION> animation,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  XELOGD("Request to load avatar animation {}: {}", asset_id->to_string(),
         to_utf8(kernel_state()->avatar_asset_pack()->GetAssetName(*asset_id)));
  auto run = [asset_id = *asset_id, animation](
                 uint32_t& length, uint32_t& extended_error) -> X_RESULT {
    assert_true(gTitleVersion > 0);
    AssetPack* asset_pack;
    if (gTitleVersion >= 3) {
      asset_pack = kernel_state()->avatar_asset_pack();
    } else {
      asset_pack = kernel_state()->legacy_avatar_asset_pack();
    }
    auto compressed_data_buffer_ptr =
        kernel_state()->memory()->TranslateVirtual(
            animation->compressed_data_buffer_ptr);
    if (!avatars::LoadAnimationToGuest(asset_pack, asset_id, animation,
                                       compressed_data_buffer_ptr,
                                       gCoordinateSystem)) {
      XELOGD("Failed to load animation!");
      length = 0;
      extended_error = X_E_FAIL;
      return X_E_FAIL;
    }
    XELOGD("Success in loading animation!");
    length = 0;
    extended_error = X_ERROR_SUCCESS;
    return X_ERROR_SUCCESS;
  };
  if (!overlapped) {
    uint32_t length, extended_error;
    return run(length, extended_error);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamAvatarLoadAnimation, kAvatars, kStub);

dword_result_t XamAvatarGenerateMipMaps_entry(
    lpvoid_t assets, dword_t flags, dword_t output_size, lpvoid_t output_buffer,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto run = [](uint32_t& length, uint32_t& extended_error) -> X_RESULT {
    length = 0;
    extended_error = X_E_SUCCESS;
    return X_E_SUCCESS;
  };
  if (!overlapped) {
    uint32_t length, extended_error;
    return run(length, extended_error);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamAvatarGenerateMipMaps, kAvatars, kStub);

dword_result_t XamAvatarSetMetadata_entry(
    dword_t user_index, dword_t avatar_info_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;
    // Update and save settings.
    const auto& user_profile =
        kernel_state()->xam_state()->GetUserProfile(user_index);

    // Skip writing data about users with id != 0 they're not supported
    if (!user_profile) {
      extended_error = X_E_NO_SUCH_USER;
      return X_ERROR_FUNCTION_FAILED;
    }

    const uint32_t avatar_info_id =
        static_cast<uint32_t>(UserSettingId::XPROFILE_GAMERCARD_AVATAR_INFO_1);

    X_USER_PROFILE_SETTING setting_data;
    setting_data.user_index = static_cast<uint32_t>(user_index);
    setting_data.setting_id = avatar_info_id;
    setting_data.data.type = X_USER_DATA_TYPE::BINARY;
    setting_data.data.data.binary.size = kMaxUserDataSize;
    setting_data.data.data.binary.ptr = static_cast<uint32_t>(avatar_info_ptr);

    const UserSetting setting = UserSetting(&setting_data);

    kernel_state()->xam_state()->user_tracker()->UpsertSetting(
        user_profile->xuid(), kDashboardID, &setting);

    return X_STATUS_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result == X_ERROR_SUCCESS ? result : extended_error;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamAvatarSetMetadata, kAvatars, kStub);

dword_result_t XamAvatarGetAssetIcon_entry(
    pointer_t<AssetId> asset_id, lpvoid_t a3, lpdword_t a4, dword_t a5,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  auto run = [](uint32_t& length, uint32_t& extended_error) -> X_RESULT {
    length = 0;
    extended_error = X_E_FAIL;
    return X_E_FAIL;
  };
  if (!overlapped) {
    uint32_t length, extended_error;
    return run(length, extended_error);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetIcon, kAvatars, kStub);

dword_result_t XamPngDecode_entry(lpdword_t png_data, dword_t size,
                                  lpdword_t buffer, dword_t flags, dword_t r7,
                                  dword_t r8, dword_t r9) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamPngDecode, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Avatar);
