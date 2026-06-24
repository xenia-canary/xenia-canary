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
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

struct X_ASSET_ID {   // X_GUID
  uint64_t data;      // 0x0 sz:0x8
  uint32_t data2;     // 0x8 sz:0x4
  uint32_t title_id;  // 0xC sz:0x4
};
static_assert_size(X_ASSET_ID, 0x10);

// More Research Needed
struct X_AVATAR_METADATA {
  // body type exists between 0x120 and 0x130
  uint8_t data1[0x4];
  uint32_t weight;           // 0x4 sz:0x4
  uint32_t height;           // 0x8 sz:0x4
  uint8_t data3[0xF0];       // 0xC sz:0xF0
  uint32_t skin_color;       // 0xFC sz:0x4
  uint8_t data4[0x4];        // 0x100 sz:0x4
  uint32_t lipstick_color;   // 0x104 sz:0x4
  uint8_t data5[0x8];        // 0x108 sz:0x4
  uint32_t eyeshadow_color;  // 0x110 sz:0x4
  uint8_t data6[0x2D4];
};
static_assert_size(X_AVATAR_METADATA, 0x3E8);

// https://github.com/hetelek/Velocity/blob/master/XboxInternals/AvatarAsset/AvatarAssetDefinintions.h
enum X_AVATAR_BODY_TYPE : uint8_t { Unknown, Male, Female, All };

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

#pragma pack(push, 1)
struct X_ASSET_METADATA {
  uint8_t metadata_version;
  X_AVATAR_BODY_TYPE gender;
  X_BINARY_ASSET_TYPE type;
  uint32_t asset_type_details;
  X_ASSET_SUBCATEGORY category;
  X_SKELETON_VERSION skeleton_version;
};
static_assert_size(X_ASSET_METADATA, 15);
#pragma pack(pop)

struct X_RGB_COLOR {
  uint8_t blue;
  uint8_t green;
  uint8_t red;
  uint8_t alpha;
};
static_assert_size(X_RGB_COLOR, 4);

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
