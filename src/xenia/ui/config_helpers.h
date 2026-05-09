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

// Known enum-like cvars with their valid options
inline const std::map<std::string, std::vector<std::string>>&
GetKnownEnumOptions() {
  static const std::map<std::string, std::vector<std::string>> options = {
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
      {"user_language",
       {"English", "Japanese", "German", "French", "Spanish", "Italian",
        "Korean", "TChinese", "Portuguese", "SChinese", "Polish", "Russian"}},
      {"user_country",
       {"UAE",
        "Albania",
        "Armenia",
        "Argentina",
        "Austria",
        "Australia",
        "Azerbaijan",
        "Belgium",
        "Bulgaria",
        "Bahrain",
        "Brunei",
        "Bolivia",
        "Brazil",
        "Belarus",
        "Belize",
        "Canada",
        "Switzerland",
        "Chile",
        "China",
        "Colombia",
        "Costa Rica",
        "Czech Republic",
        "Germany",
        "Denmark",
        "Dominican Republic",
        "Algeria",
        "Ecuador",
        "Estonia",
        "Egypt",
        "Spain",
        "Finland",
        "Faroe Islands",
        "France",
        "Great Britain",
        "Georgia",
        "Greece",
        "Guatemala",
        "Hong Kong",
        "Honduras",
        "Croatia",
        "Hungary",
        "Indonesia",
        "Ireland",
        "Israel",
        "India",
        "Iraq",
        "Iran",
        "Iceland",
        "Italy",
        "Jamaica",
        "Jordan",
        "Japan",
        "Kenya",
        "Kyrgyzstan",
        "Korea",
        "Kuwait",
        "Kazakhstan",
        "Lebanon",
        "Liechtenstein",
        "Lithuania",
        "Luxembourg",
        "Latvia",
        "Libya",
        "Morocco",
        "Monaco",
        "Macedonia",
        "Mongolia",
        "Macau",
        "Maldives",
        "Mexico",
        "Malaysia",
        "Nicaragua",
        "Netherlands",
        "Norway",
        "New Zealand",
        "Oman",
        "Panama",
        "Peru",
        "Philippines",
        "Pakistan",
        "Poland",
        "Puerto Rico",
        "Portugal",
        "Paraguay",
        "Qatar",
        "Romania",
        "Russia",
        "Saudi Arabia",
        "Sweden",
        "Singapore",
        "Slovenia",
        "Slovakia",
        "El Salvador",
        "Syria",
        "Thailand",
        "Tunisia",
        "Turkey",
        "Trinidad",
        "Taiwan",
        "Ukraine",
        "United States",
        "Uruguay",
        "Uzbekistan",
        "Venezuela",
        "Vietnam",
        "Yemen",
        "South Africa",
        "Zimbabwe"}},
  };
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
