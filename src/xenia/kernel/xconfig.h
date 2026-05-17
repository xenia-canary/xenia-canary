/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XCONFIG_H_
#define XENIA_KERNEL_XCONFIG_H_

#include <array>
#include <concepts>
#include <cstdint>
#include <numeric>

#include "xenia/base/byte_order.h"
#include "xenia/base/mutex.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {

enum X_CONFIG_CATEGORY : uint16_t {
  XCONFIG_STATIC_CATEGORY = 0x00,
  XCONFIG_STATISTIC_CATEGORY = 0x01,
  XCONFIG_SECURED_CATEGORY = 0x02,
  XCONFIG_USER_CATEGORY = 0x03,
  XCONFIG_XNET_MACHINE_ACCOUNT_CATEGORY = 0x04,
  XCONFIG_XNET_PARAMETERS_CATEGORY = 0x05,
  XCONFIG_MEDIA_CENTER_CATEGORY = 0x06,
  XCONFIG_CONSOLE_CATEGORY = 0x07,
  XCONFIG_DVD_CATEGORY = 0x08,
  XCONFIG_IPTV_CATEGORY = 0x09,
  XCONFIG_SYSTEM_CATEGORY = 0x0A,
  XCONFIG_DEVKIT_CATEGORY = 0x0B
};

enum XCONFIG_STATIC_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_STATIC_FIRST_POWER_ON_DATE = 0x01,
  XCONFIG_STATIC_SMC_CONFIG = 0x02
};

enum XCONFIG_STATISTIC_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_STATISTICS_XUID_MAC_ADDRESS = 0x01,
  XCONFIG_STATISTICS_XUID_COUNT = 0x02,
  XCONFIG_STATISTICS_ODD_FAILURES = 0x03,
  XCONFIG_STATISTICS_HDD_SMART_DATA = 0x04,
  XCONFIG_STATISTICS_UEM_ERRORS = 0x05,
  XCONFIG_STATISTICS_FPM_ERRORS = 0x06,
  XCONFIG_STATISTICS_LAST_REPORT_TIME = 0x07,
  XCONFIG_STATISTICS_BUG_CHECK_DATA = 0x08,
  XCONFIG_STATISTICS_TEMPERATURE = 0x09,
  XCONFIG_STATISTICS_XEKEYS_WRITE_FAILURE = 0x0A
};

enum XCONFIG_SECURED_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_SECURED_MAC_ADDRESS = 0x01,
  XCONFIG_SECURED_AV_REGION = 0x02,
  XCONFIG_SECURED_GAME_REGION = 0x03,
  XCONFIG_SECURED_DVD_REGION = 0x04,
  XCONFIG_SECURED_RESET_KEY = 0x05,
  XCONFIG_SECURED_SYSTEM_FLAGS = 0x06,
  XCONFIG_SECURED_POWER_MODE = 0x07,
  XCONFIG_SECURED_ONLINE_NETWORK_ID = 0x08,
  XCONFIG_SECURED_POWER_VCS_CONTROL = 0x09,
  XCONFIG_SECURED_UNKNOWN = 0x10
};

enum XCONFIG_USER_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_USER_TIME_ZONE_BIAS = 0x01,
  XCONFIG_USER_TIME_ZONE_STD_NAME = 0x02,
  XCONFIG_USER_TIME_ZONE_DLT_NAME = 0x03,
  XCONFIG_USER_TIME_ZONE_STD_DATE = 0x04,
  XCONFIG_USER_TIME_ZONE_DLT_DATE = 0x05,
  XCONFIG_USER_TIME_ZONE_STD_BIAS = 0x06,
  XCONFIG_USER_TIME_ZONE_DLT_BIAS = 0x07,
  XCONFIG_USER_DEFAULT_PROFILE = 0x08,
  XCONFIG_USER_LANGUAGE = 0x09,
  XCONFIG_USER_VIDEO_FLAGS = 0x0A,
  XCONFIG_USER_AUDIO_FLAGS = 0x0B,
  XCONFIG_USER_RETAIL_FLAGS = 0x0C,
  XCONFIG_USER_DEVKIT_FLAGS = 0x0D,
  XCONFIG_USER_COUNTRY = 0x0E,
  XCONFIG_USER_PC_FLAGS = 0x0F,
  XCONFIG_USER_SMB_CONFIG = 0x10,
  XCONFIG_USER_LIVE_PUID = 0x11,
  XCONFIG_USER_LIVE_CREDENTIALS = 0x12,
  XCONFIG_USER_AV_COMPOSITE_SCREENSZ = 0x13,  // Used for any other case
  XCONFIG_USER_AV_COMPONENT_SCREENSZ =
      0x14,  // Used only if avpack is set to component (3)
  XCONFIG_USER_AV_VGA_SCREENSZ = 0x15,  // Used only if avpack is set to VGA (6)
  XCONFIG_USER_PC_GAME = 0x16,
  XCONFIG_USER_PC_PASSWORD = 0x17,
  XCONFIG_USER_PC_MOVIE = 0x18,
  XCONFIG_USER_PC_GAME_RATING = 0x19,
  XCONFIG_USER_PC_MOVIE_RATING = 0x1A,
  XCONFIG_USER_PC_HINT = 0x1B,
  XCONFIG_USER_PC_HINT_ANSWER = 0x1C,
  XCONFIG_USER_PC_OVERRIDE = 0x1D,
  XCONFIG_USER_MUSIC_PLAYBACK_MODE = 0x1E,
  XCONFIG_USER_MUSIC_VOLUME = 0x1F,
  XCONFIG_USER_MUSIC_FLAGS = 0x20,
  XCONFIG_USER_ARCADE_FLAGS = 0x21,
  XCONFIG_USER_PC_VERSION = 0x22,
  XCONFIG_USER_PC_TV = 0x23,
  XCONFIG_USER_PC_TV_RATING = 0x24,
  XCONFIG_USER_PC_EXPLICIT_VIDEO = 0x25,
  XCONFIG_USER_PC_EXPLICIT_VIDEO_RATING = 0x26,
  XCONFIG_USER_PC_UNRATED_VIDEO = 0x27,
  XCONFIG_USER_PC_UNRATED_VIDEO_RATING = 0x28,
  XCONFIG_USER_VIDEO_OUTPUT_BLACK_LEVELS = 0x29,
  XCONFIG_USER_VIDEO_PLAYER_DISPLAY_MODE = 0x2A,
  XCONFIG_USER_ALTERNATE_VIDEO_TIMING_ID = 0x2B,
  XCONFIG_USER_VIDEO_DRIVER_OPTIONS = 0x2C,
  XCONFIG_USER_MUSIC_UI_FLAGS = 0x2D,
  XCONFIG_USER_VIDEO_MEDIA_SOURCE_TYPE = 0x2E,
  XCONFIG_USER_MUSIC_MEDIA_SOURCE_TYPE = 0x2F,
  XCONFIG_USER_PHOTO_MEDIA_SOURCE_TYPE = 0x30
};

enum XCONFIG_XNET_MACHINE_ACCOUNT_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_XNET_DATA = 0x01
};

enum XCONFIG_MEDIA_CENTER_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_MEDIA_CENTER_MEDIA_PLAYER = 0x01,
  XCONFIG_MEDIA_CENTER_XESLED_VERSION = 0x02,
  XCONFIG_MEDIA_CENTER_XESLED_TRUST_SECRET = 0x03,
  XCONFIG_MEDIA_CENTER_XESLED_TRUST_CODE = 0x04,
  XCONFIG_MEDIA_CENTER_XESLED_HOST_ID = 0x05,
  XCONFIG_MEDIA_CENTER_XESLED_KEY = 0x06,
  XCONFIG_MEDIA_CENTER_XESLED_HOST_MAC_ADDRESS = 0x07,
  XCONFIG_MEDIA_CENTER_SERVER_UUID = 0x08,
  XCONFIG_MEDIA_CENTER_SERVER_NAME = 0x09,
  XCONFIG_MEDIA_CENTER_SERVER_FLAG = 0x0A
};

enum XCONFIG_CONSOLE_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_CONSOLE_SCREEN_SAVER = 0x01,
  XCONFIG_CONSOLE_AUTO_SHUT_OFF = 0x02,
  XCONFIG_CONSOLE_WIRELESS_SETTINGS = 0x03,
  XCONFIG_CONSOLE_CAMERA_SETTINGS = 0x04,
  XCONFIG_CONSOLE_PLAYTIMERDATA = 0x05,
  XCONFIG_CONSOLE_MEDIA_DISABLEAUTOLAUNCH = 0x06,
  XCONFIG_CONSOLE_KEYBOARD_LAYOUT = 0x07,
  XCONFIG_CONSOLE_PC_TITLE_EXEMPTIONS = 0x08,
  XCONFIG_CONSOLE_NUI = 0x09,
  XCONFIG_CONSOLE_VOICE = 0x0A,
  XCONFIG_CONSOLE_RETAIL_EX_FLAGS = 0x0B,
  XCONFIG_CONSOLE_DASH_FIRST_USE_TUTORIAL_FLAGS = 0x0C,
  XCONFIG_CONSOLE_TV_DIAGONAL_SIZE_IN_CM = 0x0D,
  XCONFIG_CONSOLE_NETWORKSTORAGEDEVICE_SERIALNUMBER = 0x0E,
  XCONFIG_CONSOLE_DISCOVERABLE = 0x0F,
  XCONFIG_CONSOLE_LIVE_TV_PROVIDER = 0x10
};

enum XCONFIG_DVD_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_DVD_VOLUME_ID = 0x01,
  XCONFIG_DVD_BOOKMARK = 0x02
};

enum XCONFIG_IPTV_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_IPTV_SERVICE_PROVIDER_NAME = 0x01,
  XCONFIG_IPTV_PROVISIONING_SERVER_URL = 0x02,
  XCONFIG_IPTV_SUPPORT_INFO = 0x03,
  XCONFIG_IPTV_BOOTSTRAP_SERVER_URL = 0x04
};

enum XCONFIG_SYSTEM_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_SYSTEM_ALARM_TIME = 0x01,
  XCONFIG_SYSTEM_PREVIOUS_FLASH_VERSION = 0x02
};

enum XCONFIG_DEVKIT_CATEGORY_ENTRIES : uint8_t {
  XCONFIG_DEVKIT_USBD_ROOT_HUB_PORT_DISABLE_MASK = 0x01,
  XCONFIG_DEVKIT_XAM_FEATURE_ENABLE_DISABLE_MASK = 0x02,
  XCONFIG_DEVKIT_KIOSK_ID = 0x03
};

enum class X_AV_VIDEO_STANDARD : uint32_t {
  NTSCM = 1,
  NTSCJ = 2,
  PAL = 3,
  PAL_50 = 4,
};

// XCONFIG_SECURED_AV_REGION
enum X_AV_REGION : uint32_t {
  NTSCM = 0x00400100,
  NTSCJ = 0x00400200,
  PAL = 0x00400400,
  PAL_50 = 0x00800300,
};

// XCONFIG_USER_VIDEO_FLAGS
enum X_VIDEO_FLAGS : uint32_t {
  RatioNormal = 0x00000000,
  Widescreen = 0x00010000,
};

constexpr std::array<uint8_t, 4> kNoDate = {0x00, 0x00, 0x00, 0x00};
constexpr std::array<uint8_t, 4> kUS_StdDate = {0x0A, 0x05, 0x00, 0x02};
constexpr std::array<uint8_t, 4> kUS_DltDate = {0x04, 0x01, 0x00, 0x02};
constexpr std::array<uint8_t, 4> kEU_StdDate = {0x0A, 0x05, 0x00, 0x03};
constexpr std::array<uint8_t, 4> kEU_DltDate = {0x03, 0x05, 0x00, 0x02};

struct TimeZone {
  xe::be<int32_t> timezone_bias;
  std::array<char, 4> tz_std_name;
  std::array<char, 4> tz_dlt_name;
  std::array<uint8_t, 4> tz_std_date;
  std::array<uint8_t, 4> tz_dlt_date;
  xe::be<int32_t> tz_std_bias;
  xe::be<int32_t> tz_dlt_bias;

  std::string name;

  TimeZone() = default;

  TimeZone(xe::be<int32_t> timezone_bias, const std::array<char, 4> std_name,
           const std::array<char, 4> dlt_name,
           const std::array<uint8_t, 4> std_date,
           const std::array<uint8_t, 4> dlt_date, xe::be<int32_t> tz_std_bias,
           xe::be<int32_t> tz_dlt_bias, std::string name = "")
      : name(name),
        timezone_bias(timezone_bias),
        tz_std_bias(tz_std_bias),
        tz_dlt_bias(tz_dlt_bias) {
    std::memcpy(tz_std_name.data(), std_name.data(), 4);
    std::memcpy(tz_dlt_name.data(), dlt_name.data(), 4);
    std::memcpy(tz_std_date.data(), std_date.data(), 4);
    std::memcpy(tz_dlt_date.data(), dlt_date.data(), 4);
  }

  // Ctors used only to make entries in table below.
  static TimeZone NoDST(int32_t bias, std::string_view std_name,
                        std::string name) {
    std::array<char, 4> std_arr{};
    std::memcpy(std_arr.data(), std_name.data(),
                std::min(std_name.size(), std_arr.size()));
    return {bias, std_arr, {'\0', '\0', '\0', '\0'}, kNoDate, kNoDate, 0,
            0,    name};
  }

  static TimeZone WithDST(int32_t bias, std::string_view std_name,
                          std::string_view dlt_name,
                          std::array<uint8_t, 4> std_date,
                          std::array<uint8_t, 4> dlt_date, int32_t dlt_bias,
                          std::string name) {
    std::array<char, 4> std_arr{};
    std::array<char, 4> dlt_arr{};
    std::memcpy(std_arr.data(), std_name.data(),
                std::min(std_name.size(), std_arr.size()));
    std::memcpy(dlt_arr.data(), dlt_name.data(),
                std::min(dlt_name.size(), dlt_arr.size()));
    return {bias, std_arr, dlt_arr, std_date, dlt_date, 0, dlt_bias, name};
  }

  bool operator==(const TimeZone& other) const {
    return timezone_bias == other.timezone_bias &&
           std::memcmp(tz_std_name.data(), other.tz_std_name.data(), 4) == 0 &&
           std::memcmp(tz_dlt_name.data(), other.tz_dlt_name.data(), 4) == 0 &&
           std::memcmp(tz_std_date.data(), other.tz_std_date.data(), 4) == 0 &&
           std::memcmp(tz_dlt_date.data(), other.tz_dlt_date.data(), 4) == 0 &&
           tz_std_bias == other.tz_std_bias && tz_dlt_bias == other.tz_dlt_bias;
  }
};

inline const std::array<TimeZone, 75> kTimezones = {
    TimeZone::NoDST(720, "IDLW", "GMT-12 Tokelau"),
    TimeZone::NoDST(660, "NT", "GMT-11 Samoa"),
    TimeZone::NoDST(600, "HST", "GMT-10 Hawaii"),
    TimeZone::WithDST(540, "YST", "YDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-09 Alaska"),
    TimeZone::WithDST(480, "PST", "PDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-08 Pacific (U.S. & Canada)"),
    TimeZone::NoDST(420, "AMST", "GMT-07 Arizona"),
    TimeZone::WithDST(420, "MST", "MST", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-07 Mountain (U.S. & Canada)"),
    TimeZone::NoDST(360, "CAST", "GMT-06 Central America"),
    TimeZone::WithDST(360, "CST", "CDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-06 Central (U.S. & Canada)"),
    TimeZone::WithDST(360, "MST", "MDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-06 Mexico City"),
    TimeZone::NoDST(360, "CCST", "GMT-06 Saskatchewan"),
    TimeZone::WithDST(300, "EST", "EDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-05 Eastern (U.S. & Canada)"),
    TimeZone::NoDST(300, "EST", "GMT-05 Indiana"),
    TimeZone::NoDST(300, "SPST", "GMT-05 Bogota"),
    TimeZone::WithDST(240, "AST", "ADT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-04 Atlantic (U.S. & Canada)"),
    TimeZone::NoDST(240, "SWST", "GMT-04 Caracas"),
    TimeZone::WithDST(240, "PSST", "PSDT", {0x03, 0x02, 0x06, 0x00},
                      {0x0A, 0x02, 0x06, 0x00}, -60, "GMT-04 Santiago"),
    TimeZone::WithDST(210, "NST", "NDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-03:30 Newfoundland"),
    TimeZone::WithDST(180, "ESST", "ESDT", {0x02, 0x02, 0x00, 0x02},
                      {0x0A, 0x03, 0x00, 0x02}, -60, "GMT-03 Brasilia"),
    TimeZone::NoDST(180, "SEST", "GMT-03 Buenos Aires"),
    TimeZone::WithDST(180, "GST", "GDT", kUS_StdDate, kUS_DltDate, -60,
                      "GMT-03 Greenland"),
    TimeZone::WithDST(120, "MAST", "MADT", {0x09, 0x05, 0x00, 0x02},
                      {0x03, 0x05, 0x00, 0x02}, -60, "GMT-02 Mid-Atlantic"),
    TimeZone::WithDST(60, "AST", "ADT", {0x0A, 0x05, 0x00, 0x03},
                      {0x03, 0x05, 0x00, 0x02}, -60, "GMT-01 Azores"),
    TimeZone::NoDST(60, "WAT", "GMT-01 Cape Verde"),
    TimeZone::NoDST(0, "GST", "GMT+00 Casablanca"),
    TimeZone::WithDST(0, "GMT", "BST", {0x0A, 0x05, 0x00, 0x02},
                      {0x03, 0x05, 0x00, 0x01}, -60, "GMT+00 London"),
    TimeZone::WithDST(-60, "WEST", "WEDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+01 Berlin"),
    TimeZone::WithDST(-60, "CEST", "CEDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+01 Belgrade"),
    TimeZone::WithDST(-60, "RST", "RDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+01 Paris, Madrid"),
    TimeZone::WithDST(-60, "SCST", "SCDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+01 Sarajevo"),
    TimeZone::NoDST(-60, "WAST", "GMT+01 W. Central Africa"),
    TimeZone::WithDST(-120, "GTST", "GTDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+02 Athens"),
    TimeZone::WithDST(-120, "EEST", "EEDT", {0x09, 0x05, 0x00, 0x01},
                      {0x03, 0x05, 0x00, 0x00}, -60, "GMT+02 Bucharest"),
    TimeZone::WithDST(-120, "EST", "EDT", {0x09, 0x05, 0x03, 0x02},
                      {0x05, 0x01, 0x05, 0x02}, -60, "GMT+02 Cairo"),
    TimeZone::NoDST(-120, "SAST", "GMT+02 Pretoria"),
    TimeZone::WithDST(-120, "FLST", "FLDT", {0x0A, 0x05, 0x00, 0x04},
                      {0x03, 0x05, 0x00, 0x03}, -60, "GMT+02 Helsinki"),
    TimeZone::NoDST(-120, "JST", "GMT+02 Jerusalem"),
    TimeZone::WithDST(-180, "AST", "ADT", {0x0A, 0x01, 0x00, 0x04},
                      {0x04, 0x01, 0x00, 0x03}, -60, "GMT+03 Baghdad"),
    TimeZone::NoDST(-180, "AST", "GMT+03 Kuwait"),
    TimeZone::WithDST(-180, "RST", "RDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+03 Moscow"),
    TimeZone::NoDST(-180, "EAST", "GMT+03 Nairobi"),
    TimeZone::WithDST(-210, "IST", "IDT", {0x09, 0x04, 0x02, 0x02},
                      {0x03, 0x01, 0x00, 0x02}, -60, "GMT+03:30 Tehran"),
    TimeZone::NoDST(-240, "AST", "GMT+04 Abu Dhabi"),
    TimeZone::WithDST(-240, "CST", "CDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+04 Baku"),
    TimeZone::NoDST(-270, "AST", "GMT+04:30 Kabul"),
    TimeZone::WithDST(-300, "EST", "EDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+05 Ekaterinburg"),
    TimeZone::NoDST(-300, "WAST", "GMT+05 Islamabad"),
    TimeZone::NoDST(-330, "IST", "GMT+05:30 New Delhi"),
    TimeZone::NoDST(-345, "NST", "GMT+05:45 Kathmandu"),
    TimeZone::WithDST(-360, "NCST", "NCDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+06 Almaty"),
    TimeZone::NoDST(-360, "CAST", "GMT+06 Dhaka"),
    TimeZone::NoDST(-360, "SRST", "GMT+06 Sri Lanka"),
    TimeZone::NoDST(-390, "MST", "GMT+06:30 Yangon"),
    TimeZone::NoDST(-420, "SAST", "GMT+07 Bangkok"),
    TimeZone::WithDST(-420, "NAST", "NADT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+07 Krasnoyarsk"),
    TimeZone::NoDST(-480, "CST", "GMT+08 Beijing"),
    TimeZone::WithDST(-480, "NEST", "NEDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+08 Irkutsk"),
    TimeZone::NoDST(-480, "MPST", "GMT+08 Singapore"),
    TimeZone::NoDST(-480, "AWST", "GMT+08 Perth"),
    TimeZone::NoDST(-480, "TST", "GMT+08 Taipei"),
    TimeZone::NoDST(-540, "TST", "GMT+09 Tokyo"),
    TimeZone::NoDST(-540, "KST", "GMT+09 Seoul"),
    TimeZone::WithDST(-540, "YST", "YDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+09 Yakutsk"),
    TimeZone::WithDST(-570, "ACST", "ACDT", {0x03, 0x05, 0x00, 0x02},
                      {0x0A, 0x05, 0x00, 0x02}, -60, "GMT+09:30 Adelaide"),
    TimeZone::NoDST(-570, "ACST", "GMT+09:30 Darwin"),
    TimeZone::NoDST(-600, "AEST", "GMT+10 Brisbane"),
    TimeZone::WithDST(-600, "AEST", "AEDT", {0x03, 0x05, 0x00, 0x02},
                      {0x0A, 0x05, 0x00, 0x02}, -60, "GMT+10 Sydney"),
    TimeZone::NoDST(-600, "WPST", "GMT+10 Guam"),
    TimeZone::WithDST(-600, "TST", "TDT", {0x03, 0x05, 0x00, 0x02},
                      {0x0A, 0x01, 0x00, 0x02}, -60, "GMT+10 Hobart"),
    TimeZone::WithDST(-600, "VST", "VDT", kEU_StdDate, kEU_DltDate, -60,
                      "GMT+10 Vladivostok"),
    TimeZone::NoDST(-660, "CPST", "GMT+11 Solomon Islands"),
    TimeZone::WithDST(-720, "NZST", "NZDT", {0x03, 0x03, 0x00, 0x02},
                      {0x0A, 0x01, 0x00, 0x02}, -60, "GMT+12 Auckland"),
    TimeZone::NoDST(-720, "FST", "GMT+12 Fiji Islands"),
    TimeZone::NoDST(-780, "TST", "GMT+13 Nuku'alofa"),
    TimeZone::NoDST(-840, "KST", "GMT+14 Kiribati"),
};

struct Resolution {
  std::string name_;
  xe::be<uint16_t> width_;
  xe::be<uint16_t> height_;

  Resolution(uint16_t width, uint16_t height) : width_(width), height_(height) {
    name_ = fmt::format("{}x{}", width, height);
  };

  Resolution(uint32_t value) : width_(value >> 16), height_(value) {
    name_ = fmt::format("{}x{}", width_.get(), height_.get());
  };

  uint32_t to_host() const {
    return (static_cast<uint32_t>(width_.get()) << 16) |
           static_cast<uint32_t>(height_.get());
  }

  std::pair<uint16_t, uint16_t> aspect_ratio() const {
    const uint16_t divisor = std::gcd(width_.get(), height_.get());
    return {width_.get() / divisor, height_.get() / divisor};
  }

  bool is_widescreen() const {
    return aspect_ratio() != std::make_pair<uint16_t, uint16_t>(4, 3);
  }
};

// Not sure why HDMI is assigned to composite, but ok.
// XCONFIG_USER_AV_COMPOSITE_SCREENSZ
inline const std::array<Resolution, 3> XHDTVResolution = {
    Resolution(640, 480),   // 0x028001E0
    Resolution(1280, 720),  // 0x050002D0, always widescreen
    Resolution(1920,
               1080)  // 0x07800438, interlaced added in 1888, always widescreen
};

// XCONFIG_USER_AV_COMPONENT_SCREENSZ
inline const std::array<Resolution, 20> XVGAResolution = {
    Resolution(640, 480),  Resolution(640, 576),  Resolution(720, 480),
    Resolution(720, 576),  Resolution(800, 600),  Resolution(848, 480),
    Resolution(1024, 768), Resolution(1152, 864), Resolution(1280, 720),
    Resolution(1280, 768), Resolution(1280, 960), Resolution(1280, 1024),
    Resolution(1360, 768), Resolution(1440, 768), Resolution(1440, 900),
    Resolution(1600, 768), Resolution(1680, 720), Resolution(1680, 1050),
    Resolution(1920, 540), Resolution(1920, 1080)};

// XCONFIG_USER_AUDIO_FLAGS
enum X_AUDIO_FLAGS : uint32_t {
  // Audio Mode Analog
  DolbyProLogic = 0x00000001,
  AnalogMono = 0x00000002,
  // Audio Mode Digital
  DigitalStereo = 0x00000000,
  DolbyDigital = 0x00010000,
  DolbyDigitalWithWMAPRO = 0x00030000,
  // Special Flags
  StereoBypass = 0x00000003,
  LowLatency = 0x80000000,
};

// XCONFIG_USER_RETAIL_FLAGS
enum X_RETAIL_FLAGS : uint32_t {
  // Clock
  DSTOff = 0x00000002,
  TwentyFourHourClock = 0x00000008,

  // Startup
  DashboardStartup = 0x00000080,
  IPTVStartup = 0x00000800,
  DiscStartup = 0x00002000,
  MCXDownloaderStartup = 0x00020000,
  // IPTV
  IPTVEnabled = 0x00001000,
  IPTVDVREnabled = 0x00080000,
  IPTVDisabled = 0x02000000,
  // Kinect
  KinectInitialized = 0x20000000,
  KinectDisabled = 0x80000000,

  // Other
  DashboardInitialized = 0x00000040,
  BackgroundDownloadOn = 0x00010000,
};

// XCONFIG_USER_PC_FLAGS
enum X_PC_FLAGS : uint8_t {
  XBLAllowed = 0x01,
  XBLMembershipCreationAllowed = 0x02,
  XboxOneGameAllowed = 0x04,
  PCEnabled = 0x80,
};

// XCONFIG_USER_PC_GAMES
enum X_PC_GAMES_FLAGS : uint32_t {
  EarlyChildhoodMax = 0x00000000,
  EveryoneMax = 0x00000002,
  Everyone_10Max = 0x00000004,
  TeenMax = 0x00000006,
  MatureMax = 0x00000008,
  NoGameRestrictions = 0x000000FF,
};

// XCONFIG_USER_PC_PASSWORD
enum X_PC_PASSWORD_FLAGS : uint32_t {
  XButton = 0x00000001,
  YButton = 0x00000002,
  LeftButton = 0x00000003,
  RightButton = 0x00000004,
  UPButton = 0x00000005,
  DownButton = 0x00000006,
  LTButton = 0x00000009,
  RTButton = 0x0000000A,
  LBButton = 0x0000000B,
  RBButton = 0x0000000C,
};

// XCONFIG_USER_PC_MOVIE
enum X_PC_MOVIE_FLAGS : uint32_t {
  GeneralAudiences = 0x00000001,
  ParentalGuidance = 0x00000003,
  ParentalGuidance13 = 0x00000004,
  Restricted = 0x00000006,
  NoMovieRestrictions = 0x000000FF,
};

// XCONFIG_USER_PC_GAME_RATING
enum X_PC_GAME_RATING_FLAGS : uint32_t {
  DefaultGame = 0x00000000,
};

// XCONFIG_USER_PC_MOVIE_RATING
enum X_PC_MOVIE_RATING_FLAGS : uint32_t {
  DefaultMovie = 0x00000000,
};

// XCONFIG_USER_ARCADE_FLAGS
enum X_ARCADE_FLAGS : uint32_t {
  AutoDownloadOff = 0x00000000,
  AutoDownloadNewReleases = 0x00000001,
};

// XCONFIG_USER_PC_VERSION
enum X_PC_VERSION : uint32_t {
  VersionOne = 0x00000001,
};

// XCONFIG_USER_PC_TV
enum X_PC_TV : uint32_t {
  TVG = 0X00000006,
  TVPG = 0X00000008,
  TV14 = 0X0000000a,
  TVMatureAudience = 0x0000000C,
  NoTVRestrictions = 0X000000FF,
};

// XCONFIG_USER_PC_TV_RATING
enum X_PC_TV_RATING : uint32_t {
  DefaultTV = 0X00000000,
};

// XCONFIG_USER_PC_EXPLICIT_VIDEO
enum X_PC_EXPLICIT_VIDEO : uint32_t {
  ExplicitVideoAllowed = 0X000000FF,
  ExplicitVideoBanned = 0X00000000,
};

// XCONFIG_USER_PC_EXPLICIT_VIDEO_RATING
enum X_PC_EXPLICIT_VIDEO_RATING : uint32_t {
  ExplicitAllowed = 0X00000000,
};

// XCONFIG_USER_PC_UNRATED_VIDEO
enum X_PC_EXPLICIT_UNRATED : uint32_t {
  ExplicitUnratedBanned = 0X00000000,
  UnratedALL = 0X000000FF,
};

// XCONFIG_USER_PC_UNRATED_VIDEO_RATING
enum X_PC_EXPLICIT_UNRATED_RATING : uint32_t {
  DefaultExplicitUnrated = 0X00000000,
};

// XCONFIG_USER_VIDEO_OUTPUT_BLACK_LEVELS
enum X_BLACK_LEVEL : uint32_t {
  High = 0x00000100,
  Intermediate = 0x00000200,
  LevelNormal = 0x00000300,
};

// XCONFIG_CONSOLE_SCREENSAVER
enum X_SCREENSAVER : uint32_t {
  ScreensaverOn = 0x000A,
  ScreensaverOff = 0x1000,
};

// XCONFIG_CONSOLE_AUTO_SHUTDOWN
enum X_AUTO_SHUTDOWN : uint32_t {
  AutoShutdownOff = 0x0000,
  AutoShutdownOneHr = 0x003C,
  AutoShutdownSixHr = 0x0168,
};

// XCONFIG_CONSOLE_CAMERA_SETTINGS
enum X_CAMERA_FLAGS : uint32_t {
  // Room
  RoomAuto = 0x00000000,
  DarkWall = 0x00000001,
  LightWall = 0x00000002,
  // Lighting
  LightingAuto = 0x00000000,
  Incandescent = 0x00000004,
  Flourescent = 0x00000008,
  Daylight = 0x0000000C,
  // Flourescent Anti-Flicker
  AntiFlickerAuto = 0x00000000,
  AntiFlickerOn = 0x00000001,
  AntiFlickerOff = 0x00000002,
  // Default
  AutoAll = 0x00000001,
};

// XCONFIG_CONSOLE_KEYBOARD_LAYOUT
enum X_KEYBOARD_LAYOUT : uint16_t {
  KeyboardDefault = 0x0000,
  EnglishQWERTY = 0x0001,
};

#pragma pack(push, 1)

struct XConfigData {
  // -------------------------------------------------------------------------
  // 0x0 XCONFIG_STATIC_SETTINGS
  // -------------------------------------------------------------------------
  struct Static {
    xe::be<uint32_t> checksum;              // 0x000
    xe::be<uint32_t> version;               // 0x004
    std::array<uint8_t, 5> first_power_on;  // 0x008  date; key 0x1
    uint8_t _reserved0;                     // 0x00D
    std::array<uint8_t, 256> smc_block;     // 0x00E  union_SMC_BLOCK
  } static_settings;

  // -------------------------------------------------------------------------
  // 0x1 XCONFIG_STATISTIC_SETTINGS
  // -------------------------------------------------------------------------
  struct Statistic {
    xe::be<uint32_t> checksum;                  // 0x0000
    xe::be<uint32_t> version;                   // 0x0004
    std::array<uint8_t, 6> xuid_mac_address;    // 0x0008
    std::array<uint8_t, 2> _reserved0;          // 0x000E
    xe::be<uint32_t> xuid_count;                // 0x0010
    std::array<uint8_t, 32> odd_failures;       // 0x0014
    std::array<uint8_t, 101> bug_check_data;    // 0x0034
    std::array<uint8_t, 200> temperature_data;  // 0x0099
    std::array<uint8_t, 467> _unused;           // 0x0161
    std::array<uint8_t, 512> hdd_smart_data;    // 0x0334
    std::array<uint8_t, 100> uem_errors;        // 0x0534
    std::array<uint8_t, 56> fpm_errors;         // 0x0598
    xe::be<uint64_t> last_report_time;          // 0x05D0 FILETIME?
  } statistic;

  // -------------------------------------------------------------------------
  // 0x2 XCONFIG_SECURED_SETTINGS
  // -------------------------------------------------------------------------
  struct Secured {
    xe::be<uint32_t> checksum;                  // 0x00
    xe::be<uint32_t> version;                   // 0x04
    std::array<uint8_t, 4> online_network_id;   // 0x08
    std::array<uint8_t, 8> _reserved1;          // 0x0C
    std::array<uint8_t, 12> _reserved2;         // 0x14
    std::array<uint8_t, 6> mac_address;         // 0x20
    std::array<uint8_t, 2> _reserved3;          // 0x26
    xe::be<uint32_t> av_region;                 // 0x28
    xe::be<uint16_t> game_region;               // 0x2C
    std::array<uint8_t, 6> _reserved4;          // 0x2E
    xe::be<uint32_t> dvd_region;                // 0x34
    xe::be<uint32_t> reset_key;                 // 0x38
    xe::be<uint32_t> system_flags;              // 0x3C
    std::array<uint8_t, 2> power_mode;          // 0x40
    std::array<uint8_t, 2> power_vcs_control;   // 0x42
    std::array<uint8_t, 444> _reserved_region;  // 0x44
  } secured;

  // -------------------------------------------------------------------------
  // 0x3 XCONFIG_USER_SETTINGS
  // -------------------------------------------------------------------------
  struct User {
    xe::be<uint32_t> checksum;                   // 0x000
    xe::be<uint32_t> version;                    // 0x004
    xe::be<int32_t> time_zone_bias;              // 0x008
    std::array<char, 4> tz_std_name;             // 0x00C
    std::array<char, 4> tz_dlt_name;             // 0x010
    std::array<uint8_t, 4> tz_std_date;          // 0x014  XCONFIG_TIMEZONE_DATE
    std::array<uint8_t, 4> tz_dlt_date;          // 0x018  XCONFIG_TIMEZONE_DATE
    xe::be<int32_t> tz_std_bias;                 // 0x01C
    xe::be<int32_t> tz_dlt_bias;                 // 0x020
    xe::be<uint64_t> default_profile;            // 0x024
    xe::be<uint32_t> language;                   // 0x02C
    xe::be<uint32_t> video_flags;                // 0x030
    xe::be<uint32_t> audio_flags;                // 0x034
    xe::be<uint32_t> retail_flags;               // 0x038
    xe::be<uint32_t> devkit_flags;               // 0x03C
    xe::be<uint8_t> country;                     // 0x040
    xe::be<uint8_t> parental_control_flags;      // 0x041
    std::array<uint8_t, 2> _reserved_flag;       // 0x042
    std::array<uint8_t, 256> smb_config;         // 0x044
    xe::be<uint64_t> live_puid;                  // 0x144
    std::array<uint8_t, 16> live_credentials;    // 0x14C
    xe::be<int32_t> av_pack_hdmi_sz;             // 0x15C
    xe::be<int32_t> av_pack_component_sz;        // 0x160
    xe::be<int32_t> av_pack_vga_sz;              // 0x164
    xe::be<uint32_t> parental_control_game;      // 0x168
    xe::be<uint32_t> parental_control_password;  // 0x16C
    xe::be<uint32_t> parental_control_movie;     // 0x170
    xe::be<uint32_t> parental_control_game_rating;            // 0x174
    xe::be<uint32_t> parental_control_movie_rating;           // 0x178
    uint8_t parental_control_hint;                            // 0x17C
    std::array<uint8_t, 32> parental_control_hint_answer;     // 0x17D
    std::array<uint8_t, 32> parental_control_override;        // 0x19D
    xe::be<uint32_t> music_playback_mode;                     // 0x1BD
    xe::be<float> music_volume;                               // 0x1C1
    xe::be<uint32_t> music_flags;                             // 0x1C5
    xe::be<uint32_t> arcade_flags;                            // 0x1C9
    xe::be<uint32_t> parental_control_version;                // 0x1CD
    xe::be<uint32_t> parental_control_tv;                     // 0x1D1
    xe::be<uint32_t> parental_control_tv_rating;              // 0x1D5
    xe::be<uint32_t> parental_control_explicit_video;         // 0x1D9
    xe::be<uint32_t> parental_control_explicit_video_rating;  // 0x1DD
    xe::be<uint32_t> parental_control_unrated_video;          // 0x1E1
    xe::be<uint32_t> parental_control_unrated_video_rating;   // 0x1E5
    xe::be<uint32_t> video_output_black_levels;               // 0x1E9
    uint8_t video_player_display_mode;                        // 0x1ED
    xe::be<uint32_t> alternative_video_timing_ids;            // 0x1EE
    xe::be<uint32_t> video_driver_options;                    // 0x1F2
    xe::be<uint32_t> music_ui_flags;                          // 0x1F6
    uint8_t video_media_source_type;                          // 0x1FA
    uint8_t music_media_source_type;                          // 0x1FB
    uint8_t photo_media_source_type;                          // 0x1FC
  } user;

  // -------------------------------------------------------------------------
  // 0x4 XCONFIG_XNET_MACHINE_ACCOUNT
  // -------------------------------------------------------------------------
  struct XnetMachineAccount {
    xe::be<uint32_t> version;       // 0x000
    std::array<uint8_t, 492> data;  // 0x004
  } xnet_machine_account;

  // -------------------------------------------------------------------------
  // 0x5 XCONFIG_XNET_PARAMETERS
  // -------------------------------------------------------------------------
  struct XnetParameters {
    uint8_t cfg_size_of_struct;                      // 0x00
    uint8_t cfg_flags;                               // 0x01
    uint8_t cfg_sock_max_dgram_sockets;              // 0x02
    uint8_t cfg_sock_max_stream_sockets;             // 0x03
    uint8_t cfg_sock_default_recv_bufsize_in_k;      // 0x04
    uint8_t cfg_sock_default_send_bufsize_in_k;      // 0x05
    uint8_t cfg_key_reg_max;                         // 0x06
    uint8_t cfg_sec_reg_max;                         // 0x07
    uint8_t cfg_qos_data_limit_div4;                 // 0x08
    uint8_t cfg_qos_probe_timeout_in_seconds;        // 0x09
    uint8_t cfg_qos_probe_entries;                   // 0x0A
    uint8_t cfg_qos_srv_max_simultaneous_responses;  // 0x0B
    uint8_t cfg_qos_pair_wait_time_in_seconds;       // 0x0C
  } xnet_parameters;

  // -------------------------------------------------------------------------
  // 0x6 XCONFIG_MEDIA_CENTER_SETTINGS
  // -------------------------------------------------------------------------
  struct MediaCenter {
    xe::be<uint32_t> checksum;                        // 0x0000
    xe::be<uint32_t> version;                         // 0x0004
    std::array<uint8_t, 20> media_player;             // 0x0008
    std::array<uint8_t, 10> xe_sled_version;          // 0x001C
    std::array<uint8_t, 20> xe_sled_trust_secret;     // 0x0026
    std::array<uint8_t, 8> xe_sled_trust_code;        // 0x003A
    std::array<uint8_t, 20> xe_sled_host_id;          // 0x0042
    std::array<uint8_t, 1628> xe_sled_key;            // 0x0056
    std::array<uint8_t, 6> xe_sled_host_mac_address;  // 0x06B2
    std::array<uint8_t, 16> server_uuid;              // 0x06B8
    std::array<uint8_t, 128> server_name;             // 0x06C8
    std::array<uint8_t, 4> server_flags;              // 0x0748
  } media_center;

  // -------------------------------------------------------------------------
  // 0x7 XCONFIG_CONSOLE_SETTINGS
  // -------------------------------------------------------------------------
  struct Console {
    xe::be<uint32_t> checksum;                         // 0x000
    xe::be<uint32_t> version;                          // 0x004
    xe::be<int16_t> screen_saver;                      // 0x008
    xe::be<int16_t> auto_shut_off;                     // 0x00A
    std::array<uint8_t, 256> wireless_settings;        // 0x00C
    xe::be<uint32_t> camera_settings;                  // 0x10C
    std::array<uint8_t, 28> camera_settings_reserved;  // 0x110
    std::array<uint8_t, 20> play_timer_data;  // 0x12C  XCONFIG_PLAY_TIMER_DATA
    xe::be<int16_t> media_disable_auto_launch;  // 0x140
    xe::be<int16_t> keyboard_layout;            // 0x142
  } console;

  // -------------------------------------------------------------------------
  // 0x8 XCONFIG_DVD_SETTINGS
  // -------------------------------------------------------------------------
  struct Dvd {
    xe::be<uint32_t> version;           // 0x000
    std::array<uint8_t, 20> volume_id;  // 0x004
    std::array<uint8_t, 640> data;      // 0x018
  } dvd;

  // -------------------------------------------------------------------------
  // 0x9 XCONFIG_IPTV_SETTINGS
  // -------------------------------------------------------------------------
  struct Iptv {
    xe::be<uint32_t> checksum;                         // 0x000
    xe::be<uint32_t> version;                          // 0x004
    std::array<uint8_t, 120> service_provider_name;    // 0x008  wchar
    std::array<uint8_t, 128> provisioning_server_url;  // 0x080  wchar
    std::array<uint8_t, 128> support_info;             // 0x100  wchar
    std::array<uint8_t, 128> bootstrap_server_url;     // 0x180  wchar
  } iptv;

  // -------------------------------------------------------------------------
  // 0xA XCONFIG_SYSTEM_SETTINGS
  // -------------------------------------------------------------------------
  struct System {
    xe::be<uint32_t> version;                 // 0x00
    xe::be<uint64_t> alarm_time;              // 0x04  union_LARGE_INTEGER
    xe::be<uint32_t> previous_flash_version;  // 0x0C
  } system;
};

static_assert(offsetof(XConfigData::Secured, mac_address) == 32);
static_assert(offsetof(XConfigData::Secured, av_region) == 40);
static_assert(offsetof(XConfigData::User, language) == 44);
static_assert(offsetof(XConfigData::User, audio_flags) == 52);
static_assert(offsetof(XConfigData::User, retail_flags) == 56);
static_assert(offsetof(XConfigData::User, country) == 64);
static_assert(offsetof(XConfigData::User, music_volume) == 449);

#pragma pack(pop)

template <typename T>
concept XConfigValidValueType =
    std::same_as<T, uint8_t> || std::same_as<T, uint16_t> ||
    std::same_as<T, uint32_t> || std::same_as<T, uint64_t> ||
    std::same_as<T, float>;

class XConfig {
 public:
  XConfig() = delete;
  XConfig(const std::filesystem::path& xconfig_path);

  ~XConfig() = default;

  void WriteSetting(const X_CONFIG_CATEGORY category, const uint16_t setting_id,
                    const void* buffer);

  void ReadSetting(const X_CONFIG_CATEGORY category, const uint16_t setting_id,
                   void* buffer);
  template <XConfigValidValueType T>
  T ReadSetting(const X_CONFIG_CATEGORY category, const uint16_t setting_id) {
    const auto field = FindField(category, setting_id);

    if (!field || sizeof(T) != field->size) {
      return {};
    }

    T buffer;
    ReadSetting(category, setting_id, &buffer);

    buffer = xe::byte_swap<T>(buffer);

    return buffer;
  }

  uint16_t GetSettingSize(const X_CONFIG_CATEGORY category,
                          const uint16_t setting_id);

  const XConfigData* GetXConfig() const { return &xconfig_data_; }
  void WriteXConfig(const XConfigData* data);

  void SetDefaults();

 private:
  void FlushToFile();

  struct FieldDescriptor {
    X_CONFIG_CATEGORY category;
    uint16_t setting;
    uint16_t size;
    size_t block_offset;
  };

  static const FieldDescriptor* FindField(X_CONFIG_CATEGORY category,
                                          uint16_t setting);
  uint8_t* CategoryBase(X_CONFIG_CATEGORY category);
  const uint8_t* CategoryBase(X_CONFIG_CATEGORY category) const;

#define XCONFIG_FIELD(category, setting, block, member)       \
  {(category), (setting),                                     \
   static_cast<uint16_t>(sizeof(XConfigData::block::member)), \
   offsetof(XConfigData::block, member)}

  static inline constexpr XConfig::FieldDescriptor kFields[] = {
      // -- XCONFIG_STATIC (0x0) ---------------------------------------------
      XCONFIG_FIELD(XCONFIG_STATIC_CATEGORY, XCONFIG_STATIC_FIRST_POWER_ON_DATE,
                    Static, first_power_on),

      // -- XCONFIG_SECURED (0x2) --------------------------------------------
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_MAC_ADDRESS,
                    Secured, mac_address),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_AV_REGION,
                    Secured, av_region),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_GAME_REGION,
                    Secured, game_region),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_DVD_REGION,
                    Secured, dvd_region),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_RESET_KEY,
                    Secured, reset_key),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_SYSTEM_FLAGS,
                    Secured, system_flags),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_POWER_MODE,
                    Secured, power_mode),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_ONLINE_NETWORK_ID,
                    Secured, online_network_id),
      XCONFIG_FIELD(XCONFIG_SECURED_CATEGORY, XCONFIG_SECURED_POWER_VCS_CONTROL,
                    Secured, power_vcs_control),

      // -- XCONFIG_USER (0x3) -----------------------------------------------
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_BIAS, User,
                    time_zone_bias),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_STD_NAME,
                    User, tz_std_name),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_DLT_NAME,
                    User, tz_dlt_name),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_STD_DATE,
                    User, tz_std_date),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_DLT_DATE,
                    User, tz_dlt_date),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_STD_BIAS,
                    User, tz_std_bias),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_TIME_ZONE_DLT_BIAS,
                    User, tz_dlt_bias),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_DEFAULT_PROFILE, User,
                    default_profile),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_LANGUAGE, User,
                    language),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_VIDEO_FLAGS, User,
                    video_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_AUDIO_FLAGS, User,
                    audio_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS, User,
                    retail_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_DEVKIT_FLAGS, User,
                    devkit_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_COUNTRY, User, country),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_FLAGS, User,
                    parental_control_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_SMB_CONFIG, User,
                    smb_config),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_LIVE_PUID, User,
                    live_puid),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_LIVE_CREDENTIALS, User,
                    live_credentials),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_AV_COMPOSITE_SCREENSZ,
                    User, av_pack_hdmi_sz),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_AV_COMPONENT_SCREENSZ,
                    User, av_pack_component_sz),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_AV_VGA_SCREENSZ, User,
                    av_pack_vga_sz),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_GAME, User,
                    parental_control_game),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_PASSWORD, User,
                    parental_control_password),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_MOVIE, User,
                    parental_control_movie),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_GAME_RATING, User,
                    parental_control_game_rating),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_MOVIE_RATING, User,
                    parental_control_movie_rating),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_HINT, User,
                    parental_control_hint),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_HINT_ANSWER, User,
                    parental_control_hint_answer),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_OVERRIDE, User,
                    parental_control_override),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_MUSIC_PLAYBACK_MODE,
                    User, music_playback_mode),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_MUSIC_VOLUME, User,
                    music_volume),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_MUSIC_FLAGS, User,
                    music_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_ARCADE_FLAGS, User,
                    arcade_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_VERSION, User,
                    parental_control_version),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_TV, User,
                    parental_control_tv),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_TV_RATING, User,
                    parental_control_tv_rating),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_EXPLICIT_VIDEO, User,
                    parental_control_explicit_video),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY,
                    XCONFIG_USER_PC_EXPLICIT_VIDEO_RATING, User,
                    parental_control_explicit_video_rating),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_UNRATED_VIDEO, User,
                    parental_control_unrated_video),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PC_UNRATED_VIDEO_RATING,
                    User, parental_control_unrated_video_rating),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY,
                    XCONFIG_USER_VIDEO_OUTPUT_BLACK_LEVELS, User,
                    video_output_black_levels),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY,
                    XCONFIG_USER_VIDEO_PLAYER_DISPLAY_MODE, User,
                    video_player_display_mode),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY,
                    XCONFIG_USER_ALTERNATE_VIDEO_TIMING_ID, User,
                    alternative_video_timing_ids),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_VIDEO_DRIVER_OPTIONS,
                    User, video_driver_options),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_MUSIC_UI_FLAGS, User,
                    music_ui_flags),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_VIDEO_MEDIA_SOURCE_TYPE,
                    User, video_media_source_type),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_MUSIC_MEDIA_SOURCE_TYPE,
                    User, music_media_source_type),
      XCONFIG_FIELD(XCONFIG_USER_CATEGORY, XCONFIG_USER_PHOTO_MEDIA_SOURCE_TYPE,
                    User, photo_media_source_type),

      // -- XCONFIG_CONSOLE (0x7) --------------------------------------------
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_SCREEN_SAVER,
                    Console, screen_saver),
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_AUTO_SHUT_OFF,
                    Console, auto_shut_off),
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_WIRELESS_SETTINGS,
                    Console, wireless_settings),
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_CAMERA_SETTINGS,
                    Console, camera_settings),
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_PLAYTIMERDATA,
                    Console, play_timer_data),
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY,
                    XCONFIG_CONSOLE_MEDIA_DISABLEAUTOLAUNCH, Console,
                    media_disable_auto_launch),
      XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_KEYBOARD_LAYOUT,
                    Console, keyboard_layout),
      // XCONFIG_FIELD(XCONFIG_CONSOLE_CATEGORY,
      //              XCONFIG_CONSOLE_PC_TITLE_EXEMPTIONS,
      //              Console, keyboard_layout),

      // -- XCONFIG_SYSTEM (0xA) ---------------------------------------------
      XCONFIG_FIELD(XCONFIG_SYSTEM_CATEGORY, XCONFIG_SYSTEM_ALARM_TIME, System,
                    alarm_time),
      XCONFIG_FIELD(XCONFIG_SYSTEM_CATEGORY,
                    XCONFIG_SYSTEM_PREVIOUS_FLASH_VERSION, System,
                    previous_flash_version)};

#undef XCONFIG_FIELD

  xe_mutex lock_{};

  std::filesystem::path storage_path_;
  std::filesystem::path file_path_;
  XConfigData xconfig_data_{};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XCONFIG_H_
