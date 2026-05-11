/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_scan_progress_dialog_wx.h"

#include <algorithm>
#include <unordered_map>

#include "third_party/fmt/include/fmt/format.h"

#include <wx/bmpbndl.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include "xenia/base/filesystem.h"
#include "xenia/ui/icon_decode.h"
#include "xenia/ui/wx_locale.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {

namespace {

constexpr int kTimerMs = 100;
constexpr int kIconDip = 64;

std::string ToLowerAsciiCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

wxString FormatHeader(const std::filesystem::path& root) {
  return wxString::Format(_("Scanning: %s"),
                          wxString::FromUTF8(xe::path_to_utf8(root)));
}

wxString FormatCounters(const app::DirectoryScanner::Progress& p,
                        size_t already_installed) {
  wxString base = wxString::Format(
      _("%zu directories scanned   %zu files probed   %zu games found"),
      p.directories_scanned, p.files_probed, p.games_found);
  if (already_installed > 0) {
    base += wxString::Format(_("   %zu already installed"), already_installed);
  }
  return base;
}

wxString PickDisplayName(const app::DiscoveredGame& g) {
  const std::string& name = app::PreferredName(g);
  return name.empty() ? wxString(_("(unknown title)"))
                      : wxString::FromUTF8(name);
}

wxString FormatRowLine1(const app::DiscoveredGame& g) {
  wxString tag = wxString::Format("[%s]", wxString::FromUTF8(g.format).Upper());
  return wxString::Format("%s  %s   %s", tag,
                          wxString::FromUTF8(g.title_id_hex),
                          PickDisplayName(g));
}

// Matches the launch-path auto-label format ("Disc N") when possible, falls
// back to media_id hex so multi-source rows without disc metadata still get
// a distinguishing label.
std::string DeriveDiscLabel(const app::DiscoveredGame& g) {
  if (g.disc_count > 1 && g.disc_number > 0) {
    return fmt::format("Disc {}", static_cast<unsigned>(g.disc_number));
  }
  if (g.media_id != 0) {
    return fmt::format("{:08X}", g.media_id);
  }
  return {};
}

wxString FormatRowDetails(const app::DiscoveredGame& g) {
  std::vector<wxString> parts;
  if (!g.content_type_name.empty()) {
    parts.push_back(wxString::Format(_("Type: %s"),
                                     wxString::FromUTF8(g.content_type_name)));
  }
  if (!g.publisher.empty()) {
    parts.push_back(
        wxString::Format(_("Publisher: %s"), wxString::FromUTF8(g.publisher)));
  }
  if (!g.module_name.empty()) {
    parts.push_back(
        wxString::Format(_("Module: %s"), wxString::FromUTF8(g.module_name)));
  }
  if (!g.version.empty()) {
    parts.push_back(
        wxString::Format(_("Version: %s"), wxString::FromUTF8(g.version)));
  }
  wxString joined;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      joined += "   ";
    }
    joined += parts[i];
  }
  return joined;
}

}  // namespace

GameScanProgressDialog::GameScanProgressDialog(
    wxWindow* parent, const std::filesystem::path& root_directory,
    InstalledTitleMap already_installed, ImportCallback import_callback,
    wxString import_disabled_reason)
    : wxDialog(parent, wxID_ANY, _("Scan for Games"), wxDefaultPosition,
               wxSize(780, 540), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      root_directory_(root_directory),
      already_installed_(std::move(already_installed)),
      already_installed_ids_([this] {
        std::unordered_set<uint32_t> ids;
        ids.reserve(already_installed_.size());
        for (const auto& kv : already_installed_) {
          ids.insert(kv.first);
        }
        return ids;
      }()),
      import_callback_(std::move(import_callback)),
      import_disabled_reason_(std::move(import_disabled_reason)),
      scanner_(std::make_unique<app::DirectoryScanner>()),
      timer_(this) {
  Build();
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Tick(); });
  // CallAfter so the dialog paints before the worker thread starts.
  Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {
    if (evt.IsShown() && scanner_ && !scanner_->GetProgress().started) {
      CallAfter([this]() {
        if (scanner_) {
          const XLanguage lang =
              XLanguageFromLocaleCode(GetActiveLocaleCanonicalName());
          scanner_->Start(root_directory_, lang);
        }
      });
    }
    evt.Skip();
  });
  timer_.Start(kTimerMs);
  Centre();
}

GameScanProgressDialog::~GameScanProgressDialog() {
  timer_.Stop();
  // unique_ptr destruction cancels + joins the worker thread.
}

void GameScanProgressDialog::Build() {
  auto* root = new wxBoxSizer(wxVERTICAL);

  header_label_ =
      new wxStaticText(this, wxID_ANY, FormatHeader(root_directory_));
  wxFont header_font = header_label_->GetFont();
  header_font.MakeBold();
  header_label_->SetFont(header_font);
  root->Add(header_label_, 0, wxALL, 8);

  counters_label_ = new wxStaticText(this, wxID_ANY, "");
  root->Add(counters_label_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

  root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

  list_panel_ = new wxScrolledWindow(this, wxID_ANY);
  list_panel_->SetScrollRate(0, 16);
  list_sizer_ = new wxBoxSizer(wxVERTICAL);
  list_panel_->SetSizer(list_sizer_);

  // Centered placeholder shown until the final list starts rendering.
  placeholder_label_ =
      new wxStaticText(list_panel_, wxID_ANY, _("Loading titles..."));
  wxFont placeholder_font = placeholder_label_->GetFont();
  placeholder_font.Scale(1.2f);
  placeholder_label_->SetFont(placeholder_font);
  list_sizer_->AddStretchSpacer(1);
  list_sizer_->Add(placeholder_label_, 0, wxALIGN_CENTER_HORIZONTAL);
  list_sizer_->AddStretchSpacer(1);

  root->Add(list_panel_, 1, wxEXPAND | wxALL, 6);

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  cancel_button_ = new wxButton(this, wxID_ANY, _("Cancel"));
  cancel_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCancel(); });
  buttons->Add(cancel_button_, 0, wxALL, 4);
  buttons->AddStretchSpacer(1);
  if (import_callback_) {
    import_button_ = new wxButton(this, wxID_ANY, _("Import"));
    import_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnImport(); });
    if (!import_disabled_reason_.IsEmpty()) {
      import_button_->SetToolTip(import_disabled_reason_);
    }
    import_button_->Hide();  // unhidden in Tick once we know the count
    buttons->Add(import_button_, 0, wxALL, 4);
  }
  close_button_ = new wxButton(this, wxID_OK, _("Close"));
  close_button_->Disable();
  buttons->Add(close_button_, 0, wxALL, 4);
  root->Add(buttons, 0, wxEXPAND | wxALL, 4);

  SetSizer(root);
}

void GameScanProgressDialog::AppendFinalRow(FinalRow& row) {
  auto& primary = row.sources.front();
  const int icon_px = list_panel_->FromDIP(kIconDip);
  const double scale = list_panel_->GetDPIScaleFactor();

  auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);

  auto bundle = WrapRgbaIcon(primary.icon_rgba, primary.icon_width,
                             primary.icon_height, icon_px, scale);
  auto* icon = new wxStaticBitmap(list_panel_, wxID_ANY, bundle);
  icon->SetMinSize(wxSize(icon_px, icon_px));
  row_sizer->Add(icon, 0, wxALL | wxALIGN_TOP, 6);

  auto* col = new wxBoxSizer(wxVERTICAL);

  auto* line1 =
      new wxStaticText(list_panel_, wxID_ANY, FormatRowLine1(primary));
  wxFont name_font(line1->GetFont());
  name_font.MakeBold();
  line1->SetFont(name_font);
  col->Add(line1, 0, wxBOTTOM, 2);

  wxString details = FormatRowDetails(primary);
  if (row.sources.size() == 1 && !row.source_installed.empty() &&
      row.source_installed[0]) {
    wxString status = _("Status: Installed");
    details = details.IsEmpty() ? status : (status + "   " + details);
  }
  if (!details.IsEmpty()) {
    auto* details_text = new wxStaticText(list_panel_, wxID_ANY, details);
    col->Add(details_text, 0, wxBOTTOM, 2);
  }

  if (row.sources.size() == 1) {
    auto* path_text =
        new wxStaticText(list_panel_, wxID_ANY,
                         wxString::FromUTF8(xe::path_to_utf8(primary.path)));
    col->Add(path_text, 0, wxBOTTOM, 2);
  } else {
    // Per-source: user-edited GPD label if installed, else DeriveDiscLabel.
    const std::vector<InstalledDisc>* installed_discs = nullptr;
    auto it = already_installed_.find(primary.title_id);
    if (it != already_installed_.end()) {
      installed_discs = &it->second;
    }
    for (size_t i = 0; i < row.sources.size(); ++i) {
      const auto& src = row.sources[i];
      const bool installed =
          i < row.source_installed.size() && row.source_installed[i];
      std::string label;
      if (installed && installed_discs) {
        for (const auto& d : *installed_discs) {
          if (d.path == src.path && !d.label.empty()) {
            label = d.label;
            break;
          }
        }
      }
      if (label.empty()) {
        label = DeriveDiscLabel(src);
      }
      const wxString prefix =
          installed ? wxString(_("(Installed) ")) : wxString();
      wxString line;
      if (!label.empty()) {
        line = wxString::Format("%s%s  %s", prefix, wxString::FromUTF8(label),
                                wxString::FromUTF8(xe::path_to_utf8(src.path)));
      } else {
        line = wxString::Format("%s%s", prefix,
                                wxString::FromUTF8(xe::path_to_utf8(src.path)));
      }
      auto* path_text = new wxStaticText(list_panel_, wxID_ANY, line);
      col->Add(path_text, 0, wxBOTTOM, 2);
    }
  }

  row_sizer->Add(col, 1, wxEXPAND | wxRIGHT, 6);
  list_sizer_->Add(row_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
  list_sizer_->Add(new wxStaticLine(list_panel_), 0,
                   wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);
}

void GameScanProgressDialog::StartFinalization() {
  finalization_started_ = true;

  list_panel_->Freeze();
  list_sizer_->Clear(/*delete_windows=*/true);
  placeholder_label_ = nullptr;
  list_panel_->Thaw();

  auto all = scanner_->GetAllResults();

  // Group every entry sharing (format, title_id) — catches multi-disc games
  // even when disc_count metadata is missing, and duplicate dumps.
  std::unordered_map<std::string, size_t> group_index;
  std::vector<FinalRow> rows;
  for (auto& g : all) {
    if (!g.title_id_hex.empty()) {
      std::string key = g.format + ":" + g.title_id_hex;
      auto it = group_index.find(key);
      if (it != group_index.end()) {
        rows[it->second].sources.push_back(std::move(g));
        continue;
      }
      group_index[key] = rows.size();
    }
    FinalRow r;
    r.sort_key = ToLowerAsciiCopy(app::PreferredName(g));
    r.sources.push_back(std::move(g));
    rows.push_back(std::move(r));
  }

  // Sort sources before computing source_installed so the bool indices stay
  // aligned. disc_number leads; path is the tie-break for missing metadata.
  for (auto& r : rows) {
    if (r.sources.size() > 1) {
      std::sort(r.sources.begin(), r.sources.end(),
                [](const auto& a, const auto& b) {
                  if (a.disc_number != b.disc_number) {
                    return a.disc_number < b.disc_number;
                  }
                  return a.path < b.path;
                });
    }
  }

  for (auto& r : rows) {
    r.source_installed.assign(r.sources.size(), false);
    for (size_t i = 0; i < r.sources.size(); ++i) {
      auto it = already_installed_.find(r.sources[i].title_id);
      if (it == already_installed_.end()) {
        continue;
      }
      for (const auto& d : it->second) {
        if (d.path == r.sources[i].path) {
          r.source_installed[i] = true;
          break;
        }
      }
    }
  }

  std::sort(rows.begin(), rows.end(), [](const FinalRow& a, const FinalRow& b) {
    if (a.sort_key != b.sort_key) {
      return a.sort_key < b.sort_key;
    }
    return a.sources.front().title_id_hex < b.sources.front().title_id_hex;
  });

  new_title_count_ = 0;
  installed_title_count_ = 0;
  for (const auto& r : rows) {
    bool any_installed = false;
    for (size_t i = 0; i < r.sources.size(); ++i) {
      if (r.source_installed[i]) {
        any_installed = true;
      } else {
        ++new_title_count_;
      }
    }
    if (any_installed) {
      ++installed_title_count_;
    }
  }

  // Snapshot survives the chunked render that drains pending_final_rows_.
  all_rows_ = rows;
  pending_final_rows_ = std::move(rows);
}

void GameScanProgressDialog::Tick() {
  if (!scanner_) {
    return;
  }
  auto progress = scanner_->GetProgress();
  // Scanner-side tally is per-source so it over-counts multi-disc titles
  // during the scan; switch to the precise per-title count once we have it.
  const size_t installed =
      finalization_started_
          ? installed_title_count_
          : scanner_->CountResultsWithTitleId(already_installed_ids_);
  counters_label_->SetLabel(FormatCounters(progress, installed));

  if (progress.done && !progress.cancelled && !finalization_started_) {
    StartFinalization();
  }

  // Bounded chunk-per-tick keeps the UI responsive on large libraries.
  if (finalization_started_ && !pending_final_rows_.empty()) {
    constexpr size_t kFinalRowsPerTick = 32;
    size_t to_add = std::min(kFinalRowsPerTick, pending_final_rows_.size());
    list_panel_->Freeze();
    for (size_t i = 0; i < to_add; ++i) {
      AppendFinalRow(pending_final_rows_[i]);
    }
    pending_final_rows_.erase(pending_final_rows_.begin(),
                              pending_final_rows_.begin() + to_add);
    list_panel_->FitInside();
    list_panel_->Layout();
    list_panel_->Thaw();
  }

  const bool ui_drained = pending_final_rows_.empty();
  const bool truly_done = progress.done &&
                          (progress.cancelled || finalization_started_) &&
                          ui_drained;
  if (progress.cancelled && placeholder_label_) {
    placeholder_label_->SetLabel(_("Cancelled."));
    list_panel_->Layout();
  }
  cancel_button_->Enable(!progress.done && !cancel_pressed_);
  if (import_button_) {
    // Disabled until row render is done so writes can't fire mid-layout.
    // import_disabled_reason_ keeps it disabled regardless.
    if (finalization_started_) {
      if (new_title_count_ > 0) {
        import_button_->SetLabel(
            wxString::Format(_("Import %zu New Items"), new_title_count_));
        import_button_->Show();
        import_button_->Enable(truly_done && import_disabled_reason_.IsEmpty());
      } else {
        import_button_->Hide();
      }
    }
  }
  close_button_->Enable(truly_done);
  if (truly_done && !was_done_) {
    close_button_->SetFocus();
  }
  was_done_ = truly_done;

  Layout();
}

void GameScanProgressDialog::OnCancel() {
  if (scanner_) {
    scanner_->Cancel();
  }
  cancel_pressed_ = true;
  cancel_button_->Disable();
  cancel_button_->SetLabel(_("Cancelling..."));
}

void GameScanProgressDialog::OnImport() {
  if (!import_callback_) {
    return;
  }
  std::vector<std::vector<PendingImport>> to_import;
  for (const auto& r : all_rows_) {
    std::vector<PendingImport> group;
    const bool needs_label = r.sources.size() > 1;
    for (size_t i = 0; i < r.sources.size(); ++i) {
      if (i < r.source_installed.size() && r.source_installed[i]) {
        continue;
      }
      PendingImport pi;
      pi.game = r.sources[i];
      if (needs_label) {
        pi.disc_label = DeriveDiscLabel(r.sources[i]);
      }
      group.push_back(std::move(pi));
    }
    if (!group.empty()) {
      to_import.push_back(std::move(group));
    }
  }
  import_button_->Disable();
  const bool ok = import_callback_(std::move(to_import));
  if (ok) {
    EndModal(wxID_OK);
  } else {
    import_button_->Enable(true);
  }
}

}  // namespace ui
}  // namespace xe
