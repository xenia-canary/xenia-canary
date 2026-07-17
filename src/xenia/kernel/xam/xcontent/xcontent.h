/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XCONTENT_XCONTENT_H_
#define XENIA_KERNEL_XAM_XCONTENT_XCONTENT_H_

#include <cstdint>

#include "xenia/base/string_util.h"
#include "xenia/kernel/util/xex2_info.h"
#include "xenia/vfs/devices/stfs_xbox.h"

namespace xe {
namespace kernel {
namespace xam {
// If set in XCONTENT_DATA_AGGREGATE, will be substituted with the running
// titles ID
// TODO: check if actual x360 kernel/xam has a value similar to this
constexpr uint32_t kCurrentlyRunningTitleId = 0xFFFFFFFF;

struct XCONTENT_DATA;
struct XCONTENT_DATA_AGGREGATE;
struct XCONTENT_DATA_INTERNAL;

enum class XContentPackageType : uint32_t {
  kCon = 0x434F4E20,
  kPirs = 0x50495253,
  kLive = 0x4C495645,
};

enum class XContentVolumeType : uint32_t {
  kStfs = 0,
  kSvod = 1,
};

enum XContentFlag : uint32_t {
  kNone = 0,
  // Creation flags
  kCreateNew = 1,
  kCreateAlways = 2,
  kOpenExisting = 3,
  kOpenAlways = 4,
  kTruncateExisting = 5,

  // Attributes
  kNoProfileTransfer = 0x10,
  kNoDeviceTransfer = 0x20,
  kStronglySigned = 0x40,
  kAllowProfileTransfer = 0x80,
  kMoveOnly = 0x800,

  // Device selector?
  kManageStorage = 0x100,
  kForceUI = 0x200,

  // Enumeration
  kExcludeCommon = 0x1000,

  // Other
  kEnumerateAllProfiles = 0x10000000,

  // Device enumerator?
  kExcludeReadOnlyDevices = 0x80000000,
};

/* XContent structures */
struct XContentLicense {
  be<uint64_t> licensee_id;
  be<uint32_t> license_bits;
  be<uint32_t> license_flags;
};
static_assert_size(XContentLicense, 0x10);

struct XContentMediaData {
  uint8_t series_id[0x10];
  uint8_t season_id[0x10];
  be<uint16_t> season_number;
  be<uint16_t> episode_number;
};
static_assert_size(XContentMediaData, 0x24);

struct XContentAvatarAssetData {
  be<uint32_t> sub_category;
  be<uint32_t> colorizable;
  uint8_t asset_id[0x10];
  uint8_t skeleton_version_mask;
  uint8_t reserved[0xB];
};
static_assert_size(XContentAvatarAssetData, 0x24);

struct XContentAttributes {
  uint8_t profile_transfer : 1;
  uint8_t device_transfer : 1;
  uint8_t move_only_transfer : 1;
  uint8_t kinect_enabled : 1;
  uint8_t disable_network_storage : 1;
  uint8_t deep_link_supported : 1;
  uint8_t reserved : 2;
};
static_assert_size(XContentAttributes, 1);

#pragma pack(push, 1)
struct XContentMetadata {
  static constexpr uint32_t kThumbLengthV1 = 0x4000;
  static constexpr uint32_t kThumbLengthV2 = 0x3D00;

  static constexpr uint32_t kNumLanguagesV1 = 9;
  // metadata_version 2 adds 3 languages inside thumbnail/title_thumbnail space
  static constexpr uint32_t kNumLanguagesV2 = 12;

  be<XContentType> content_type;
  be<uint32_t> metadata_version;
  be<uint64_t> content_size;
  xex2_opt_execution_info execution_info;
  uint8_t console_id[5];
  be<uint64_t> profile_id;
  union {
    vfs::StfsVolumeDescriptor stfs;
    vfs::SvodDeviceDescriptor svod;
  } volume_descriptor;
  be<uint32_t> data_file_count;
  be<uint64_t> data_file_size;
  be<XContentVolumeType> volume_type;
  be<uint64_t> online_creator;
  be<uint32_t> category;
  uint8_t reserved2[0x20];
  union {
    XContentMediaData media_data;
    XContentAvatarAssetData avatar_asset_data;
  } metadata_v2;
  uint8_t device_id[0x14];
  union {
    be<uint16_t> uint[kNumLanguagesV1][128];
    char16_t chars[kNumLanguagesV1][128];
  } display_name_raw;
  union {
    be<uint16_t> uint[kNumLanguagesV1][128];
    char16_t chars[kNumLanguagesV1][128];
  } description_raw;
  union {
    be<uint16_t> uint[64];
    char16_t chars[64];
  } publisher_raw;
  union {
    be<uint16_t> uint[64];
    char16_t chars[64];
  } title_name_raw;
  union {
    uint8_t as_byte;
    XContentAttributes bits;
  } flags;
  be<uint32_t> thumbnail_size;
  be<uint32_t> title_thumbnail_size;
  uint8_t thumbnail[kThumbLengthV2];
  union {
    be<uint16_t> uint[kNumLanguagesV2 - kNumLanguagesV1][128];
    char16_t chars[kNumLanguagesV2 - kNumLanguagesV1][128];
  } display_name_ex_raw;
  uint8_t title_thumbnail[kThumbLengthV2];
  union {
    be<uint16_t> uint[kNumLanguagesV2 - kNumLanguagesV1][128];
    char16_t chars[kNumLanguagesV2 - kNumLanguagesV1][128];
  } description_ex_raw;

  std::u16string display_name(XLanguage language) const {
    uint32_t lang_id =
        language == XLanguage::kInvalid ? 1 : uint32_t(language) - 1;

    if (lang_id >= kNumLanguagesV2) {
      assert_always();
      // no room for this lang, read from english slot..
      lang_id = uint32_t(XLanguage::kEnglish) - 1;
    }

    const be<uint16_t>* str = 0;
    if (lang_id >= 0 && lang_id < kNumLanguagesV1) {
      str = display_name_raw.uint[lang_id];
    } else if (lang_id >= kNumLanguagesV1 && lang_id < kNumLanguagesV2 &&
               metadata_version >= 2) {
      str = display_name_ex_raw.uint[lang_id - kNumLanguagesV1];
    }

    if (!str) {
      // Invalid language ID?
      assert_always();
      return u"";
    }

    return load_and_swap<std::u16string>(str);
  }

  std::u16string description(XLanguage language) const {
    uint32_t lang_id = uint32_t(language) - 1;

    if (lang_id >= kNumLanguagesV2) {
      assert_always();
      // no room for this lang, read from english slot..
      lang_id = uint32_t(XLanguage::kEnglish) - 1;
    }

    const be<uint16_t>* str = 0;
    if (lang_id >= 0 && lang_id < kNumLanguagesV1) {
      str = description_raw.uint[lang_id];
    } else if (lang_id >= kNumLanguagesV1 && lang_id < kNumLanguagesV2 &&
               metadata_version >= 2) {
      str = description_ex_raw.uint[lang_id - kNumLanguagesV1];
    }

    if (!str) {
      // Invalid language ID?
      assert_always();
      return u"";
    }

    return load_and_swap<std::u16string>(str);
  }

  std::u16string publisher() const {
    return load_and_swap<std::u16string>(publisher_raw.uint);
  }

  std::u16string title_name() const {
    return load_and_swap<std::u16string>(title_name_raw.uint);
  }

  bool set_display_name(XLanguage language, const std::u16string_view value) {
    uint32_t lang_id = uint32_t(language) - 1;

    if (lang_id >= kNumLanguagesV2) {
      assert_always();
      // no room for this lang, store in english slot..
      lang_id = uint32_t(XLanguage::kEnglish) - 1;
    }

    char16_t* str = 0;
    if (lang_id >= 0 && lang_id < kNumLanguagesV1) {
      str = display_name_raw.chars[lang_id];
    } else if (lang_id >= kNumLanguagesV1 && lang_id < kNumLanguagesV2 &&
               metadata_version >= 2) {
      str = display_name_ex_raw.chars[lang_id - kNumLanguagesV1];
    }

    if (!str) {
      // Invalid language ID?
      assert_always();
      return false;
    }

    string_util::copy_and_swap_truncating(str, value,
                                          countof(display_name_raw.chars[0]));
    return true;
  }

  bool set_description(XLanguage language, const std::u16string_view value) {
    uint32_t lang_id = uint32_t(language) - 1;

    if (lang_id >= kNumLanguagesV2) {
      assert_always();
      // no room for this lang, store in english slot..
      lang_id = uint32_t(XLanguage::kEnglish) - 1;
    }

    char16_t* str = 0;
    if (lang_id >= 0 && lang_id < kNumLanguagesV1) {
      str = description_raw.chars[lang_id];
    } else if (lang_id >= kNumLanguagesV1 && lang_id < kNumLanguagesV2 &&
               metadata_version >= 2) {
      str = description_ex_raw.chars[lang_id - kNumLanguagesV1];
    }

    if (!str) {
      // Invalid language ID?
      assert_always();
      return false;
    }

    string_util::copy_and_swap_truncating(str, value,
                                          countof(description_raw.chars[0]));
    return true;
  }

  void set_publisher(const std::u16string_view value) {
    string_util::copy_and_swap_truncating(publisher_raw.chars, value,
                                          countof(publisher_raw.chars));
  }

  void set_title_name(const std::u16string_view value) {
    string_util::copy_and_swap_truncating(title_name_raw.chars, value,
                                          countof(title_name_raw.chars));
  }
};
static_assert_size(XContentMetadata, 0x93D6);

static constexpr uint8_t license_count = 0x10;

struct XContentHeader {
  be<XContentPackageType> magic;
  uint8_t signature[0x228];
  XContentLicense licenses[license_count];
  uint8_t content_id[0x14];
  be<uint32_t> header_size;

  bool is_magic_valid() const {
    return magic == XContentPackageType::kCon ||
           magic == XContentPackageType::kLive ||
           magic == XContentPackageType::kPirs;
  }
};
static_assert_size(XContentHeader, 0x344);
#pragma pack(pop)

struct XContentContainerHeader {
  XContentHeader content_header;
  XContentMetadata content_metadata;
  // TODO: title/system updates contain more data after XContentMetadata, seems
  // to affect header.header_size

  bool is_package_readonly() const {
    if (content_metadata.volume_type == XContentVolumeType::kSvod) {
      return true;
    }

    return content_metadata.volume_descriptor.stfs.flags.bits.read_only_format;
  }

  // Custom extension for Xenia to handle savefiles with janky filenames. Like
  // trailing space in filename.
  char file_name_raw[42];

  std::string file_name() const {
    std::string value;
    value.assign(file_name_raw, strnlen(file_name_raw, countof(file_name_raw)));
    return value;
  }
  void set_file_name(const std::string_view value) {
    // Filename is field that is only for CON directory "packages" in Xenia.
    if (content_header.magic != XContentPackageType::kCon) {
      return;
    }
    std::fill_n(file_name_raw, countof(file_name_raw), 0);
    string_util::copy_maybe_truncating<string_util::Safety::IKnowWhatIAmDoing>(
        file_name_raw, value, xe::countof(file_name_raw));
  }
};
static_assert_size(XContentContainerHeader, 0x971A + 42);

struct XCONTENT_DATA {
  be<uint32_t> device_id;         // 0x0 sz:0x4
  be<XContentType> content_type;  // 0x4 sz:0x4
  union {
    // this should be be<uint16_t>, but that stops copy constructor being
    // generated...
    uint16_t uint[128];
    char16_t chars[128];
  } display_name_raw;  // 0x8 sz:0x100

  char file_name_raw[42];  // 0x108 sz:0x2A

  // Some games use this padding field as a null-terminator, as eg.
  // DLC packages usually fill the entire file_name_raw array
  // Not every game sets it to 0 though, so make sure any file_name_raw reads
  // only go up to 42 chars!
  uint8_t padding[2];  // 0x132 sz: 0x2

  bool operator==(const XCONTENT_DATA& other) const {
    // Package is located via device_id/content_type/file_name, so only need to
    // compare those
    return device_id == other.device_id && content_type == other.content_type &&
           file_name() == other.file_name();
  }

  std::u16string display_name() const {
    return load_and_swap<std::u16string>(display_name_raw.uint);
  }

  std::string file_name() const {
    std::string value;
    value.assign(file_name_raw, strnlen(file_name_raw, countof(file_name_raw)));
    return value;
  }

  void set_display_name(const std::u16string_view value) {
    // Some games (e.g. 584108A9) require multiple null-terminators for it to
    // read the string properly, blanking the array should take care of that

    std::fill_n(display_name_raw.chars, countof(display_name_raw.chars), 0);
    string_util::copy_and_swap_truncating(display_name_raw.chars, value,
                                          countof(display_name_raw.chars));
  }

  void set_file_name(const std::string_view value) {
    std::fill_n(file_name_raw, countof(file_name_raw), 0);
    string_util::copy_maybe_truncating<string_util::Safety::IKnowWhatIAmDoing>(
        file_name_raw, value, xe::countof(file_name_raw));

    // Some games rely on padding field acting as a null-terminator...
    padding[0] = padding[1] = 0;
  }
};
static_assert_size(XCONTENT_DATA, 0x134);

struct XCONTENT_DATA_AGGREGATE : XCONTENT_DATA {
  be<uint64_t> xuid;  // some titles store XUID here?
  be<uint32_t> title_id;

  XCONTENT_DATA_AGGREGATE() = default;
  XCONTENT_DATA_AGGREGATE(const XCONTENT_DATA& other);
  XCONTENT_DATA_AGGREGATE(const XCONTENT_DATA_INTERNAL& other);

  bool operator==(const XCONTENT_DATA_AGGREGATE& other) const {
    // Package is located via device_id/title_id/content_type/file_name, so only
    // need to compare those
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type &&
           file_name() == other.file_name();
  }
};
static_assert_size(XCONTENT_DATA_AGGREGATE, 0x148);

struct XCONTENT_DATA_MEDIA {
  be<uint8_t> series_id[0x10];  // 0x0 sz:0x10
  be<uint8_t> seasonid[0x10];   // 0x10 sz:0x10
  be<uint16_t> season_number;   // 0x20 sz:0x2
  be<uint16_t> episode_number;  // 0x22 sz:0x2
};
static_assert_size(XCONTENT_DATA_MEDIA, 0x24);

struct XCONTENT_DATA_AVATAR_ASSET {
  be<uint32_t> subcategory;           // 0x0 sz:0x4
  be<int32_t> colorizable;            // 0x4 sz:0x4
  be<uint8_t> asset_id[0x10];         // 0x8 sz:0x10
  be<uint8_t> skeleton_version_mask;  // 0x18 sz:0x1
};
static_assert_size(XCONTENT_DATA_AVATAR_ASSET, 0x1C);

enum XCONTENT_INTERNAL_FLAGS : uint32_t {
  Partial = 0x00000001,
  Corrupt = 0x00000002,
  KinectEnabled = 0x00000004,
  DeepLinkSupported = 0x00000008,
  DisableNetworkStorage = 0x00000010,
  MoveOnlyTransfer = 0x00000020,
  DeviceTransfer = 0x00000040,
  ProfileTransfer = 0x00000080,
};

struct XCONTENT_DATA_INTERNAL : XCONTENT_DATA {  // 0x0 sz:0x134
  be<uint32_t> category;                         // 0x134 sz:0x4
  be<uint64_t> xuid;                             // 0x138 sz:0x8
  be<uint32_t> title_id;                         // 0x140 sz:0x4
  be<uint32_t> license_mask;                     // 0x144 sz:0x4

  // added in V4532 0x148
  be<uint64_t> content_size;  // 0x148 sz:0x8

  // added in NXE
  be<uint64_t> creation_time;     // 0x150 sz:0x8 FILETIME
  char16_t title_name_raw[0x40];  // 0x158 sz:0x80

  // compiler does not like
  // union {
  // XCONTENT_DATA_AVATAR_ASSET avatar_content_data;  // 0x1D8 sz:0x1C
  // XCONTENT_DATA_MEDIA media_content_data;          // 0x1D8 sz:0x24
  //};
  be<uint8_t> unused[0x24];                   // 0x1D8 sz:0x24
  be<XCONTENT_INTERNAL_FLAGS> xcontent_flag;  // 0x1FC sz:0x04

  XCONTENT_DATA_INTERNAL() = default;
  XCONTENT_DATA_INTERNAL(const XCONTENT_DATA& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    set_display_name(other.display_name());
    set_file_name(other.file_name());
    padding[0] = padding[1] = 0;
    category = 0;
    xuid = 0;
    title_id = kCurrentlyRunningTitleId;
    license_mask = 0;
    content_size = 0;
    creation_time = 0;
    set_title_name(u"");
    xcontent_flag = {};
    memset(unused, 0, xe::countof(unused));
  }
  XCONTENT_DATA_INTERNAL(const XCONTENT_DATA_AGGREGATE& other) {
    device_id = other.device_id;
    content_type = other.content_type;
    set_display_name(other.display_name());
    set_file_name(other.file_name());
    padding[0] = padding[1] = 0;
    category = 0;
    xuid = other.xuid;
    title_id = other.title_id;
    license_mask = 0;
    content_size = 0;
    creation_time = 0;
    set_title_name(u"");
    xcontent_flag = {};
    memset(unused, 0, xe::countof(unused));
  }

  bool operator==(const XCONTENT_DATA_INTERNAL& other) const {
    // Package is located via device_id/title_id/content_type/file_name, so only
    // need to compare those
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type &&
           file_name() == other.file_name();
  }
  bool operator==(const XCONTENT_DATA_AGGREGATE& other) const {
    return device_id == other.device_id && title_id == other.title_id &&
           content_type == other.content_type &&
           file_name() == other.file_name();
  }
  std::u16string title_name() const {
    return load_and_swap<std::u16string>(title_name_raw);
  }

  void set_title_name(const std::u16string_view value) {
    std::fill_n(title_name_raw, countof(title_name_raw), 0);
    string_util::copy_and_swap_truncating(title_name_raw, value,
                                          countof(title_name_raw));
  }
};
static_assert_size(XCONTENT_DATA_INTERNAL, 0x200);

inline XCONTENT_DATA_AGGREGATE::XCONTENT_DATA_AGGREGATE(
    const XCONTENT_DATA& other) {
  device_id = other.device_id;
  content_type = other.content_type;
  set_display_name(other.display_name());
  set_file_name(other.file_name());
  padding[0] = padding[1] = 0;
  xuid = 0;
  title_id = kCurrentlyRunningTitleId;
}

inline XCONTENT_DATA_AGGREGATE::XCONTENT_DATA_AGGREGATE(
    const XCONTENT_DATA_INTERNAL& other) {
  device_id = other.device_id;
  content_type = other.content_type;
  set_display_name(other.display_name());
  set_file_name(other.file_name());
  padding[0] = padding[1] = 0;
  xuid = other.xuid;
  title_id = other.title_id;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XCONTENT_XCONTENT_H_
