/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_CONFIG_HELPERS_H_
#define XENIA_UI_CONFIG_HELPERS_H_

#include <map>
#include <string>
#include <vector>

#include "xenia/base/platform.h"

#include "xenia/xbox.h"

namespace xe {
namespace ui {

struct CvarAlias {
  std::string old_name;
  std::string old_value;
  std::string new_name;
  std::string new_value;
};

inline const std::vector<CvarAlias>& GetCvarAliases() {
  static const std::vector<CvarAlias> aliases = {
      {"use_new_decoder", "true", "xma_decoder", "new"},
      {"use_old_decoder", "true", "xma_decoder", "old"},
      {"readback_resolve", "true", "readback_resolve", "fast"},
      {"readback_resolve", "false", "readback_resolve", "fast"},
      {"render_target_path_d3d12", "rtv", "render_target_path", "performance"},
      {"render_target_path_d3d12", "rov", "render_target_path", "accuracy"},
      {"render_target_path_vulkan", "fbo", "render_target_path", "performance"},
      {"render_target_path_vulkan", "fsi", "render_target_path", "accuracy"},
      {"disable_doubleclick_fullscreen", "true", "disable_game_window_mouse",
       "true"},
      {"d3d12_submit_on_primary_buffer_end", "true",
       "submit_on_primary_buffer_end", "true"},
      {"d3d12_submit_on_primary_buffer_end", "false",
       "submit_on_primary_buffer_end", "false"},
      {"notification_sound_path", "*", "achievement_sound_path", "*"},
      {"vsync", "true", "guest_display_refresh_cap", "true"},
      {"vsync", "false", "guest_display_refresh_cap", "false"},
#if XE_PLATFORM_WIN32
      {"gpu", "any", "gpu", "d3d12"},
      {"apu", "any", "apu", "xaudio2"},
      {"hid", "any", "hid", "sdl"},
#elif XE_PLATFORM_LINUX
      {"gpu", "any", "gpu", "vulkan"},
      {"apu", "any", "apu", "alsa"},
      {"hid", "any", "hid", "sdl"},
#elif XE_PLATFORM_MAC
      {"gpu", "any", "gpu", "metal"},
      {"apu", "any", "apu", "sdl"},
      {"hid", "any", "hid", "sdl"},
#else
      {"gpu", "any", "gpu", "vulkan"},
      {"apu", "any", "apu", "sdl"},
      {"hid", "any", "hid", "sdl"},
#endif
  };
  return aliases;
}

// Ordered name<->id mappings for the user_language / user_country cvars.
// Shared between the config backwards-compat layer (which still accepts the
// legacy string values) and the UI option lists below.
struct CvarEnumOption {
  std::string name;
  int value;
};

inline const std::vector<CvarEnumOption>& GetUserLanguageOptions() {
  static const std::vector<CvarEnumOption> options = {
      {"English", static_cast<int>(XLanguage::kEnglish)},
      {"Japanese", static_cast<int>(XLanguage::kJapanese)},
      {"German", static_cast<int>(XLanguage::kGerman)},
      {"French", static_cast<int>(XLanguage::kFrench)},
      {"Spanish", static_cast<int>(XLanguage::kSpanish)},
      {"Italian", static_cast<int>(XLanguage::kItalian)},
      {"Korean", static_cast<int>(XLanguage::kKorean)},
      {"TChinese", static_cast<int>(XLanguage::kTChinese)},
      {"Portuguese", static_cast<int>(XLanguage::kPortuguese)},
      {"Polish", static_cast<int>(XLanguage::kPolish)},
      {"Russian", static_cast<int>(XLanguage::kRussian)},
      {"Swedish", static_cast<int>(XLanguage::kSwedish)},
      {"Turkish", static_cast<int>(XLanguage::kTurkish)},
      {"Norwegian", static_cast<int>(XLanguage::kNorwegian)},
      {"Dutch", static_cast<int>(XLanguage::kDutch)},
      {"SChinese", static_cast<int>(XLanguage::kSChinese)},
  };
  return options;
}

inline const std::vector<CvarEnumOption>& GetUserCountryOptions() {
  static const std::vector<CvarEnumOption> options = {
      {"UAE", static_cast<int>(XOnlineCountry::kUnitedArabEmirates)},
      {"Albania", static_cast<int>(XOnlineCountry::kAlbania)},
      {"Armenia", static_cast<int>(XOnlineCountry::kArmenia)},
      {"Argentina", static_cast<int>(XOnlineCountry::kArgentina)},
      {"Austria", static_cast<int>(XOnlineCountry::kAustria)},
      {"Australia", static_cast<int>(XOnlineCountry::kAustralia)},
      {"Azerbaijan", static_cast<int>(XOnlineCountry::kAzerbaijan)},
      {"Belgium", static_cast<int>(XOnlineCountry::kBelgium)},
      {"Bulgaria", static_cast<int>(XOnlineCountry::kBulgaria)},
      {"Bahrain", static_cast<int>(XOnlineCountry::kBahrain)},
      {"Brunei", static_cast<int>(XOnlineCountry::kBruneiDarussalam)},
      {"Bolivia", static_cast<int>(XOnlineCountry::kBolivia)},
      {"Brazil", static_cast<int>(XOnlineCountry::kBrazil)},
      {"Belarus", static_cast<int>(XOnlineCountry::kBelarus)},
      {"Belize", static_cast<int>(XOnlineCountry::kBelize)},
      {"Canada", static_cast<int>(XOnlineCountry::kCanada)},
      {"Switzerland", static_cast<int>(XOnlineCountry::kSwitzerland)},
      {"Chile", static_cast<int>(XOnlineCountry::kChile)},
      {"China", static_cast<int>(XOnlineCountry::kChina)},
      {"Colombia", static_cast<int>(XOnlineCountry::kColombia)},
      {"Costa Rica", static_cast<int>(XOnlineCountry::kCostaRica)},
      {"Czech Republic", static_cast<int>(XOnlineCountry::kCzechRepublic)},
      {"Germany", static_cast<int>(XOnlineCountry::kGermany)},
      {"Denmark", static_cast<int>(XOnlineCountry::kDenmark)},
      {"Dominican Republic",
       static_cast<int>(XOnlineCountry::kDominicanRepublic)},
      {"Algeria", static_cast<int>(XOnlineCountry::kAlgeria)},
      {"Ecuador", static_cast<int>(XOnlineCountry::kEcuador)},
      {"Estonia", static_cast<int>(XOnlineCountry::kEstonia)},
      {"Egypt", static_cast<int>(XOnlineCountry::kEgypt)},
      {"Spain", static_cast<int>(XOnlineCountry::kSpain)},
      {"Finland", static_cast<int>(XOnlineCountry::kFinland)},
      {"Faroe Islands", static_cast<int>(XOnlineCountry::kFaroeIslands)},
      {"France", static_cast<int>(XOnlineCountry::kFrance)},
      {"Great Britain", static_cast<int>(XOnlineCountry::kGreatBritain)},
      {"Georgia", static_cast<int>(XOnlineCountry::kGeorgia)},
      {"Greece", static_cast<int>(XOnlineCountry::kGreece)},
      {"Guatemala", static_cast<int>(XOnlineCountry::kGuatemala)},
      {"Hong Kong", static_cast<int>(XOnlineCountry::kHongKong)},
      {"Honduras", static_cast<int>(XOnlineCountry::kHonduras)},
      {"Croatia", static_cast<int>(XOnlineCountry::kCroatia)},
      {"Hungary", static_cast<int>(XOnlineCountry::kHungary)},
      {"Indonesia", static_cast<int>(XOnlineCountry::kIndonesia)},
      {"Ireland", static_cast<int>(XOnlineCountry::kIreland)},
      {"Israel", static_cast<int>(XOnlineCountry::kIsrael)},
      {"India", static_cast<int>(XOnlineCountry::kIndia)},
      {"Iraq", static_cast<int>(XOnlineCountry::kIraq)},
      {"Iran", static_cast<int>(XOnlineCountry::kIran)},
      {"Iceland", static_cast<int>(XOnlineCountry::kIceland)},
      {"Italy", static_cast<int>(XOnlineCountry::kItaly)},
      {"Jamaica", static_cast<int>(XOnlineCountry::kJamaica)},
      {"Jordan", static_cast<int>(XOnlineCountry::kJordan)},
      {"Japan", static_cast<int>(XOnlineCountry::kJapan)},
      {"Kenya", static_cast<int>(XOnlineCountry::kKenya)},
      {"Kyrgyzstan", static_cast<int>(XOnlineCountry::kKyrgyzstan)},
      {"Korea", static_cast<int>(XOnlineCountry::kKorea)},
      {"Kuwait", static_cast<int>(XOnlineCountry::kKuwait)},
      {"Kazakhstan", static_cast<int>(XOnlineCountry::kKazakhstan)},
      {"Lebanon", static_cast<int>(XOnlineCountry::kLebanon)},
      {"Liechtenstein", static_cast<int>(XOnlineCountry::kLiechtenstein)},
      {"Lithuania", static_cast<int>(XOnlineCountry::kLithuania)},
      {"Luxembourg", static_cast<int>(XOnlineCountry::kLuxembourg)},
      {"Latvia", static_cast<int>(XOnlineCountry::kLatvia)},
      {"Libya", static_cast<int>(XOnlineCountry::kLibya)},
      {"Morocco", static_cast<int>(XOnlineCountry::kMorocco)},
      {"Monaco", static_cast<int>(XOnlineCountry::kMonaco)},
      {"Macedonia", static_cast<int>(XOnlineCountry::kMacedonia)},
      {"Mongolia", static_cast<int>(XOnlineCountry::kMongolia)},
      {"Macau", static_cast<int>(XOnlineCountry::kMacau)},
      {"Maldives", static_cast<int>(XOnlineCountry::kMaldives)},
      {"Mexico", static_cast<int>(XOnlineCountry::kMexico)},
      {"Malaysia", static_cast<int>(XOnlineCountry::kMalaysia)},
      {"Nicaragua", static_cast<int>(XOnlineCountry::kNicaragua)},
      {"Netherlands", static_cast<int>(XOnlineCountry::kNetherlands)},
      {"Norway", static_cast<int>(XOnlineCountry::kNorway)},
      {"New Zealand", static_cast<int>(XOnlineCountry::kNewZealand)},
      {"Oman", static_cast<int>(XOnlineCountry::kOman)},
      {"Panama", static_cast<int>(XOnlineCountry::kPanama)},
      {"Peru", static_cast<int>(XOnlineCountry::kPeru)},
      {"Philippines", static_cast<int>(XOnlineCountry::kPhilippines)},
      {"Pakistan", static_cast<int>(XOnlineCountry::kPakistan)},
      {"Poland", static_cast<int>(XOnlineCountry::kPoland)},
      {"Puerto Rico", static_cast<int>(XOnlineCountry::kPuertoRico)},
      {"Portugal", static_cast<int>(XOnlineCountry::kPortugal)},
      {"Paraguay", static_cast<int>(XOnlineCountry::kParaguay)},
      {"Qatar", static_cast<int>(XOnlineCountry::kQatar)},
      {"Romania", static_cast<int>(XOnlineCountry::kRomania)},
      {"Russia", static_cast<int>(XOnlineCountry::kRussianFederation)},
      {"Saudi Arabia", static_cast<int>(XOnlineCountry::kSaudiArabia)},
      {"Sweden", static_cast<int>(XOnlineCountry::kSweden)},
      {"Singapore", static_cast<int>(XOnlineCountry::kSingapore)},
      {"Slovenia", static_cast<int>(XOnlineCountry::kSlovenia)},
      {"Slovakia", static_cast<int>(XOnlineCountry::kSlovakRepublic)},
      {"El Salvador", static_cast<int>(XOnlineCountry::kElSalvador)},
      {"Syria", static_cast<int>(XOnlineCountry::kSyria)},
      {"Thailand", static_cast<int>(XOnlineCountry::kThailand)},
      {"Tunisia", static_cast<int>(XOnlineCountry::kTunisia)},
      {"Turkey", static_cast<int>(XOnlineCountry::kTurkey)},
      {"Trinidad", static_cast<int>(XOnlineCountry::kTrinidadAndTobago)},
      {"Taiwan", static_cast<int>(XOnlineCountry::kTaiwan)},
      {"Ukraine", static_cast<int>(XOnlineCountry::kUkraine)},
      {"United States", static_cast<int>(XOnlineCountry::kUnitedStates)},
      {"Uruguay", static_cast<int>(XOnlineCountry::kUruguay)},
      {"Uzbekistan", static_cast<int>(XOnlineCountry::kUzbekistan)},
      {"Venezuela", static_cast<int>(XOnlineCountry::kVenezuela)},
      {"Vietnam", static_cast<int>(XOnlineCountry::kVietNam)},
      {"Yemen", static_cast<int>(XOnlineCountry::kYemen)},
      {"South Africa", static_cast<int>(XOnlineCountry::kSouthAfrica)},
      {"Zimbabwe", static_cast<int>(XOnlineCountry::kZimbabwe)},
  };
  return options;
}

inline const std::vector<CvarEnumOption>& GetVideoStandardOptions() {
  static const std::vector<CvarEnumOption> options = {
      {"NTSC", 1},
      {"NTSC-J", 2},
      {"PAL", 3},
  };
  return options;
}

inline const std::vector<CvarEnumOption>&
GetInternalDisplayResolutionOptions() {
  static const std::vector<CvarEnumOption> options = {
      {"640x480", 0},    {"640x576", 1},   {"720x480", 2},    {"720x576", 3},
      {"800x600", 4},    {"848x480", 5},   {"1024x768", 6},   {"1152x864", 7},
      {"1280x720", 8},   {"1280x768", 9},  {"1280x960", 10},  {"1280x1024", 11},
      {"1360x768", 12},  {"1440x900", 13}, {"1680x1050", 14}, {"1920x540", 15},
      {"1920x1080", 16}, {"Custom", 17},
  };
  return options;
}

// Returns the name<->id option list for an integer cvar that is presented as a
// string dropdown in the UI, or nullptr if the cvar isn't one of them.
inline const std::vector<CvarEnumOption>* FindIntCvarEnumOptions(
    const std::string& name) {
  if (name == "user_language") {
    return &GetUserLanguageOptions();
  }
  if (name == "user_country") {
    return &GetUserCountryOptions();
  }
  if (name == "video_standard") {
    return &GetVideoStandardOptions();
  }
  if (name == "internal_display_resolution") {
    return &GetInternalDisplayResolutionOptions();
  }
  return nullptr;
}

// Maps a stored integer cvar value (as a string) to its UI dropdown display
// name. Returns |value| unchanged for non-int-enum cvars or on no match.
inline std::string IntCvarValueToDisplayName(const std::string& name,
                                             const std::string& value) {
  const auto* options = FindIntCvarEnumOptions(name);
  if (!options) {
    return value;
  }
  try {
    const int id = std::stoi(value);
    for (const auto& option : *options) {
      if (option.value == id) {
        return option.name;
      }
    }
  } catch (...) {
  }
  return value;
}

// Maps a UI dropdown display name back to the integer cvar value (as a string).
// Returns |display_name| unchanged for non-int-enum cvars or on no match.
inline std::string DisplayNameToIntCvarValue(const std::string& name,
                                             const std::string& display_name) {
  const auto* options = FindIntCvarEnumOptions(name);
  if (!options) {
    return display_name;
  }
  for (const auto& option : *options) {
    if (option.name == display_name) {
      return std::to_string(option.value);
    }
  }
  return display_name;
}

// Known enum-like cvars with their valid options
inline const std::map<std::string, std::vector<std::string>>&
GetKnownEnumOptions() {
  static const std::map<std::string, std::vector<std::string>> options = []() {
    std::map<std::string, std::vector<std::string>> m = {
#if XE_PLATFORM_WIN32
        {"gpu", {"d3d12", "vulkan", "null"}},
        {"apu", {"xaudio2", "sdl", "nop"}},
        {"hid", {"sdl", "winkey", "xinput", "nop"}},
#elif XE_PLATFORM_LINUX
        {"gpu", {"vulkan", "null"}},
        {"apu", {"alsa", "sdl", "nop"}},
        {"hid", {"sdl", "nop"}},
#elif XE_PLATFORM_MAC
        {"gpu", {"metal", "vulkan", "null"}},
        {"apu", {"nop", "sdl"}},
        {"hid", {"sdl", "nop"}},
#else
        {"gpu", {"vulkan", "null"}},
        {"apu", {"nop", "sdl"}},
        {"hid", {"sdl", "nop"}},
#endif
        {"d3d12_readback_resolve",
         {"kCopy", "kComputeLuminance", "kComputeRGBA16"}},
        {"occlusion_query", {"fake", "fast", "strict"}},
        {"readback_resolve", {"fast", "some", "full", "none"}},
        {"render_target_path", {"performance", "accuracy"}},
        {"postprocess_antialiasing", {"off", "fxaa", "fxaa_extreme"}},
        {"postprocess_scaling_and_sharpening", {"none", "cas", "fsr"}},
        {"spirv_version_override", {"auto", "1.0", "1.3", "1.4", "1.5", "1.6"}},
        {"xma_decoder", {"old", "new", "master", "fake"}},
    };
    const auto names = [](const std::vector<CvarEnumOption>& opts) {
      std::vector<std::string> v;
      v.reserve(opts.size());
      for (const auto& opt : opts) {
        v.push_back(opt.name);
      }
      return v;
    };
    for (const char* name : {"user_language", "user_country", "video_standard",
                             "internal_display_resolution"}) {
      m.emplace(name, names(*FindIntCvarEnumOptions(name)));
    }
    return m;
  }();
  return options;
}

enum class CvarPathKind {
  kDirectory,
  kFileOpen,
  kFileSave,
};

struct CvarPathInfo {
  CvarPathKind kind = CvarPathKind::kDirectory;
  // Pipe-delimited wxFileDialog wildcard
  // ("Audio (*.wav)|*.wav|All (*.*)|*.*"). Empty for kDirectory.
  std::string wildcard;
};

inline const CvarPathInfo* GetCvarPathInfo(const std::string& name) {
  static const std::map<std::string, CvarPathInfo> kInfo = {
      {"achievement_sound_path",
       {CvarPathKind::kFileOpen,
        "Audio Files (*.wav;*.mp3;*.ogg;*.flac;*.m4a;*.aac;*.wma;*.opus)"
        "|*.wav;*.mp3;*.ogg;*.flac;*.m4a;*.aac;*.wma;*.opus"
        "|All Files (*.*)|*.*"}},
      {"custom_font_path",
       {CvarPathKind::kFileOpen,
        "Font Files (*.ttf;*.otf;*.ttc)|*.ttf;*.otf;*.ttc"
        "|All Files (*.*)|*.*"}},
      {"mappings_file",
       {CvarPathKind::kFileOpen,
        "Text Files (*.txt)|*.txt|All Files (*.*)|*.*"}},
      {"log_file",
       {CvarPathKind::kFileSave,
        "Log Files (*.log;*.txt)|*.log;*.txt|All Files (*.*)|*.*"}},
  };
  auto it = kInfo.find(name);
  return it == kInfo.end() ? nullptr : &it->second;
}

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_CONFIG_HELPERS_H_
