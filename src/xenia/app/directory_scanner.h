/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_DIRECTORY_SCANNER_H_
#define XENIA_APP_DIRECTORY_SCANNER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "xenia/xbox.h"

namespace xe {
namespace app {

// Lightweight summary of one launchable file found by DirectoryScanner.
struct DiscoveredGame {
  std::filesystem::path path;
  std::string format;        // "xex" | "iso" | "zar" | "stfs"
  uint32_t title_id = 0;     // raw value (title_id_hex is the formatted form)
  std::string title_id_hex;  // 8 uppercase hex digits
  uint32_t media_id = 0;     // identifies the specific disc / content blob

  // XEX/ISO/ZAR
  std::string module_name;
  std::string version;  // "1.0.0.0" style

  // STFS containers
  std::string display_name;
  std::string title_name;
  std::string publisher;
  uint32_t content_type = 0;      // raw XContentType
  std::string content_type_name;  // human-readable, e.g. "Arcade Title"

  // Both — multi-disc games
  uint8_t disc_number = 0;
  uint8_t disc_count = 0;

  // Title icon. icon_rgba is decoded for the dialog's fast wxImage wrap;
  // icon_png keeps the source bytes for writing into the per-title GPD on
  // import. Empty for XEX/ISO/ZAR.
  int icon_width = 0;
  int icon_height = 0;
  std::vector<uint8_t> icon_rgba;
  std::vector<uint8_t> icon_png;
};

// title_name (clean STFS header field) → display_name (often disc-suffixed)
// → module_name (XEX/ISO/ZAR). May be empty if none are set.
inline const std::string& PreferredName(const DiscoveredGame& g) {
  if (!g.title_name.empty()) {
    return g.title_name;
  }
  if (!g.display_name.empty()) {
    return g.display_name;
  }
  return g.module_name;
}

// Recursively scans a directory for launchable game files. Only default.xex
// is considered a XEX; other .xex files are skipped (modules/companions). A
// default.xex hit terminates the current directory. ISO/ZAR/STFS hits are
// collected as multi-game candidates without stopping. Recursion happens
// only when no default.xex was found. See ScanDirectory() for full algorithm.
class DirectoryScanner {
 public:
  struct Progress {
    std::filesystem::path current_directory;
    size_t directories_scanned = 0;
    size_t files_probed = 0;
    size_t games_found = 0;
    bool started = false;
    bool done = false;
    bool cancelled = false;
  };

  DirectoryScanner();
  ~DirectoryScanner();

  DirectoryScanner(const DirectoryScanner&) = delete;
  DirectoryScanner& operator=(const DirectoryScanner&) = delete;

  // Spawn the worker threads. Second call is a no-op. `language` picks the
  // localized STFS slot (falls back to English when the slot is empty).
  void Start(std::filesystem::path root,
             XLanguage language = XLanguage::kEnglish);

  // Ask the worker to exit at the next opportunity. Safe to call any time.
  void Cancel();

  Progress GetProgress() const;

  // Full snapshot of everything found so far.
  std::vector<DiscoveredGame> GetAllResults() const;

  // Count of results matching any title_id in the set. Lock-only, no copy.
  size_t CountResultsWithTitleId(
      const std::unordered_set<uint32_t>& title_ids) const;

 private:
  void Run(std::filesystem::path root);
  void WorkerLoop();
  void ScanDirectory(const std::filesystem::path& dir);
  void EnqueueSubdirs(std::vector<std::filesystem::path> subdirs);
  void RecordGame(DiscoveredGame game);
  void SetCurrentDirectory(const std::filesystem::path& dir);

  mutable std::mutex mu_;
  Progress progress_;
  std::vector<DiscoveredGame> results_;

  // queue_mu_ protects pending_ and inflight_.
  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::deque<std::filesystem::path> pending_;
  size_t inflight_ = 0;

  std::atomic<bool> cancel_requested_{false};
  std::thread orchestrator_;
  std::vector<std::thread> workers_;
  bool started_ = false;
  XLanguage language_ = XLanguage::kEnglish;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_DIRECTORY_SCANNER_H_
