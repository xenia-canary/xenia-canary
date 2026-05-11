/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GAME_SCAN_PROGRESS_DIALOG_WX_H_
#define XENIA_UI_GAME_SCAN_PROGRESS_DIALOG_WX_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "xenia/app/directory_scanner.h"

class wxBoxSizer;
class wxButton;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;

namespace xe {
namespace ui {

// Modal dialog that scans a user-chosen directory for launchable game files
// using app::DirectoryScanner. Read-only — discovered games are displayed but
// not written to any GPD / profile state.
class GameScanProgressDialog : public wxDialog {
 public:
  // Disc entry already registered on a profile (path + possibly user-edited
  // label).
  struct InstalledDisc {
    std::filesystem::path path;
    std::string label;
  };
  using InstalledTitleMap =
      std::unordered_map<uint32_t, std::vector<InstalledDisc>>;

  // One disc to import, paired with the label the writer should apply.
  struct PendingImport {
    app::DiscoveredGame game;
    std::string disc_label;
  };

  // Returns true to dismiss the dialog, false to keep it open for retry.
  // Receives one group per row, with only the not-already-installed sources.
  // If null, the Import button is hidden.
  using ImportCallback =
      std::function<bool(std::vector<std::vector<PendingImport>>)>;

  GameScanProgressDialog(wxWindow* parent,
                         const std::filesystem::path& root_directory,
                         InstalledTitleMap already_installed = {},
                         ImportCallback import_callback = {},
                         wxString import_disabled_reason = {});
  ~GameScanProgressDialog() override;

  // One title's row in the final view. `source_installed[i]` is true when
  // sources[i].path is already registered on a profile.
  struct FinalRow {
    std::vector<app::DiscoveredGame> sources;
    std::vector<bool> source_installed;
    std::string sort_key;
  };

 private:
  void Build();
  void Tick();
  void AppendFinalRow(FinalRow& row);
  void StartFinalization();
  void OnCancel();
  void OnImport();

  std::filesystem::path root_directory_;
  InstalledTitleMap already_installed_;
  std::unordered_set<uint32_t> already_installed_ids_;  // keys of above
  ImportCallback import_callback_;
  wxString import_disabled_reason_;
  std::unique_ptr<app::DirectoryScanner> scanner_;
  std::vector<FinalRow> pending_final_rows_;
  // Kept past the chunked render so OnImport still has data to forward.
  std::vector<FinalRow> all_rows_;
  size_t new_title_count_ = 0;
  size_t installed_title_count_ = 0;
  wxTimer timer_;

  wxStaticText* header_label_ = nullptr;
  wxStaticText* counters_label_ = nullptr;
  wxScrolledWindow* list_panel_ = nullptr;
  wxBoxSizer* list_sizer_ = nullptr;
  wxStaticText* placeholder_label_ = nullptr;
  wxButton* cancel_button_ = nullptr;
  wxButton* import_button_ = nullptr;
  wxButton* close_button_ = nullptr;

  bool was_done_ = false;
  bool cancel_pressed_ = false;
  bool finalization_started_ = false;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GAME_SCAN_PROGRESS_DIALOG_WX_H_
