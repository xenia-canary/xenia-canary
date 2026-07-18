/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/locale.h"

#include <algorithm>
#include <unordered_map>

#include "third_party/tomlplusplus/include/toml++/toml.hpp"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"

DEFINE_string(
    language, "en",
    "UI language. \"en\" (default) uses the built-in English strings as-is. "
    "Any other value (e.g. \"it\") loads translations from "
    "locale/<language>.toml next to the executable, falling back to "
    "English for any string the file doesn't cover.",
    "General");

namespace xe {
namespace locale {

namespace {
std::unordered_map<std::string, std::string> translations_;
std::string active_language_ = "en";
// The language that was active when Initialize() ran at startup - i.e. the
// language the native menu bar (built once, early in startup) actually
// reflects. Deliberately never updated by SwitchLanguage(), so UI code can
// compare it against current_language() to know whether a restart is
// actually needed to fully apply the user's choice.
std::string startup_language_ = "en";

// Loads (or clears, for "en") the translation table for `language_code`.
// Shared by Initialize() (startup) and SwitchLanguage() (runtime change from
// the settings UI).
void LoadLanguage(std::string_view language_code) {
  translations_.clear();
  active_language_ = std::string(language_code);

  if (active_language_.empty() || active_language_ == "en") {
    // Nothing to load - English strings are used as their own translation.
    return;
  }

  auto locale_path = xe::filesystem::GetExecutableFolder() / "locale" /
                     (active_language_ + ".toml");
  if (!std::filesystem::exists(locale_path)) {
    XELOGW(
        "locale: no translation file found for language \"{}\" (looked "
        "for {}), falling back to English.",
        active_language_, xe::path_to_utf8(locale_path));
    return;
  }

  toml::parse_result result;
  try {
    result = toml::parse_file(xe::path_to_utf8(locale_path));
  } catch (const toml::parse_error& error) {
    XELOGE("locale: failed to parse {}: {}", xe::path_to_utf8(locale_path),
           error.description());
    return;
  }

  size_t loaded = 0;
  for (auto&& [key, value] : result) {
    auto translated = value.value<std::string>();
    if (translated) {
      translations_.emplace(std::string(key.str()), *translated);
      ++loaded;
    }
  }

  XELOGI("locale: loaded {} translated string(s) for language \"{}\" from {}",
         loaded, active_language_, xe::path_to_utf8(locale_path));
}

// Built-in display names, in each language's own script, for locales Xenia
// ships a translation for. Codes without an entry here simply show as their
// uppercased code (e.g. "xx") until someone adds a proper name - this list
// is meant to grow alongside contributed translation files.
const std::unordered_map<std::string, std::string>& LanguageNames() {
  static const std::unordered_map<std::string, std::string> kNames = {
      {"en", "English"},
      {"it", "Italiano"},
  };
  return kNames;
}

}  // namespace

void Initialize() {
  LoadLanguage(cvars::language);
  startup_language_ = active_language_;
}

std::string Translate(std::string_view source) {
  if (!translations_.empty()) {
    auto it = translations_.find(std::string(source));
    if (it != translations_.end() && !it->second.empty()) {
      return it->second;
    }
  }
  // Fallback: no translation loaded, or none defined for this string -
  // just use the English source as-is.
  return std::string(source);
}

const std::string& current_language() { return active_language_; }

bool RestartRequired() { return active_language_ != startup_language_; }

std::vector<std::string> AvailableLanguages() {
  // Scanning the disk and logging on every call would mean doing it every
  // single UI frame the language dropdown is open (60+ times a second) -
  // wasteful, and it floods xenia.log. The set of translation files present
  // next to the executable isn't expected to change while Xenia is running,
  // so scan once and reuse the result.
  static const std::vector<std::string> kCachedLanguages = [] {
    std::vector<std::string> languages = {"en"};

    auto locale_dir = xe::filesystem::GetExecutableFolder() / "locale";
    if (!std::filesystem::exists(locale_dir)) {
      XELOGI(
          "locale: no \"locale\" folder found next to the executable "
          "(looked for {}) - only English will be offered.",
          xe::path_to_utf8(locale_dir));
      return languages;
    }

    for (const auto& file : xe::filesystem::ListFiles(locale_dir)) {
      if (file.type != xe::filesystem::FileInfo::Type::kFile) {
        continue;
      }
      if (xe::utf8::equal_case(xe::path_to_utf8(file.name.extension()),
                               ".toml")) {
        std::string code = xe::path_to_utf8(file.name.stem());
        if (code != "en" && std::find(languages.begin(), languages.end(),
                                      code) == languages.end()) {
          languages.push_back(code);
        }
      }
    }

    XELOGI("locale: found {} language(s) in {} (plus built-in English).",
           languages.size() - 1, xe::path_to_utf8(locale_dir));
    return languages;
  }();

  return kCachedLanguages;
}

std::string DisplayName(std::string_view language_code) {
  const auto& names = LanguageNames();
  auto it = names.find(std::string(language_code));
  if (it != names.end()) {
    return it->second;
  }
  std::string upper(language_code);
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
  return upper;
}

void SwitchLanguage(std::string_view language_code) {
  LoadLanguage(language_code);
  OVERRIDE_string(language, active_language_);
}

}  // namespace locale
}  // namespace xe
