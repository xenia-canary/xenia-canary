/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/util/shim_utils.h"

namespace xe {
namespace kernel {
namespace xam {

Avatar::Avatar() {}

void Avatar::SetXUID(uint64_t xuid) {
  if (xuid == offline_xuid_) {
    return;
  }

  const uint32_t offline_xuid_offset = 0x380;

  xe::be<uint64_t>* offline_xuid = reinterpret_cast<xe::be<uint64_t>*>(
      manifest.data() + offline_xuid_offset);

  *offline_xuid = xuid;
  offline_xuid_ = xuid;
}

X_AVATAR_BODY_TYPE Avatar::GetBodyType() const {
  X_AVATAR_ASSET_ID asset_id = GetBodyComponent().asset_id;

  if (asset_id == kMaleBody) {
    return X_AVATAR_BODY_TYPE::Male;
  }

  if (asset_id == kFemaleBody) {
    return X_AVATAR_BODY_TYPE::Female;
  }

  return X_AVATAR_BODY_TYPE::Unknown;
}

X_ASSET_GUID_TYPE Avatar::GetAssetGuidType(X_AVATAR_ASSET_ID asset_id) {
  const auto asset_id_bytes = asset_id.ToByteArray();

  if ((asset_id_bytes[8] & 0xF0) != 0xC0) {
    return X_ASSET_GUID_TYPE::Custom;
  }

  switch (asset_id_bytes[7] & 0xF) {
    case 0:
      return X_ASSET_GUID_TYPE::TOC;
    case 1:
      return X_ASSET_GUID_TYPE::Awardable;
    case 2:
      return X_ASSET_GUID_TYPE::Marketplace;
    case 15:
      return X_ASSET_GUID_TYPE::Custom;
    default:
      return X_ASSET_GUID_TYPE::Custom;
  }

  return X_ASSET_GUID_TYPE::Custom;
}

bool Avatar::ValidateAssetId(X_AVATAR_ASSET_ID guid) {
  if (GetAssetGuidType(guid) == X_ASSET_GUID_TYPE::TOC) {
    const auto guid_bytes = guid.ToByteArray();

    const auto asset_version_id =
        std::vector<uint8_t>(guid_bytes.data() + 10, guid_bytes.data() + 16);

    const std::vector<uint8_t> toc_asset_version_id(
        std::begin(kTocAssetVersionId), std::end(kTocAssetVersionId));

    return toc_asset_version_id == asset_version_id;
  }

  return true;
}

Avatar Avatar::ParseManifest(std::span<const uint8_t> manifest) {
  Avatar avatar = Avatar();

  avatar.manifest =
      std::vector(manifest.data(), manifest.data() + manifest.size());

  uint8_t* manifest_ptr = avatar.manifest.data();

  manifest_ptr += 4;

  avatar.weight_factor = xe::load_and_swap<float>(manifest_ptr);
  manifest_ptr += sizeof(float);

  avatar.height_factor = xe::load_and_swap<float>(manifest_ptr);
  manifest_ptr += sizeof(float);

  for (uint32_t i = 0; i < kBlendShapesCount; i++) {
    X_AVATAR_ASSET_ID asset_id =
        *reinterpret_cast<X_AVATAR_ASSET_ID*>(manifest_ptr);
    manifest_ptr += sizeof(X_AVATAR_ASSET_ID);

    assert_true(ValidateAssetId(asset_id));

    avatar.blend_shapes_.push_back(asset_id);
  }

  for (uint32_t i = 0; i < kReplacementTexturesCount; i++) {
    X_REPLACEMENT_TEXTURE texture =
        *reinterpret_cast<X_REPLACEMENT_TEXTURE*>(manifest_ptr);
    manifest_ptr += sizeof(X_REPLACEMENT_TEXTURE);

    assert_true(ValidateAssetId(texture.linked_asset_id));
    assert_true(ValidateAssetId(texture.texture_asset_id));

    avatar.replacement_textures_.push_back(texture);
  }

  for (uint32_t i = 0; i < kColoursCount; i++) {
    X_RGB_COLOR color = *reinterpret_cast<X_RGB_COLOR*>(manifest_ptr);
    manifest_ptr += sizeof(X_RGB_COLOR);

    avatar.dynamic_colors_.push_back(color);
  }

  avatar.body_component_info_ =
      *reinterpret_cast<X_COMPONENT_INFO*>(manifest_ptr);
  manifest_ptr += sizeof(X_COMPONENT_INFO);

  assert_true(ValidateAssetId(avatar.body_component_info_.asset_id));

  avatar.head_component_info_ =
      *reinterpret_cast<X_COMPONENT_INFO*>(manifest_ptr);
  manifest_ptr += sizeof(X_COMPONENT_INFO);

  assert_true(ValidateAssetId(avatar.head_component_info_.asset_id));

  for (uint32_t i = 0; i < kComponentsCount; i++) {
    X_COMPONENT_INFO info = *reinterpret_cast<X_COMPONENT_INFO*>(manifest_ptr);
    manifest_ptr += sizeof(X_COMPONENT_INFO);

    assert_true(ValidateAssetId(info.asset_id));

    avatar.components_info_.push_back(info);
  }

  for (uint32_t i = 0; i < kPreviouslyRequiredComponentsCount; i++) {
    X_COMPONENT_INFO info = *reinterpret_cast<X_COMPONENT_INFO*>(manifest_ptr);
    manifest_ptr += sizeof(X_COMPONENT_INFO);

    assert_true(ValidateAssetId(info.asset_id));

    avatar.previous_required_component_info_.push_back(info);
  }

  // Console ID - 5 Bytes?

  avatar.offline_xuid_ = *reinterpret_cast<xe::be<uint64_t>*>(manifest_ptr);

  return avatar;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
