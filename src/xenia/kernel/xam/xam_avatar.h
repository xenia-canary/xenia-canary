/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_AVATAR_H_
#define XENIA_KERNEL_XAM_XAM_AVATAR_H_

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/avatar.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

struct X_AVATAR_METADATA {
  uint8_t manifest[kMaxUserDataSize];
};
static_assert_size(X_AVATAR_METADATA, 0x3E8);

// v2 assets its int32_t
enum X_BINARY_ASSET_TYPE : uint32_t {
  Component = 1,
  Texture = 2,
  ShapeOverride = 3,
  Animation = 4,
  ShapeOverridePost = 5,
};

enum X_ASSET_SUBCATEGORY : int32_t {
  // costume 0x64 - 0x6A
  CostumeShortDress = 0x64,
  CostumeLongDress = 0x65,
  CostumeFormalSuit = 0x67,
  CostumeCasualSuit = 0x68,
  CostumeCostume = 0x69,
  // shirt 0xC8 - 0xD2
  ShirtSportsTee = 0xC8,
  ShirtTee = 0xC9,
  ShirtVest = 0xCA,
  ShirtPolo = 0xCB,
  ShirtLongSleeveTee = 0xCC,
  ShirtShortSleeveShirt = 0xCD,
  ShirtLongSleeveShirt = 0xCE,
  ShirtSweater = 0xCF,
  ShirtHoodie = 0xD0,
  ShirtJacket = 0xD1,
  ShirtCoat = 0xD2,
  // pants 0x12C - 0x135
  PantsHotpants = 0x12C,
  PantsShorts = 0x12D,
  PantsLongShorts = 0x12E,
  PantsLeggings = 0x12F,
  PantsPants = 0x130,
  PantsCargo = 0x131,
  PantsJeans = 0x132,
  PantsShortSkirt = 0x133,
  PantsKilt = 0x134,
  PantsLongSkirt = 0x135,
  // shoes 0x190 - 0x197
  ShoesSandals = 0x190,
  ShoesHeels = 0x191,
  ShoesPumps = 0x192,
  ShoesFormal = 0x193,
  ShoesTrainers = 0x194,
  ShoesShortBoots = 0x195,
  ShoesHighBoots = 0x196,
  ShoesCostume = 0x197,
  // hat 0x1F4 - 0x1FD
  HatBeanie = 0x1F4,
  HatFlatCap = 0x1f5,
  HatBaseballCap = 0x1f6,
  HatPeakCap = 0x1f7,
  HatBrimmed = 0x1f8,
  HatFez = 0x1f9,
  HatHeadwrap = 0x1fa,
  HatCostume = 0x1fb,
  HatBearskin = 0x1fc,
  HatHelmet = 0x1fd,
  // gloves 0x258 - 0x259
  GlovesFingerless = 0x258,
  GlovesFullFingered = 0x259,
  // glasses 0x2BC - 0x2BE
  GlassesGlasses = 0x2BC,
  GlassesSunglasses = 0x2BD,
  GlassesCostume = 0x2BE,
  // wrist 0x320 - 0x323
  WristwearBracelet = 0x320,
  WristwearWatch = 0x321,
  WristwearBands = 0x322,
  WristwearSweatbands = 0x323,
  // earrings 0x384 - 0x38B
  EarringsSingleStud = 0x384,
  EarringsStuds = 0x385,
  EarringsSingleDangler = 0x386,
  EarringsDanglers = 0x387,
  EarringsSingleSmallHoop = 0x388,
  EarringsSmallHoops = 0x389,
  EarringsSingleLargeHoop = 0x38a,
  EarringsLargehoops = 0x38b,
  // ring 0x3E8 - 0x3EA
  RingRight = 0x3e8,
  RingLeft = 0x3e9,
  // carryable 0x44C - 0x44C
  Carryable = 0x44c,
};

enum X_SKELETON_VERSION : uint8_t {
  Nxe = 1,
  Natal,
  NxeAndNatal,
};

struct X_AVATAR_ASSETS {
  xe::be<uint32_t> skeleton_ptr;   // X_AVATAR_SKELETON*
  xe::be<uint32_t> carryable_ptr;  // X_AVATAR_CARRYABLE*
  xe::be<uint32_t> component_count;
  xe::be<uint32_t> component_info_ptr;    // X_AVATAR_COMPONENT_INFO*
  xe::be<uint32_t> component_models_ptr;  // X_AVATAR_MODEL*
};
static_assert_size(X_AVATAR_ASSETS, 0x14);

struct X_AVATAR_MODEL {
  xe::be<uint32_t> cpu_memory_buffer_size;
  xe::be<uint32_t> gpu_memory_buffer_size;
  xe::be<uint32_t> total_texture_data_size;
  xe::be<uint32_t> global_vertex_buffer_size;
  xe::be<uint32_t> global_index_buffer_size;
  xe::be<uint32_t> batch_count;
  xe::be<uint32_t> texture_count;
  xe::be<uint32_t> xpu_memory_buffer_ptr;     // uint8_t*
  xe::be<uint32_t> gpu_memory_buffer_ptr;     // uint8_t*
  xe::be<uint32_t> global_vertex_buffer_ptr;  // uint8_t*
  xe::be<uint32_t> global_index_buffer_ptr;   // uint8_t*
  xe::be<uint32_t> batches_ptr;               // X_AVATAR_TRIANGLE_BATCH
  xe::be<uint32_t> textures_ptr;              // X_AVATAR_TEXTURE
};
static_assert_size(X_AVATAR_MODEL, 0x34);

struct X_AVATAR_SKELETON {
  xe::be<uint32_t> count;
  xe::be<uint32_t> joints_ptr;  // AVATAR_SKELETON_JOINT*
};
static_assert_size(X_AVATAR_SKELETON, 0x8);

struct X_AVATAR_COMPONENT_INFO {
  X_AVATAR_ASSET_ID model_asset_id;
  xe::be<uint16_t> component_mask;  // X_AVATAR_COMPONENT_MASK
  xe::be<uint32_t> reserved[3];
};
static_assert_size(X_AVATAR_COMPONENT_INFO, 0x20);

struct X_AVATAR_CARRYABLE {
  xe::be<uint32_t> skeleton_ptr;  // X_AVATAR_SKELETON*
  X_AVATAR_COMPONENT_INFO component_info;
  X_AVATAR_MODEL component_model;
  xe::be<uint32_t> animation_asset_ptr;  // X_AVATAR_ANIMATION_ASSET*
};
static_assert_size(X_AVATAR_CARRYABLE, 0x5C);

struct X_AVATAR_SKELETON_HIERARCHY_JOINT {
  xe::be<uint16_t> parent;
  xe::be<uint16_t> child;
  xe::be<uint16_t> sibling;
};
static_assert_size(X_AVATAR_SKELETON_HIERARCHY_JOINT, 0x6);

struct X_COLOR_GROUP {
  X_RGB_COLOR color;
  uint32_t unknown;
};
static_assert_size(X_COLOR_GROUP, 8);

struct X_COLOR_TABLE_ENTRY {
  X_COLOR_GROUP color[3];
};
static_assert_size(X_COLOR_TABLE_ENTRY, 24);

struct X_COLOR_TABLE {
  uint32_t count;
  uint32_t entries_ptr;  // X_COLOR_TABLE_ENTRY*
};
static_assert_size(X_COLOR_TABLE, 8);

struct X_AVATAR_ANIMATION {
  uint32_t frame_count;
  float duration;
  float frames_per_second;
};
static_assert_size(X_AVATAR_ANIMATION, 12);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_AVATAR_H_
