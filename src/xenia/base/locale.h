/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_LOCALE_H_
#define XENIA_BASE_LOCALE_H_

#include <string>
#include <string_view>
#include <vector>

namespace xe {
namespace locale {

// Loads the translation table for the language requested via the
// "language" cvar (e.g. "it"). Looks for
// <executable_folder>/locale/<language>.toml.
//
// If the cvar is "en" (the default), or the requested locale file cannot be
// found/parsed, this is a no-op and Translate() will simply return the
// English source strings unchanged. This keeps the feature entirely
// opt-in and impossible to break existing behavior for anyone who doesn't
// pick a language.
void Initialize();

// Returns the translated string matching `source`, if the currently loaded
// locale defines one, otherwise returns `source` itself untouched.
//
// `source` is always the original English UI string, which doubles as the
// lookup key - this mirrors the classic gettext `_()` convention, so the
// English text keeps working as a sensible fallback even for languages
// that have no (or an incomplete) translation file yet.
std::string Translate(std::string_view source);

// Returns the language code currently active (e.g. "en", "it").
const std::string& current_language();

// Returns true if the current language differs from the one the app was
// started with (or last restarted into) - i.e. the native menu bar is out of
// date and a restart is actually needed to fully apply the change. Dialogs
// are unaffected either way; they always reflect current_language().
bool RestartRequired();

// Scans <executable_folder>/locale/*.toml and returns the language codes
// found there, always including "en" first (the built-in English strings,
// which needs no file on disk).
std::vector<std::string> AvailableLanguages();

// Returns a human-readable name for a language code, in that language's own
// script (e.g. "it" -> "Italiano"), so it reads correctly even to someone who
// doesn't yet know the current UI language. Falls back to the code itself
// (uppercased) for languages not in the built-in name table - this never
// blocks a translation file from being usable, it just shows a less pretty
// label until a name is added here.
std::string DisplayName(std::string_view language_code);

// Switches the active language immediately (reloading its locale file, or
// clearing translations for "en") without needing to relaunch Xenia. Also
// updates the "language" cvar so the choice is saved to xenia.config.toml
// on exit. Menu bar text built once at startup (native OS menu items) will
// only reflect the new language after a restart; ImGui-drawn dialogs update
// on the next frame.
void SwitchLanguage(std::string_view language_code);

}  // namespace locale
}  // namespace xe

// Shorthand for xe::locale::Translate, used at UI call sites so translatable
// strings are easy to grep for and visually distinct from strings that are
// intentionally left untranslated (hotkeys, format specifiers, ImGui ID
// suffixes, file filters, log/debug messages, CVar names, etc).
#define XE_LOCALIZE(str) ::xe::locale::Translate(str)

#endif  // XENIA_BASE_LOCALE_H_
