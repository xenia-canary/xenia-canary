/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/game_library.h"

#include <algorithm>
#include <fstream>
#include <regex>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"

namespace xe {
namespace app {

// '/' separators avoid TOML escaping and round-trip through std::filesystem.
static std::string PathToToml(const std::filesystem::path& path) {
  std::string s = xe::path_to_utf8(path);
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

// Force exactly one default: keep the first flagged one, else flag the first.
static void NormalizeDefault(LibraryEntry& entry) {
  bool seen = false;
  for (auto& p : entry.paths) {
    if (p.is_default && !seen) {
      seen = true;
    } else {
      p.is_default = false;
    }
  }
  if (!seen && !entry.paths.empty()) {
    entry.paths.front().is_default = true;
  }
}

const LibraryPath& LibraryEntry::default_path() const {
  assert_false(paths.empty());
  for (const auto& p : paths) {
    if (p.is_default) {
      return p;
    }
  }
  return paths.front();
}

GameLibrary::GameLibrary(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path GameLibrary::EntryDir(uint32_t title_id) const {
  return root_ / fmt::format("{:08X}", title_id);
}

std::filesystem::path GameLibrary::IconPath(uint32_t title_id) const {
  return EntryDir(title_id) / "icon.png";
}

LibraryEntry* GameLibrary::Find(uint32_t title_id) {
  auto it = std::find_if(
      entries_.begin(), entries_.end(),
      [=](const LibraryEntry& e) { return e.title_id == title_id; });
  return it == entries_.end() ? nullptr : &*it;
}

const LibraryEntry* GameLibrary::Find(uint32_t title_id) const {
  return const_cast<GameLibrary*>(this)->Find(title_id);
}

void GameLibrary::Load() {
  entries_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(root_, ec)) {
    return;
  }

  const auto dirs = xe::filesystem::FilterByName(
      xe::filesystem::ListDirectories(root_), std::regex("[0-9A-Fa-f]{8}"));

  for (const auto& dir : dirs) {
    const auto game_toml = dir.path / dir.name / "game.toml";
    if (!std::filesystem::exists(game_toml, ec)) {
      continue;
    }

    toml::table tbl;
    try {
      tbl = toml::parse_file(xe::path_to_utf8(game_toml));
    } catch (const toml::parse_error& e) {
      XELOGE("GameLibrary: failed to parse {}: {}", xe::path_to_utf8(game_toml),
             e.description());
      continue;
    }

    auto title_id_str = tbl["title_id"].value<std::string>();
    if (!title_id_str) {
      XELOGW("GameLibrary: missing title_id, skipping {}",
             xe::path_to_utf8(game_toml));
      continue;
    }

    LibraryEntry entry;
    entry.title_id =
        xe::string_util::from_string<uint32_t>(*title_id_str, true);
    entry.name = tbl["name"].value_or(std::string());

    if (auto* paths = tbl["paths"].as_array()) {
      for (auto&& node : *paths) {
        auto* pt = node.as_table();
        if (!pt) {
          continue;
        }
        auto path_str = (*pt)["path"].value<std::string>();
        if (!path_str || path_str->empty()) {
          continue;
        }
        LibraryPath lp;
        lp.path = xe::to_path(*path_str);
        lp.label = (*pt)["label"].value_or(std::string());
        lp.is_default = (*pt)["default"].value_or(false);
        entry.paths.push_back(std::move(lp));
      }
    }

    if (entry.paths.empty()) {
      XELOGW("GameLibrary: no usable paths, skipping {}",
             xe::path_to_utf8(game_toml));
      continue;
    }

    NormalizeDefault(entry);
    entries_.push_back(std::move(entry));
  }
}

bool GameLibrary::WriteEntryAtomic(const LibraryEntry& entry) {
  const auto dir = EntryDir(entry.title_id);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot create {}: {}", xe::path_to_utf8(dir),
           ec.message());
    return false;
  }

  toml::array paths;
  for (const auto& p : entry.paths) {
    toml::table pt;
    pt.insert("label", p.label);
    pt.insert("path", PathToToml(p.path));
    if (p.is_default) {
      pt.insert("default", true);
    }
    pt.is_inline(true);
    paths.push_back(std::move(pt));
  }

  toml::table tbl;
  tbl.insert("title_id", fmt::format("{:08X}", entry.title_id));
  tbl.insert("name", entry.name);
  tbl.insert("paths", std::move(paths));

  const auto final_path = dir / "game.toml";
  const auto tmp_path = dir / "game.toml.tmp";
  {
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      XELOGE("GameLibrary: cannot open {}", xe::path_to_utf8(tmp_path));
      return false;
    }
    file << tbl << "\n";
    if (!file) {
      XELOGE("GameLibrary: write failed for {}", xe::path_to_utf8(tmp_path));
      return false;
    }
  }

  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot replace {}: {}", xe::path_to_utf8(final_path),
           ec.message());
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

bool GameLibrary::Upsert(LibraryEntry entry) {
  if (entry.paths.empty()) {
    XELOGW("GameLibrary: refusing to store {:08X} with no paths",
           entry.title_id);
    return false;
  }
  NormalizeDefault(entry);

  if (!WriteEntryAtomic(entry)) {
    return false;
  }

  if (auto* existing = Find(entry.title_id)) {
    *existing = std::move(entry);
  } else {
    entries_.push_back(std::move(entry));
  }
  return true;
}

bool GameLibrary::AddDisc(uint32_t title_id, const std::string& name,
                          const std::filesystem::path& path,
                          const std::string& label) {
  if (title_id == 0 || path.empty()) {
    return false;
  }

  LibraryEntry entry;
  if (auto* existing = Find(title_id)) {
    entry = *existing;
  } else {
    entry.title_id = title_id;
    entry.name = name;
  }

  const bool known =
      std::any_of(entry.paths.begin(), entry.paths.end(),
                  [&](const LibraryPath& p) { return p.path == path; });
  if (known) {
    return false;
  }

  entry.paths.push_back({path, label, false});
  return Upsert(std::move(entry));
}

bool GameLibrary::SetIcon(uint32_t title_id, std::span<const uint8_t> png) {
  const auto dir = EntryDir(title_id);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot create {}: {}", xe::path_to_utf8(dir),
           ec.message());
    return false;
  }

  const auto final_path = dir / "icon.png";
  const auto tmp_path = dir / "icon.png.tmp";
  {
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      XELOGE("GameLibrary: cannot open {}", xe::path_to_utf8(tmp_path));
      return false;
    }
    file.write(reinterpret_cast<const char*>(png.data()), png.size());
    if (!file) {
      XELOGE("GameLibrary: write failed for {}", xe::path_to_utf8(tmp_path));
      return false;
    }
  }

  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    XELOGE("GameLibrary: cannot replace {}: {}", xe::path_to_utf8(final_path),
           ec.message());
    std::filesystem::remove(tmp_path, ec);
    return false;
  }
  return true;
}

bool GameLibrary::Remove(uint32_t title_id) {
  std::error_code ec;
  std::filesystem::remove_all(EntryDir(title_id), ec);
  if (ec) {
    XELOGE("GameLibrary: cannot remove {:08X}: {}", title_id, ec.message());
    return false;
  }
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [=](const LibraryEntry& e) {
                                  return e.title_id == title_id;
                                }),
                 entries_.end());
  return true;
}

}  // namespace app
}  // namespace xe
