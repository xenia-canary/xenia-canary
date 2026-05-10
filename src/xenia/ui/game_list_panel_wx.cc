/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_list_panel_wx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <wx/button.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/mstream.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/textdlg.h>
#include <wx/variant.h>

#include "third_party/fmt/include/fmt/chrono.h"
#include "third_party/fmt/include/fmt/format.h"

#include "xenia/app/emulator_window.h"
#include "xenia/app/game_compat_db.h"
#include "xenia/base/chrono.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/system.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/patcher/patch_db.h"
#include "xenia/ui/game_config_dialog_wx.h"
#include "xenia/ui/patches_dialog_wx.h"
#include "xenia/xbox.h"

#include <thread>

namespace xe {
namespace app {

namespace {

constexpr int kIconSize = 64;

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string FormatLastPlayed(time_t timestamp) {
  if (timestamp == 0) {
    return "-";
  }
  std::tm tm = {};
#if XE_PLATFORM_WIN32
  localtime_s(&tm, &timestamp);
#else
  localtime_r(&timestamp, &tm);
#endif
  return fmt::format("{:%Y-%m-%d %H:%M}", tm);
}

// `text` may contain a single \n to split into two centered lines; translators
// pick the break point. No newline = single centered line.
wxBitmapBundle MakeTextPlaceholder(const wxString& text) {
  wxBitmap bmp(kIconSize, kIconSize, 32);
  wxMemoryDC dc(bmp);
  dc.SetBackground(wxBrush(wxColour(60, 60, 60)));
  dc.Clear();
  wxFont font = dc.GetFont();
  font.Scale(0.85f);
  dc.SetFont(font);
  dc.SetTextForeground(wxColour(180, 180, 180));
  wxString line1 = text.BeforeFirst('\n');
  wxString line2 = text.AfterFirst('\n');
  wxSize l1 = dc.GetTextExtent(line1);
  if (line2.empty()) {
    int y = (kIconSize - l1.y) / 2;
    dc.DrawText(line1, (kIconSize - l1.x) / 2, y);
  } else {
    wxSize l2 = dc.GetTextExtent(line2);
    int total_h = l1.y + l2.y + 2;
    int y = (kIconSize - total_h) / 2;
    dc.DrawText(line1, (kIconSize - l1.x) / 2, y);
    dc.DrawText(line2, (kIconSize - l2.x) / 2, y + l1.y + 2);
  }
  dc.SelectObject(wxNullBitmap);
  return wxBitmapBundle::FromBitmap(bmp);
}

const wxBitmapBundle& LoggedOutPlaceholder() {
  static const wxBitmapBundle k = MakeTextPlaceholder(_("Not\nlogged in"));
  return k;
}

const wxBitmapBundle& NotPlayedPlaceholder() {
  static const wxBitmapBundle k = MakeTextPlaceholder(_("Not\nplayed"));
  return k;
}

wxString CompatStateName(CompatState state) {
  switch (state) {
    case CompatState::kPlayable:
      return _("Playable");
    case CompatState::kGameplay:
      return _("Gameplay");
    case CompatState::kLoads:
      return _("Loads");
    case CompatState::kUnplayable:
      return _("Unplayable");
    case CompatState::kUnknown:
    default:
      return _("Unknown");
  }
}

wxColour CompatColor(CompatState state) {
  switch (state) {
    case CompatState::kPlayable:
      return wxColour(80, 200, 90);
    case CompatState::kGameplay:
      return wxColour(230, 200, 60);
    case CompatState::kLoads:
    case CompatState::kUnplayable:
      return wxColour(220, 80, 80);
    case CompatState::kUnknown:
    default:
      return wxColour(140, 140, 140);
  }
}

// Pixel-AA filled circle on a transparent bitmap; portable across themes.
wxBitmapBundle MakeCompatBall(CompatState state) {
  constexpr int kSize = 28;
  constexpr float kRadius = 7.0f;
  wxColour color = CompatColor(state);
  wxImage image(kSize, kSize);
  image.SetAlpha();
  unsigned char* rgb = image.GetData();
  unsigned char* alpha = image.GetAlpha();
  std::memset(alpha, 0, kSize * kSize);
  const float cx = kSize * 0.5f;
  const float cy = kSize * 0.5f;
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      float dx = x + 0.5f - cx;
      float dy = y + 0.5f - cy;
      float d = std::sqrt(dx * dx + dy * dy);
      float a = std::clamp(kRadius - d + 0.5f, 0.0f, 1.0f);
      if (a > 0.0f) {
        size_t pi = (static_cast<size_t>(y) * kSize + x) * 3;
        rgb[pi + 0] = color.Red();
        rgb[pi + 1] = color.Green();
        rgb[pi + 2] = color.Blue();
        alpha[y * kSize + x] = static_cast<unsigned char>(a * 255.0f + 0.5f);
      }
    }
  }
  return wxBitmapBundle::FromBitmap(wxBitmap(image));
}

const wxBitmapBundle& CompatBall(CompatState state) {
  static const wxBitmapBundle kBalls[] = {
      MakeCompatBall(CompatState::kUnknown),
      MakeCompatBall(CompatState::kUnplayable),
      MakeCompatBall(CompatState::kLoads),
      MakeCompatBall(CompatState::kGameplay),
      MakeCompatBall(CompatState::kPlayable),
  };
  return kBalls[static_cast<size_t>(state)];
}

wxString EscapeMarkup(const wxString& s) {
  wxString out;
  out.reserve(s.length());
  for (auto it = s.begin(); it != s.end(); ++it) {
    wxUniChar ch = *it;
    if (ch == '<') {
      out += "&lt;";
    } else if (ch == '>') {
      out += "&gt;";
    } else if (ch == '&') {
      out += "&amp;";
    } else {
      out += ch;
    }
  }
  return out;
}

wxBitmapBundle DecodeIcon(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return wxBitmapBundle();
  }
  wxMemoryInputStream stream(data.data(), data.size());
  wxImage image;
  if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
    return wxBitmapBundle();
  }
  if (image.GetWidth() != kIconSize || image.GetHeight() != kIconSize) {
    image.Rescale(kIconSize, kIconSize, wxIMAGE_QUALITY_HIGH);
  }
  return wxBitmapBundle::FromBitmap(wxBitmap(image));
}

}  // namespace

GameListPanel::GameListPanel(wxWindow* parent, EmulatorWindow* emulator_window)
    : wxPanel(parent, wxID_ANY), emulator_window_(emulator_window) {
  search_ = new wxSearchCtrl(this, wxID_ANY);
  search_->ShowCancelButton(true);
  search_->SetDescriptiveText(_("Search games..."));
  {
    wxFont f = search_->GetFont();
    f.Scale(1.4f);
    search_->SetFont(f);
  }

  list_ = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition,
                                 wxDefaultSize, wxDV_ROW_LINES | wxDV_SINGLE);
  list_->SetMinSize(wxSize(0, 0));

  // Derive sizes from the list's actual font so the layout grows with the
  // user's Windows text-scaling setting instead of clipping at fixed pixels.
  wxClientDC dc(this);
  dc.SetFont(list_->GetFont());
  const int char_h = dc.GetCharHeight();
  const int char_w = dc.GetCharWidth();
  auto col_w = [&](const wxString& header, const wxString& sample) {
    int hw = dc.GetTextExtent(header).GetWidth();
    int sw = dc.GetTextExtent(sample).GetWidth();
    // Leave room for the sort-indicator arrow and a small padding gap.
    return std::max(hw, sw) + char_w * 4;
  };
  // Title is rendered with x-large markup (~1.5x base font); row must fit
  // either that or the fixed 64px icon.
  list_->SetRowHeight(
      std::max(kIconSize + 8, static_cast<int>(char_h * 1.6f) + 8));
  const wxString status_header = _("Status");
  const wxString icon_header = _("Icon");
  const wxString title_header = _("Title");
  const wxString achievements_header = _("Achievements");
  const wxString gamerscore_header = _("Gamerscore");
  const wxString last_played_header = _("Last Played");
  list_->AppendBitmapColumn(status_header, 0, wxDATAVIEW_CELL_INERT,
                            col_w(status_header, status_header), wxALIGN_CENTER,
                            0);
  list_->AppendBitmapColumn(icon_header, 1, wxDATAVIEW_CELL_INERT,
                            kIconSize + 8, wxALIGN_CENTER, 0);
  // Title is the only flexible column; the rest are pinned at their measured
  // widths so a wider window doesn't stretch them.
  auto* title_col = list_->AppendTextColumn(
      title_header, wxDATAVIEW_CELL_INERT, static_cast<int>(char_w * 30 * 1.5f),
      wxALIGN_LEFT, 0);
  if (title_col) {
    if (auto* tr =
            dynamic_cast<wxDataViewTextRenderer*>(title_col->GetRenderer())) {
      tr->EnableMarkup(true);
    }
  }
  list_->AppendTextColumn(achievements_header, wxDATAVIEW_CELL_INERT,
                          col_w(achievements_header, "9999/9999"), wxALIGN_LEFT,
                          0);
  list_->AppendTextColumn(gamerscore_header, wxDATAVIEW_CELL_INERT,
                          col_w(gamerscore_header, "99999/999999 G"),
                          wxALIGN_LEFT, 0);
  // Trim the standard padding a bit — Last Played is right-aligned, so the
  // header's sort arrow doesn't fight with the value text.
  const int last_played_width =
      col_w(last_played_header, "0000-00-00 00:00") - char_w * 2;
  list_->AppendTextColumn(last_played_header, wxDATAVIEW_CELL_INERT,
                          last_played_width, wxALIGN_RIGHT, 0);

  // wxDataViewCtrl always auto-expands the *last* column to fill leftover
  // width regardless of flags. Pre-empt that by stretching the Title column
  // ourselves so the last column has no slack to absorb.
  const int min_title_width = char_w * 20;
  list_->Bind(wxEVT_SIZE, [this, last_played_width, min_title_width,
                           char_w](wxSizeEvent& event) {
    event.Skip();
    if (list_->GetColumnCount() < 6) {
      return;
    }
    list_->GetColumn(5)->SetWidth(last_played_width);
    int fixed = 0;
    for (unsigned i = 0; i < list_->GetColumnCount(); ++i) {
      if (i == 2) {
        continue;
      }
      fixed += list_->GetColumn(i)->GetWidth();
    }
    int avail = list_->GetClientSize().GetWidth();
#if XE_PLATFORM_MAC
    // wx's DoGetClientSize doesn't subtract NSScrollView's own scroller, and
    // NSOutlineView adds intercell spacing + per-cell insets that aren't in
    // the widths we set. Reserve enough to absorb all of it.
    avail -= wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, list_) + char_w * 10;
#endif
    list_->GetColumn(2)->SetWidth(std::max(avail - fixed, min_title_width));
  });

  search_->Bind(wxEVT_TEXT, &GameListPanel::OnSearch, this);
  search_->Bind(wxEVT_SEARCH_CANCEL,
                [this](wxCommandEvent&) { search_->Clear(); });
  list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, &GameListPanel::OnItemActivated,
              this);
  list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
              &GameListPanel::OnSelectionChanged, this);
  list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,
              &GameListPanel::OnItemContextMenu, this);
  list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_CLICK,
              &GameListPanel::OnColumnHeaderClick, this);
  // Bind motion on both because the inner client window may consume it.
  list_->Bind(wxEVT_MOTION, &GameListPanel::OnListMouseMotion, this);
  if (auto* main = list_->GetMainWindow()) {
    main->Bind(wxEVT_MOTION, &GameListPanel::OnListMouseMotion, this);
  }

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(search_, wxSizerFlags().Expand().Border(wxALL, 4));
  sizer->Add(list_,
             wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 4));
  SetSizer(sizer);

  // wxPanel's default size handler doesn't always relayout reliably when the
  // panel is mounted by AUI — call Layout() explicitly on every size event.
  Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
    Layout();
    event.Skip();
  });
}

void GameListPanel::Reload() {
  entries_.clear();

  if (!emulator_window_ || !emulator_window_->emulator()) {
    Repopulate();
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    Repopulate();
    return;
  }
  auto* xam_state = kernel_state->xam_state();
  if (!xam_state) {
    Repopulate();
    return;
  }
  auto* profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    Repopulate();
    return;
  }

  auto scanned = profile_manager->ScanAllProfilesForTitles();
  entries_.reserve(scanned.size());
  for (const auto& s : scanned) {
    Entry e;
    e.title_id = s.title_id;
    e.title_name = s.title_name;
    e.path = s.path_to_file;
    e.discs.reserve(s.all_discs.size());
    for (const auto& d : s.all_discs) {
      e.discs.push_back(Disc{d.path, d.label});
    }
    e.last_run_time = s.last_run_time;
    entries_.push_back(std::move(e));
  }

  // Per-profile timestamps are cheap; icons are PNG decodes and dominate the
  // load. Apply timestamps now, render the list, then trickle icons in.
  LoadTimestampsFromProfiles();
  Repopulate();
  StartIconLoad();
}

void GameListPanel::LoadTimestampsFromProfiles() {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }
  auto* xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }
  auto* profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    return;
  }

  for (uint8_t user_index = 0; user_index < 4; ++user_index) {
    auto* profile = profile_manager->GetProfile(user_index);
    if (!profile) {
      continue;
    }
    const auto& dashboard = profile->dashboard_gpd();
    if (!dashboard.IsValid()) {
      continue;
    }
    auto title_infos = dashboard.GetTitlesInfo();
    for (auto& entry : entries_) {
      for (const auto& info : title_infos) {
        if (info->title_id == entry.title_id && info->last_played.is_valid()) {
          auto last_played_tp =
              chrono::WinSystemClock::to_sys(info->last_played.to_time_point());
          entry.last_run_time =
              std::chrono::system_clock::to_time_t(last_played_tp);
          break;
        }
      }
      auto stats = profile->GetTitleAchievementStats(entry.title_id);
      if (stats.achievements_total > entry.achievements_total) {
        entry.achievements_total = stats.achievements_total;
        entry.achievements_unlocked = stats.achievements_unlocked;
        entry.gamerscore_total = stats.gamerscore_total;
        entry.gamerscore_earned = stats.gamerscore_earned;
      }
    }
  }
}

void GameListPanel::StartIconLoad() {
  ++icon_load_generation_;
  if (entries_.empty()) {
    return;
  }
  int gen = icon_load_generation_;
  CallAfter([this, gen]() { ProcessIconChunk(0, gen); });
}

void GameListPanel::ProcessIconChunk(size_t start, int gen) {
  if (gen != icon_load_generation_) {
    return;
  }
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }
  auto* xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }
  auto* profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    return;
  }

  // Tune so each chunk stays well under one frame (~16ms): a PNG decode +
  // rescale is a few ms, so 8 per chunk keeps the UI responsive.
  constexpr size_t kChunkSize = 8;
  size_t end = std::min(start + kChunkSize, entries_.size());
  for (size_t i = start; i < end; ++i) {
    auto& entry = entries_[i];
    if (entry.icon.IsOk()) {
      continue;
    }
    std::vector<uint8_t> data;
    for (uint8_t user_index = 0; user_index < 4 && data.empty(); ++user_index) {
      auto* profile = profile_manager->GetProfile(user_index);
      if (!profile) {
        continue;
      }
      data = profile->GetTitleIcon(entry.title_id);
    }
    if (data.empty()) {
      continue;
    }
    entry.icon = DecodeIcon(data);
    if (!entry.icon.IsOk()) {
      continue;
    }
    for (size_t r = 0; r < visible_indices_.size(); ++r) {
      if (visible_indices_[r] == i) {
        wxVariant v;
        v << entry.icon;
        list_->SetValue(v, static_cast<unsigned int>(r), 1);
        break;
      }
    }
  }
  if (end < entries_.size()) {
    CallAfter([this, end, gen]() { ProcessIconChunk(end, gen); });
  }
}

void GameListPanel::SetProfileSignedIn(bool signed_in) {
  if (profile_signed_in_ == signed_in) {
    return;
  }
  profile_signed_in_ = signed_in;
  Repopulate();
}

void GameListPanel::OnSearch(wxCommandEvent&) {
  filter_lower_ = ToLower(search_->GetValue().utf8_string());
  Repopulate();
}

void GameListPanel::LaunchOrPrompt(const std::filesystem::path& path) {
  if (!launch_cb_) {
    return;
  }
  std::error_code ec;
  if (!path.empty() && std::filesystem::exists(path, ec)) {
    launch_cb_(path);
    return;
  }
  wxString warning = path.empty()
                         ? _("No file path is set for this title.")
                         : wxString::Format(_("File not found:\n%s"),
                                            wxString::FromUTF8(path.string()));
  wxMessageDialog confirm(this, warning + _("\n\nBrowse for the file?"),
                          _("Title not found"), wxYES_NO | wxICON_WARNING);
  if (confirm.ShowModal() != wxID_YES) {
    return;
  }
  std::filesystem::path initial_dir =
      path.empty() ? std::filesystem::path() : path.parent_path();
  wxFileDialog dlg(this, _("Select Content Package"),
                   initial_dir.empty()
                       ? wxString()
                       : wxString::FromUTF8(initial_dir.string()),
                   wxEmptyString,
                   _("Supported Files|*;*.iso;*.xex;*.zar|"
                     "Disc Image (*.iso)|*.iso|"
                     "Disc Archive (*.zar)|*.zar|"
                     "Xbox Executable (*.xex)|*.xex|"
                     "All Files (*.*)|*.*"),
                   wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() == wxID_OK) {
    launch_cb_(std::filesystem::path(dlg.GetPath().utf8_string()));
  }
}

void GameListPanel::ShowEditDiscsDialog(size_t entry_index) {
  if (entry_index >= entries_.size()) {
    return;
  }
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state || !kernel_state->xam_state()) {
    return;
  }
  auto* profile_manager = kernel_state->xam_state()->profile_manager();
  if (!profile_manager) {
    return;
  }
  auto* profile = profile_manager->GetProfile(static_cast<uint8_t>(0));
  if (!profile) {
    return;
  }

  uint32_t title_id = entries_[entry_index].title_id;

  wxDialog dlg(this, wxID_ANY, _("Edit Discs"), wxDefaultPosition,
               wxSize(500, 320), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  auto* list = new wxListBox(&dlg, wxID_ANY);
  sizer->Add(list, 1, wxEXPAND | wxALL, 8);

  auto refresh = [&]() {
    list->Clear();
    if (entry_index >= entries_.size()) {
      return;
    }
    const auto& default_path = entries_[entry_index].path;
    size_t disc_num = 1;
    for (const auto& disc : entries_[entry_index].discs) {
      wxString label = disc.label.empty()
                           ? wxString::Format(_("Disc %zu"), disc_num)
                           : wxString::FromUTF8(disc.label);
      if (!default_path.empty() && disc.path == default_path) {
        label += _(" (default)");
      }
      list->Append(label, new wxStringClientData(
                              wxString::FromUTF8(disc.path.string())));
      ++disc_num;
    }
    if (list->GetCount() > 0) {
      list->SetSelection(0);
    }
  };
  refresh();

  auto* button_row = new wxBoxSizer(wxHORIZONTAL);
  auto* rename = new wxButton(&dlg, wxID_ANY, _("Rename..."));
  auto* remove = new wxButton(&dlg, wxID_ANY, _("Remove"));
  auto* close = new wxButton(&dlg, wxID_CLOSE, _("Close"));
  button_row->Add(rename, 0, wxRIGHT, 4);
  button_row->Add(remove, 0, wxRIGHT, 4);
  button_row->AddStretchSpacer();
  button_row->Add(close, 0);
  sizer->Add(button_row, 0, wxEXPAND | wxALL, 8);

  auto current_disc_path = [&]() -> std::filesystem::path {
    int sel = list->GetSelection();
    if (sel == wxNOT_FOUND) {
      return {};
    }
    auto* data = static_cast<wxStringClientData*>(list->GetClientObject(sel));
    return data ? std::filesystem::path(data->GetData().utf8_string())
                : std::filesystem::path{};
  };

  rename->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    int sel = list->GetSelection();
    if (sel == wxNOT_FOUND) {
      return;
    }
    wxString current = list->GetString(sel);
    wxTextEntryDialog input(&dlg, _("Enter new label:"), _("Rename Disc"),
                            current);
    if (input.ShowModal() != wxID_OK) {
      return;
    }
    std::string new_label = input.GetValue().utf8_string();
    if (new_label.find("::") != std::string::npos) {
      wxMessageDialog(&dlg, _("Label cannot contain '::'."), _("Invalid Label"),
                      wxOK | wxICON_WARNING)
          .ShowModal();
      return;
    }
    profile->SetDiscLabel(title_id, current_disc_path(), new_label);
    if (entry_index < entries_.size() &&
        size_t(sel) < entries_[entry_index].discs.size()) {
      entries_[entry_index].discs[sel].label = new_label;
    }
    refresh();
    list->SetSelection(sel);
  });

  remove->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
    int sel = list->GetSelection();
    if (sel == wxNOT_FOUND) {
      return;
    }
    wxString label = list->GetString(sel);
    wxMessageDialog confirm(
        &dlg, wxString::Format(_("Remove '%s' from the disc list?"), label),
        _("Remove Disc"), wxYES_NO | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES) {
      return;
    }
    profile->RemoveDiscPath(title_id, current_disc_path());
    if (entry_index < entries_.size() &&
        size_t(sel) < entries_[entry_index].discs.size()) {
      entries_[entry_index].discs.erase(entries_[entry_index].discs.begin() +
                                        sel);
    }
    refresh();
  });

  close->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dlg.EndModal(wxID_CLOSE); });

  dlg.SetSizer(sizer);
  dlg.ShowModal();

  // Reflect any changes in the main list view.
  Repopulate();
}

void GameListPanel::OnItemActivated(wxDataViewEvent& event) {
  int row = list_->ItemToRow(event.GetItem());
  if (row < 0 || row >= static_cast<int>(visible_indices_.size())) {
    return;
  }
  size_t idx = visible_indices_[row];
  if (idx >= entries_.size()) {
    return;
  }
  LaunchOrPrompt(entries_[idx].path);
}

void GameListPanel::OnSelectionChanged(wxDataViewEvent&) {
  if (selection_changed_cb_) {
    selection_changed_cb_();
  }
}

CompatState GameListPanel::GetEntryCompatState(const Entry& e) const {
  CompatState base = GetCompatState(e.title_id);
  // Best of (achievements unlocked %) and (gamerscore earned %).
  float pct = 0.0f;
  if (e.achievements_total > 0) {
    pct = std::max(pct, static_cast<float>(e.achievements_unlocked) /
                            static_cast<float>(e.achievements_total));
  }
  if (e.gamerscore_total > 0) {
    pct = std::max(pct, static_cast<float>(e.gamerscore_earned) /
                            static_cast<float>(e.gamerscore_total));
  }
  if (pct >= 0.80f) {
    return CompatState::kPlayable;
  }
  if (base == CompatState::kUnknown && pct >= 0.10f) {
    return CompatState::kGameplay;
  }
  return base;
}

void GameListPanel::OnListMouseMotion(wxMouseEvent& event) {
  event.Skip();
  if (!list_) {
    return;
  }
  // HitTest expects coords in list_'s frame regardless of event origin.
  wxPoint pt = list_->ScreenToClient(::wxGetMousePosition());
  wxDataViewItem item;
  wxDataViewColumn* col = nullptr;
  list_->HitTest(pt, item, col);

  wxString text;
  if (item.IsOk() && col) {
    int pos = list_->GetColumnPosition(col);
    int row = list_->ItemToRow(item);
    if (row >= 0 && row < static_cast<int>(visible_indices_.size())) {
      size_t idx = visible_indices_[row];
      if (idx < entries_.size()) {
        const auto& e = entries_[idx];
        if (pos == 0) {
          text = CompatStateName(GetEntryCompatState(e));
        } else if (pos == 2 && !e.path.empty()) {
          text = wxString::FromUTF8(xe::path_to_utf8(e.path));
        }
      }
    }
  }
  if (text == last_tooltip_text_) {
    return;
  }
  last_tooltip_text_ = text;
  if (text.empty()) {
    list_->UnsetToolTip();
  } else {
    list_->SetToolTip(text);
  }
}

void GameListPanel::OnItemContextMenu(wxDataViewEvent& event) {
  int row = list_->ItemToRow(event.GetItem());
  if (row < 0 || row >= static_cast<int>(visible_indices_.size())) {
    return;
  }
  size_t idx = visible_indices_[row];
  if (idx >= entries_.size()) {
    return;
  }
  const Entry& entry = entries_[idx];

  auto open_path_in_explorer = [](std::filesystem::path path) {
    std::thread(xe::LaunchFileExplorer, std::move(path)).detach();
  };

  wxMenu menu;
  if (!entry.path.empty()) {
    if (entry.discs.size() > 1) {
      auto* launch_submenu = new wxMenu;
      size_t disc_num = 1;
      for (const auto& disc : entry.discs) {
        wxString label = disc.label.empty()
                             ? wxString::Format(_("Disc %zu"), disc_num)
                             : wxString::FromUTF8(disc.label);
        auto* item = launch_submenu->Append(wxID_ANY, label);
        menu.Bind(
            wxEVT_MENU,
            [this, path = disc.path](wxCommandEvent&) { LaunchOrPrompt(path); },
            item->GetId());
        ++disc_num;
      }
      menu.AppendSubMenu(launch_submenu, _("Launch"));
      auto* edit_discs = menu.Append(wxID_ANY, _("Edit Discs..."));
      menu.Bind(
          wxEVT_MENU,
          [this, idx](wxCommandEvent&) { ShowEditDiscsDialog(idx); },
          edit_discs->GetId());
    } else {
      auto* launch = menu.Append(wxID_ANY, _("Launch"));
      menu.Bind(
          wxEVT_MENU,
          [this, path = entry.path](wxCommandEvent&) { LaunchOrPrompt(path); },
          launch->GetId());
    }
    auto* open_folder = menu.Append(wxID_ANY, _("Open containing folder"));
    menu.Bind(
        wxEVT_MENU,
        [open_path_in_explorer, parent = entry.path.parent_path()](
            wxCommandEvent&) { open_path_in_explorer(parent); },
        open_folder->GetId());
  }

  if (entry.title_id != 0 && emulator_window_ && emulator_window_->emulator()) {
    auto* kernel_state = emulator_window_->emulator()->kernel_state();
    auto* xam_state = kernel_state ? kernel_state->xam_state() : nullptr;
    auto* profile_manager = xam_state ? xam_state->profile_manager() : nullptr;
    if (profile_manager) {
      menu.AppendSeparator();
      auto* primary = profile_manager->GetProfile(uint8_t(0));
      uint64_t xuid = primary ? primary->xuid() : 0;

      auto add_content_item = [&](const wxString& label,
                                  std::filesystem::path path, bool gated) {
        auto* item = menu.Append(wxID_ANY, label);
        if (gated || !std::filesystem::exists(path)) {
          item->Enable(false);
          return;
        }
        menu.Bind(
            wxEVT_MENU,
            [open_path_in_explorer, path = std::move(path)](wxCommandEvent&) {
              open_path_in_explorer(path);
            },
            item->GetId());
      };

      add_content_item(_("Saves"),
                       profile_manager->GetProfileContentPath(
                           xuid, entry.title_id, XContentType::kSavedGame),
                       /*gated=*/!primary);
      add_content_item(_("Title Updates"),
                       profile_manager->GetProfileContentPath(
                           0, entry.title_id, XContentType::kInstaller),
                       false);
      add_content_item(
          _("DLC"),
          profile_manager->GetProfileContentPath(
              0, entry.title_id, XContentType::kMarketplaceContent),
          false);
    }
  }

  if (entry.title_id != 0) {
    auto bundled = xe::patcher::EnumerateBundledPatchesForTitle(entry.title_id);
    if (bundled.empty()) {
      auto* item = menu.Append(wxID_ANY, _("Patches"));
      item->Enable(false);
    } else {
      auto* patches_submenu = new wxMenu;
      for (const auto& bpf : bundled) {
        std::string display = bpf.filename;
        if (auto dash = display.find(" - "); dash != std::string::npos) {
          display = display.substr(dash + 3);
        }
        if (auto suffix = display.rfind(".patch.toml");
            suffix != std::string::npos) {
          display = display.substr(0, suffix);
        }
        auto* item =
            patches_submenu->Append(wxID_ANY, wxString::FromUTF8(display));
        menu.Bind(
            wxEVT_MENU,
            [this, title_id = entry.title_id, bpf](wxCommandEvent&) {
              auto* dlg = new PatchesDialog(wxGetTopLevelParent(this),
                                            emulator_window_, title_id, bpf);
              dlg->ShowModal();
              dlg->Destroy();
            },
            item->GetId());
      }
      menu.AppendSubMenu(patches_submenu, _("Patches"));
    }

    auto* config_overrides = menu.Append(wxID_ANY, _("Config Overrides..."));
    menu.Bind(
        wxEVT_MENU,
        [this, title_id = entry.title_id,
         display_name = entry.title_name](wxCommandEvent&) {
          auto* dlg =
              new GameConfigDialog(wxGetTopLevelParent(this), emulator_window_,
                                   title_id, display_name);
          dlg->ShowModal();
          dlg->Destroy();
        },
        config_overrides->GetId());

    menu.AppendSeparator();
    auto* compat = new wxMenu;
    auto* canary = compat->Append(wxID_ANY, _("Canary"));
    auto* master = compat->Append(wxID_ANY, _("Master"));
    menu.Bind(
        wxEVT_MENU,
        [title_id = entry.title_id](wxCommandEvent&) {
          xe::LaunchWebBrowser(fmt::format(
              "https://github.com/xenia-canary/game-compatibility/issues"
              "?q=is%3Aissue+is%3Aopen+{:08X}",
              title_id));
        },
        canary->GetId());
    menu.Bind(
        wxEVT_MENU,
        [title_id = entry.title_id](wxCommandEvent&) {
          xe::LaunchWebBrowser(fmt::format(
              "https://github.com/xenia-project/game-compatibility/issues"
              "?q=is%3Aissue+is%3Aopen+{:08X}",
              title_id));
        },
        master->GetId());
    menu.AppendSubMenu(compat, _("Compatibility"));

    menu.AppendSeparator();
    auto* remove = menu.Append(wxID_ANY, _("Remove from list"));
    menu.Bind(
        wxEVT_MENU,
        [this, title_id = entry.title_id,
         display_name = entry.title_name](wxCommandEvent&) {
          wxString name = display_name.empty()
                              ? wxString::Format(_("title %08X"), title_id)
                              : wxString::FromUTF8(display_name);
          wxString message = wxString::Format(
              _("Remove %s from the list?\n\nThis only clears it from the "
                "dashboard records; game files on disk are kept."),
              name);
          int reply = wxMessageBox(message, _("Remove from list"),
                                   wxYES_NO | wxICON_QUESTION,
                                   wxGetTopLevelParent(this));
          if (reply != wxYES) {
            return;
          }
          if (!emulator_window_ || !emulator_window_->emulator()) {
            return;
          }
          auto* kernel_state = emulator_window_->emulator()->kernel_state();
          auto* xam_state = kernel_state ? kernel_state->xam_state() : nullptr;
          auto* pm = xam_state ? xam_state->profile_manager() : nullptr;
          if (!pm) {
            return;
          }
          pm->RemoveTitleFromAllProfiles(title_id);
          Reload();
        },
        remove->GetId());
  }

  // 2px upward offset puts the cursor in the menu's top padding so X11's
  // trailing right-release doesn't activate the first item.
  // TODO(has207): retest on wxWidgets update — drop if PopupMenu starts
  // anchoring via gtk_menu_popup_at_widget.
  wxPoint pos = ScreenToClient(wxGetMousePosition());
  pos.y -= 2;
  PopupMenu(&menu, pos);
}

std::filesystem::path GameListPanel::GetSelectedPath() const {
  if (!list_) {
    return {};
  }
  int row = list_->GetSelectedRow();
  if (row < 0 || row >= static_cast<int>(visible_indices_.size())) {
    return {};
  }
  size_t idx = visible_indices_[row];
  if (idx >= entries_.size()) {
    return {};
  }
  return entries_[idx].path;
}

void GameListPanel::MoveSelection(int delta) {
  if (!list_ || visible_indices_.empty()) {
    return;
  }
  int row_count = static_cast<int>(visible_indices_.size());
  int row = list_->GetSelectedRow();
  if (row < 0) {
    row = (delta > 0) ? 0 : row_count - 1;
  } else {
    row = std::clamp(row + delta, 0, row_count - 1);
  }
  list_->SetFocus();
  list_->SelectRow(row);
  list_->EnsureVisible(list_->RowToItem(row));
  if (selection_changed_cb_) {
    selection_changed_cb_();
  }
}

void GameListPanel::ActivateSelected() {
  auto path = GetSelectedPath();
  if (!path.empty()) {
    LaunchOrPrompt(path);
  }
}

void GameListPanel::OnColumnHeaderClick(wxDataViewEvent& event) {
  int col = event.GetColumn();
  if (col == 1) {
    // Icon column doesn't sort.
    return;
  }
  if (sort_column_ == col) {
    sort_descending_ = !sort_descending_;
  } else {
    sort_column_ = col;
    sort_descending_ = false;
  }
  // Don't call SetSortOrder/UnsetAsSortKey here: on GTK that flips an
  // internal flag that makes wxDataViewCtrl auto-resort children on every
  // InsertLeaf using the model's default variant Compare, which clobbers
  // the order we set up in Repopulate (and falls back to item-id pointer
  // order for non-text columns, so reverse never reverses).
  Repopulate();
}

void GameListPanel::SortEntries() {
  if (sort_column_ < 0) {
    return;
  }
  auto cmp = [this](const Entry& a, const Entry& b) -> bool {
    auto less_then = [this](bool a_lt_b) {
      return sort_descending_ ? !a_lt_b : a_lt_b;
    };
    switch (sort_column_) {
      case 0: {  // Compat — ascending puts best (Playable) at top.
        auto sa = static_cast<uint8_t>(GetEntryCompatState(a));
        auto sb = static_cast<uint8_t>(GetEntryCompatState(b));
        if (sa != sb) {
          return less_then(sa > sb);
        }
        // Tiebreak by title so order within a bucket is stable and reverse
        // sort flips cleanly (no equal-element comparator ambiguity).
        auto la = ToLower(a.title_name);
        auto lb = ToLower(b.title_name);
        return less_then(la < lb);
      }
      case 2: {  // Title
        auto la = ToLower(a.title_name);
        auto lb = ToLower(b.title_name);
        return less_then(la < lb);
      }
      case 3: {  // Achievements — unlocked count, then total as tiebreak.
        if (a.achievements_unlocked != b.achievements_unlocked) {
          return less_then(a.achievements_unlocked < b.achievements_unlocked);
        }
        return less_then(a.achievements_total < b.achievements_total);
      }
      case 4: {  // Gamerscore — earned, then total as tiebreak.
        if (a.gamerscore_earned != b.gamerscore_earned) {
          return less_then(a.gamerscore_earned < b.gamerscore_earned);
        }
        return less_then(a.gamerscore_total < b.gamerscore_total);
      }
      case 5:  // Last Played
        return less_then(a.last_run_time < b.last_run_time);
      default:
        return false;
    }
  };
  std::stable_sort(entries_.begin(), entries_.end(), cmp);
}

void GameListPanel::Repopulate() {
  SortEntries();
  list_->DeleteAllItems();
  visible_indices_.clear();
  visible_indices_.reserve(entries_.size());

  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (!filter_lower_.empty()) {
      std::string title_lower = ToLower(e.title_name);
      std::string path_lower = ToLower(xe::path_to_utf8(e.path));
      if (title_lower.find(filter_lower_) == std::string::npos &&
          path_lower.find(filter_lower_) == std::string::npos) {
        continue;
      }
    }

    wxString base_title = e.title_name.empty()
                              ? _("File Corrupted")
                              : wxString::FromUTF8(e.title_name);
    wxString disc_suffix;
    if (e.discs.size() > 1) {
      disc_suffix = wxString::Format(_(" (%zu discs)"), e.discs.size());
    }

    wxVector<wxVariant> row;
    wxVariant compat_variant;
    compat_variant << CompatBall(GetEntryCompatState(e));
    row.push_back(compat_variant);
    wxVariant icon_variant;
    if (!profile_signed_in_) {
      icon_variant << LoggedOutPlaceholder();
    } else if (e.icon.IsOk()) {
      icon_variant << e.icon;
    } else {
      icon_variant << NotPlayedPlaceholder();
    }
    row.push_back(icon_variant);
    wxString title_markup =
        "<b><span size='x-large'>" + EscapeMarkup(base_title) + "</span></b>";
    if (!disc_suffix.empty()) {
      title_markup += EscapeMarkup(disc_suffix);
    }
    row.push_back(wxVariant(title_markup));
    std::string achievements_text;
    std::string gamerscore_text;
    if (e.achievements_total > 0) {
      achievements_text =
          fmt::format("{}/{}", e.achievements_unlocked, e.achievements_total);
      gamerscore_text =
          fmt::format("{}/{} G", e.gamerscore_earned, e.gamerscore_total);
    }
    row.push_back(wxVariant(wxString::FromUTF8(achievements_text)));
    row.push_back(wxVariant(wxString::FromUTF8(gamerscore_text)));
    row.push_back(
        wxVariant(wxString::FromUTF8(FormatLastPlayed(e.last_run_time))));
    list_->AppendItem(row);
    visible_indices_.push_back(i);
  }
}

}  // namespace app
}  // namespace xe
