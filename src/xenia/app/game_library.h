/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GAME_LIBRARY_H_
#define XENIA_APP_GAME_LIBRARY_H_

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace xe {
namespace app {

// One launchable location for a title (a disc, or an alternate copy).
struct LibraryPath {
  std::filesystem::path path;
  std::string label;
  bool is_default = false;
};

// A single installed title. Persisted at <root>/<title_id>/game.toml, with its
// icon alongside at <root>/<title_id>/icon.png.
struct LibraryEntry {
  uint32_t title_id = 0;
  std::string name;
  std::vector<LibraryPath> paths;  // always >= 1; exactly one is default

  // Path launched on double-click: the flagged default, else the first.
  const LibraryPath& default_path() const;
};

// Catalog of installed games, sharded one folder per title under
// storage_root/library, so updates touch only that title.
class GameLibrary {
 public:
  explicit GameLibrary(std::filesystem::path root);

  // Scans the library root and (re)loads every title's metadata.
  void Load();

  const std::vector<LibraryEntry>& entries() const { return entries_; }

  LibraryEntry* Find(uint32_t title_id);
  const LibraryEntry* Find(uint32_t title_id) const;

  // Inserts or replaces a title and persists just that title's game.toml.
  // Normalizes the default-path marker before writing.
  bool Upsert(LibraryEntry entry);

  // Records `path` as a disc of `title_id` (creating it with `name` if absent),
  // deduped, and persists. False on a dup, title_id 0, or empty path.
  bool AddDisc(uint32_t title_id, const std::string& name,
               const std::filesystem::path& path, const std::string& label);

  // Writes <root>/<title_id>/icon.png.
  bool SetIcon(uint32_t title_id, std::span<const uint8_t> png);
  std::filesystem::path IconPath(uint32_t title_id) const;

  // Removes a title's folder and drops it from the in-memory list.
  bool Remove(uint32_t title_id);

 private:
  std::filesystem::path EntryDir(uint32_t title_id) const;
  bool WriteEntryAtomic(const LibraryEntry& entry);

  std::filesystem::path root_;
  std::vector<LibraryEntry> entries_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GAME_LIBRARY_H_
