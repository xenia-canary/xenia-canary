/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_H_
#define XENIA_KERNEL_XAM_AVATAR_H_

#include <span>

#include "xenia/kernel/xam/xam.h"

namespace xe {
namespace kernel {
namespace xam {

using X_AVATAR_ASSET_ID = X_GUID;

constexpr uint32_t kBlendShapesCount = 3;
constexpr uint32_t kReplacementTexturesCount = 6;
constexpr uint32_t kColoursCount = 9;
constexpr uint32_t kComponentsCount = 13;
constexpr uint32_t kPreviouslyRequiredComponentsCount = 4;

constexpr uint8_t kTocAssetVersionId[6] = {241, 9, 161, 156, 178, 224};

const X_AVATAR_ASSET_ID kMaleBody =
    X_AVATAR_ASSET_ID(2, 0, 1, 193, 200, 241, 9, 161, 156, 178, 224);

const X_AVATAR_ASSET_ID kFemaleBody =
    X_AVATAR_ASSET_ID(2, 1, 2, 193, 200, 241, 9, 161, 156, 178, 224);

enum class X_ASSET_GUID_TYPE { Custom, TOC, Awardable, Marketplace };

// https://github.com/hetelek/Velocity/blob/master/XboxInternals/AvatarAsset/AvatarAssetDefinintions.h
enum class X_AVATAR_BODY_TYPE : uint8_t { Unknown, Male, Female, All };

struct X_RGB_COLOR {
  uint8_t alpha;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};
static_assert_size(X_RGB_COLOR, 4);

struct X_REPLACEMENT_TEXTURE {
  X_AVATAR_ASSET_ID texture_asset_id;
  X_AVATAR_ASSET_ID linked_asset_id;
};
static_assert_size(X_REPLACEMENT_TEXTURE, 0x20);

struct X_COMPONENT_INFO {
  X_AVATAR_ASSET_ID asset_id;
  xe::be<uint16_t> component_mask;
  xe::be<uint16_t> unused;
  X_RGB_COLOR colour0;
  X_RGB_COLOR colour1;
  X_RGB_COLOR colour2;
};
static_assert_size(X_COMPONENT_INFO, 0x20);

class Avatar {
 public:
  Avatar();

  void SetXUID(const uint64_t xuid);

  std::vector<uint8_t> GetManifest() const { return manifest; };

  X_AVATAR_BODY_TYPE GetBodyType() const;

  const uint64_t& GetOfflineXUID() const { return offline_xuid_; };

  const std::vector<X_AVATAR_ASSET_ID>& GetBlendShapes() const {
    return blend_shapes_;
  };

  const std::vector<X_REPLACEMENT_TEXTURE>& GetReplacmentTextures() const {
    return replacement_textures_;
  };

  const std::vector<X_RGB_COLOR>& GetDynamicColors() const {
    return dynamic_colors_;
  };

  const X_COMPONENT_INFO& GetBodyComponent() const {
    return body_component_info_;
  };

  const X_COMPONENT_INFO& GetHeadComponent() const {
    return head_component_info_;
  };

  const std::vector<X_COMPONENT_INFO>& GetComponentsInfo() const {
    return components_info_;
  };

  const std::vector<X_COMPONENT_INFO>& GetPreviousRequiredComponentsInfo()
      const {
    return previous_required_component_info_;
  };

  static X_ASSET_GUID_TYPE GetAssetGuidType(X_AVATAR_ASSET_ID asset_id);

  static bool ValidateAssetId(X_AVATAR_ASSET_ID guid);

  // TODO:
  static std::vector<Avatar> CreateRandomAvatars(X_AVATAR_BODY_TYPE body_type,
                                                 uint32_t avatars_count);

  static Avatar ParseManifest(std::span<const uint8_t> manifest);

 private:
  std::vector<uint8_t> manifest;

  uint64_t offline_xuid_ = 0;

  float weight_factor = 0;
  float height_factor = 0;

  std::vector<X_AVATAR_ASSET_ID> blend_shapes_ = {};
  std::vector<X_REPLACEMENT_TEXTURE> replacement_textures_ = {};
  std::vector<X_RGB_COLOR> dynamic_colors_ = {};
  X_COMPONENT_INFO body_component_info_ = {};
  X_COMPONENT_INFO head_component_info_ = {};
  std::vector<X_COMPONENT_INFO> components_info_ = {};
  std::vector<X_COMPONENT_INFO> previous_required_component_info_ = {};
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_H_
