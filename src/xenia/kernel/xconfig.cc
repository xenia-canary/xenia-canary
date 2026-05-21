/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xconfig.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xenia/base/memory.h"
#include "xenia/config.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"

// XConfig settings are kept as cvars so they flow through Xenia's existing
// (per-game) config system. This class is only a guest-facing facade: reads
// translate cvars/constants into the guest XConfig byte format, and guest
// writes are persisted as per-game overrides.
DEFINE_int32(user_language, 1,
             "User language ID.\n"
             "  1=en  2=ja  3=de  4=fr  5=es  6=it  7=ko  8=zh\n"
             "  9=pt 11=pl 12=ru 13=sv 14=tr 15=nb 16=nl 17=zh",
             "Console");

DEFINE_int32(user_country, 103,
             "User country ID.\n"
             "   1=AE   2=AL   3=AM   4=AR   5=AT   6=AU   7=AZ   8=BE   9=BG\n"
             "  10=BH  11=BN  12=BO  13=BR  14=BY  15=BZ  16=CA  18=CH  19=CL\n"
             "  20=CN  21=CO  22=CR  23=CZ  24=DE  25=DK  26=DO  27=DZ  28=EC\n"
             "  29=EE  30=EG  31=ES  32=FI  33=FO  34=FR  35=GB  36=GE  37=GR\n"
             "  38=GT  39=HK  40=HN  41=HR  42=HU  43=ID  44=IE  45=IL  46=IN\n"
             "  47=IQ  48=IR  49=IS  50=IT  51=JM  52=JO  53=JP  54=KE  55=KG\n"
             "  56=KR  57=KW  58=KZ  59=LB  60=LI  61=LT  62=LU  63=LV  64=LY\n"
             "  65=MA  66=MC  67=MK  68=MN  69=MO  70=MV  71=MX  72=MY  73=NI\n"
             "  74=NL  75=NO  76=NZ  77=OM  78=PA  79=PE  80=PH  81=PK  82=PL\n"
             "  83=PR  84=PT  85=PY  86=QA  87=RO  88=RU  89=SA  90=SE  91=SG\n"
             "  92=SI  93=SK  95=SV  96=SY  97=TH  98=TN  99=TR 100=TT 101=TW\n"
             " 102=UA 103=US 104=UY 105=UZ 106=VE 107=VN 108=YE 109=ZA\n",
             "Console");

DEFINE_uint32(
    audio_flag, 0x00010001,
    "Audio Mode Analog.\n"
    " 0x00000001 = Dolby Pro Logic\n"
    " 0x00000002 = Analog Mono\n"
    "Audio Mode Digital.\n"
    " 0x00000000 = Digital Stereo (choose one of the above by itself)\n"
    " 0x00010000 = Dolby Digital\n"
    " 0x00030000 = Dolby Digital with WMA PRO\n"
    "Special Flags.\n"
    " 0x00000003 = Stereo Bypass\n"
    " 0x80000000 = Low Latency\n"
    " This Config requires you to pair an analog and digitial flag together\n"
    " while digital stereo only requires an analog flag. Bonus flags are\n"
    " optional. Ex) 0x00010001\n",
    "Console");

DEFINE_bool(allow_nui_initialization, false,
            "Enable NUI initialization\n"
            " Only set true when testing kinect games. Certain games may\n"
            " require avatar implementation.",
            "Console");

DEFINE_int32(xmp_default_volume, 70,
             "Default music volume if game doesn't set it [0-100].", "Console");

DECLARE_bool(widescreen);
DECLARE_int32(video_standard);
DECLARE_bool(use_50Hz_mode);
DECLARE_uint32(internal_display_resolution);

namespace xe {
namespace kernel {

namespace {

constexpr uint16_t kMaxSettingSize = 0x40;

// Fills |out| (>= kMaxSettingSize bytes) with the guest-formatted bytes of the
// requested setting and returns its size, or 0 if the setting is unhandled.
uint16_t BuildSetting(X_CONFIG_CATEGORY category, uint16_t setting,
                      uint8_t* out) {
  switch (category) {
    case XCONFIG_SECURED_CATEGORY:
      switch (setting) {
        case XCONFIG_SECURED_AV_REGION: {
          uint32_t region = 0;
          switch (cvars::video_standard) {
            case 1:  // NTSCM
              region = X_AV_REGION::NTSCM;
              break;
            case 2:  // NTSCJ
              region = X_AV_REGION::NTSCJ;
              break;
            case 3:  // PAL
              region =
                  cvars::use_50Hz_mode ? X_AV_REGION::PAL_50 : X_AV_REGION::PAL;
              break;
            default:
              break;
          }
          xe::store_and_swap<uint32_t>(out, region);
          return 4;
        }
      }
      break;
    case XCONFIG_USER_CATEGORY:
      switch (setting) {
        case XCONFIG_USER_TIME_ZONE_BIAS:
        case XCONFIG_USER_TIME_ZONE_STD_NAME:
        case XCONFIG_USER_TIME_ZONE_DLT_NAME:
        case XCONFIG_USER_TIME_ZONE_STD_DATE:
        case XCONFIG_USER_TIME_ZONE_DLT_DATE:
        case XCONFIG_USER_TIME_ZONE_STD_BIAS:
        case XCONFIG_USER_TIME_ZONE_DLT_BIAS:
          xe::store_and_swap<uint32_t>(out, 0);
          return 4;
        case XCONFIG_USER_LANGUAGE:
          xe::store_and_swap<uint32_t>(out, cvars::user_language);
          return 4;
        case XCONFIG_USER_VIDEO_FLAGS:
          xe::store_and_swap<uint32_t>(out, cvars::widescreen
                                                ? X_VIDEO_FLAGS::Widescreen
                                                : X_VIDEO_FLAGS::RatioNormal);
          return 4;
        case XCONFIG_USER_AUDIO_FLAGS:
          xe::store_and_swap<uint32_t>(out, cvars::audio_flag);
          return 4;
        case XCONFIG_USER_RETAIL_FLAGS:
          xe::store_and_swap<uint32_t>(
              out, X_RETAIL_FLAGS::DashboardInitialized |
                       (cvars::allow_nui_initialization
                            ? X_RETAIL_FLAGS::KinectInitialized
                            : 0));
          return 4;
        case XCONFIG_USER_COUNTRY:
          out[0] = static_cast<uint8_t>(cvars::user_country);
          return 1;
        case XCONFIG_USER_PC_FLAGS:
          // All related flags must be set for PC to function.
          out[0] =
              X_PC_FLAGS::XBLAllowed | X_PC_FLAGS::XBLMembershipCreationAllowed;
          return 1;
        case XCONFIG_USER_AV_COMPONENT_SCREENSZ: {
          const auto it =
              XHDTVResolution.find(cvars::internal_display_resolution);
          xe::store_and_swap<int32_t>(
              out, it != XHDTVResolution.cend() ? it->second : 0);
          return 4;
        }
        case XCONFIG_USER_AV_VGA_SCREENSZ: {
          const auto it =
              XVGAResolution.find(cvars::internal_display_resolution);
          xe::store_and_swap<int32_t>(
              out, it != XVGAResolution.cend() ? it->second : 0);
          return 4;
        }
        case XCONFIG_USER_PC_GAME:
          xe::store_and_swap<uint32_t>(out,
                                       X_PC_GAMES_FLAGS::NoGameRestrictions);
          return 4;
        case XCONFIG_USER_PC_PASSWORD:
          std::memset(out, 0, 4);
          return 4;
        case XCONFIG_USER_PC_MOVIE:
          xe::store_and_swap<uint32_t>(out,
                                       X_PC_MOVIE_FLAGS::NoMovieRestrictions);
          return 4;
        case XCONFIG_USER_PC_GAME_RATING:
          xe::store_and_swap<uint32_t>(out,
                                       X_PC_GAME_RATING_FLAGS::DefaultGame);
          return 4;
        case XCONFIG_USER_PC_MOVIE_RATING:
          xe::store_and_swap<uint32_t>(out,
                                       X_PC_MOVIE_RATING_FLAGS::DefaultMovie);
          return 4;
        case XCONFIG_USER_PC_HINT:
          std::memset(out, 0, 0x40);
          return 0x40;
        case XCONFIG_USER_PC_HINT_ANSWER:
          std::memset(out, 0, 0x20);
          return 0x20;
        case XCONFIG_USER_ARCADE_FLAGS:
          xe::store_and_swap<uint32_t>(out, X_ARCADE_FLAGS::AutoDownloadOff);
          return 4;
        case XCONFIG_USER_PC_VERSION:
          xe::store_and_swap<uint32_t>(out, X_PC_VERSION::VersionOne);
          return 4;
        case XCONFIG_USER_PC_TV:
          xe::store_and_swap<uint32_t>(out, X_PC_TV::NoTVRestrictions);
          return 4;
        case XCONFIG_USER_PC_TV_RATING:
          xe::store_and_swap<uint32_t>(out, X_PC_TV_RATING::DefaultTV);
          return 4;
        case XCONFIG_USER_PC_EXPLICIT_VIDEO:
          xe::store_and_swap<uint32_t>(
              out, X_PC_EXPLICIT_VIDEO::ExplicitVideoAllowed);
          return 4;
        case XCONFIG_USER_PC_EXPLICIT_VIDEO_RATING:
          xe::store_and_swap<uint32_t>(
              out, X_PC_EXPLICIT_VIDEO_RATING::ExplicitAllowed);
          return 4;
        case XCONFIG_USER_PC_UNRATED_VIDEO:
          xe::store_and_swap<uint32_t>(out, X_PC_EXPLICIT_UNRATED::UnratedALL);
          return 4;
        case XCONFIG_USER_PC_UNRATED_VIDEO_RATING:
          xe::store_and_swap<uint32_t>(
              out, X_PC_EXPLICIT_UNRATED_RATING::DefaultExplicitUnrated);
          return 4;
        case XCONFIG_USER_VIDEO_OUTPUT_BLACK_LEVELS:
          xe::store_and_swap<uint32_t>(out, X_BLACK_LEVEL::LevelNormal);
          return 4;
        case XCONFIG_USER_MUSIC_VOLUME: {
          const float volume = cvars::xmp_default_volume / 100.0f;
          xe::store_and_swap<float>(out, volume);
          return 4;
        }
      }
      break;
    case XCONFIG_CONSOLE_CATEGORY:
      switch (setting) {
        case XCONFIG_CONSOLE_SCREEN_SAVER:
          xe::store_and_swap<int16_t>(out, X_SCREENSAVER::ScreensaverOff);
          return 2;
        case XCONFIG_CONSOLE_AUTO_SHUT_OFF:
          xe::store_and_swap<int16_t>(out, X_AUTO_SHUTDOWN::AutoShutdownOff);
          return 2;
        case XCONFIG_CONSOLE_CAMERA_SETTINGS:
          xe::store_and_swap<uint32_t>(out, X_CAMERA_FLAGS::AutoAll);
          return 4;
        case XCONFIG_CONSOLE_KEYBOARD_LAYOUT:
          xe::store_and_swap<int16_t>(out, X_KEYBOARD_LAYOUT::KeyboardDefault);
          return 2;
      }
      break;
    default:
      break;
  }
  return 0;
}

// Updates a cvar's live value and persists it to the running title's per-game
// override.
template <typename T>
void StoreConsoleSetting(T& live, const char* name, T value) {
  live = value;
  config::SaveGameConfigSetting(kernel_state()->emulator(), "Console", name,
                                value);
}

}  // namespace

void XConfig::ReadSetting(const X_CONFIG_CATEGORY category,
                          const uint16_t setting_id, void* buffer) {
  uint8_t scratch[kMaxSettingSize] = {};
  const uint16_t size = BuildSetting(category, setting_id, scratch);
  if (size) {
    std::memcpy(buffer, scratch, size);
  }
}

uint16_t XConfig::GetSettingSize(const X_CONFIG_CATEGORY category,
                                 const uint16_t setting_id) {
  uint8_t scratch[kMaxSettingSize];
  return BuildSetting(category, setting_id, scratch);
}

void XConfig::WriteSetting(const X_CONFIG_CATEGORY category,
                           const uint16_t setting_id, const void* buffer) {
  switch (category) {
    case XCONFIG_SECURED_CATEGORY:
      switch (setting_id) {
        case XCONFIG_SECURED_AV_REGION: {
          const uint32_t region = xe::load_and_swap<uint32_t>(buffer);
          int32_t standard = cvars::video_standard;
          bool pal_50 = cvars::use_50Hz_mode;
          switch (region) {
            case X_AV_REGION::NTSCM:
              standard = 1;
              pal_50 = false;
              break;
            case X_AV_REGION::NTSCJ:
              standard = 2;
              pal_50 = false;
              break;
            case X_AV_REGION::PAL:
              standard = 3;
              pal_50 = false;
              break;
            case X_AV_REGION::PAL_50:
              standard = 3;
              pal_50 = true;
              break;
            default:
              return;
          }
          StoreConsoleSetting<int32_t>(cvars::video_standard, "video_standard",
                                       standard);
          StoreConsoleSetting<bool>(cvars::use_50Hz_mode, "use_50Hz_mode",
                                    pal_50);
          return;
        }
      }
      break;
    case XCONFIG_USER_CATEGORY:
      switch (setting_id) {
        case XCONFIG_USER_LANGUAGE:
          StoreConsoleSetting<int32_t>(
              cvars::user_language, "user_language",
              static_cast<int32_t>(xe::load_and_swap<uint32_t>(buffer)));
          return;
        case XCONFIG_USER_VIDEO_FLAGS:
          StoreConsoleSetting<bool>(cvars::widescreen, "widescreen",
                                    (xe::load_and_swap<uint32_t>(buffer) &
                                     X_VIDEO_FLAGS::Widescreen) != 0);
          return;
        case XCONFIG_USER_AUDIO_FLAGS:
          StoreConsoleSetting<uint32_t>(cvars::audio_flag, "audio_flag",
                                        xe::load_and_swap<uint32_t>(buffer));
          return;
        case XCONFIG_USER_COUNTRY:
          StoreConsoleSetting<int32_t>(
              cvars::user_country, "user_country",
              static_cast<int32_t>(*reinterpret_cast<const uint8_t*>(buffer)));
          return;
        case XCONFIG_USER_RETAIL_FLAGS:
          StoreConsoleSetting<bool>(cvars::allow_nui_initialization,
                                    "allow_nui_initialization",
                                    (xe::load_and_swap<uint32_t>(buffer) &
                                     X_RETAIL_FLAGS::KinectInitialized) != 0);
          return;
        case XCONFIG_USER_MUSIC_VOLUME: {
          const float volume = xe::load_and_swap<float>(buffer);
          StoreConsoleSetting<int32_t>(
              cvars::xmp_default_volume, "xmp_default_volume",
              std::clamp(static_cast<int32_t>(std::lround(volume * 100.0f)), 0,
                         100));
          return;
        }
      }
      break;
    default:
      break;
  }
}

}  // namespace kernel
}  // namespace xe
