/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_config_dialog_wx.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <wx/bitmap.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcmemory.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"

#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/config_helpers.h"

namespace xe {
namespace app {

namespace {

struct EditorBuild {
  wxWindow* editor;
  std::function<std::string()> get_value;
};

EditorBuild BuildEditor(wxWindow* parent, cvar::IConfigVar* var,
                        const std::string& current_value) {
  // Bool dropdown when value reads as a literal bool, regardless of type.
  if (current_value == "true" || current_value == "false") {
    auto* choice = new wxChoice(parent, wxID_ANY);
    choice->Append("true");
    choice->Append("false");
    choice->SetSelection(current_value == "true" ? 0 : 1);
    return {choice,
            [choice]() { return choice->GetStringSelection().utf8_string(); }};
  }

  if (var) {
    const auto& enums = xe::ui::GetKnownEnumOptions();
    auto it = enums.find(var->name());
    if (it != enums.end()) {
      auto* choice = new wxChoice(parent, wxID_ANY);
      int sel = -1;
      for (size_t i = 0; i < it->second.size(); ++i) {
        choice->Append(wxString::FromUTF8(it->second[i]));
        if (it->second[i] == current_value) {
          sel = static_cast<int>(i);
        }
      }
      if (sel >= 0) {
        choice->SetSelection(sel);
      }
      return {choice, [choice]() {
                return choice->GetStringSelection().utf8_string();
              }};
    }

    if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
      auto* container = new wxPanel(parent, wxID_ANY);
      auto* hsizer = new wxBoxSizer(wxHORIZONTAL);
      auto* text = new wxTextCtrl(container, wxID_ANY,
                                  wxString::FromUTF8(current_value));
      auto* browse = new wxButton(container, wxID_ANY, _("Browse..."));
      std::string name = var->name();
      browse->Bind(wxEVT_BUTTON, [container, text, name](wxCommandEvent&) {
        const auto* info = xe::ui::GetCvarPathInfo(name);
        wxString current = text->GetValue();
        wxString picked;
        if (info && info->kind != xe::ui::CvarPathKind::kDirectory) {
          long flags = info->kind == xe::ui::CvarPathKind::kFileSave
                           ? (wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
                           : (wxFD_OPEN | wxFD_FILE_MUST_EXIST);
          wxFileDialog dlg(
              container,
              wxString::Format(_("Select %s"), wxString::FromUTF8(name)), "",
              current, wxString::FromUTF8(info->wildcard), flags);
          if (dlg.ShowModal() == wxID_OK) {
            picked = dlg.GetPath();
          }
        } else {
          wxDirDialog dlg(container,
                          wxString::Format(_("Select directory for %s"),
                                           wxString::FromUTF8(name)),
                          current);
          if (dlg.ShowModal() == wxID_OK) {
            picked = dlg.GetPath();
          }
        }
        if (!picked.IsEmpty()) {
          text->SetValue(picked);
        }
      });
      hsizer->Add(text, wxSizerFlags(1).CenterVertical());
      hsizer->Add(browse, wxSizerFlags().CenterVertical().Border(wxLEFT, 4));
      container->SetSizer(hsizer);
      return {container, [text]() { return text->GetValue().utf8_string(); }};
    }

    if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var) ||
        dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
      auto* spin = new wxSpinCtrl(parent, wxID_ANY);
      spin->SetRange(std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::max());
      try {
        spin->SetValue(std::stoi(current_value));
      } catch (...) {
        spin->SetValue(0);
      }
      return {spin, [spin]() { return std::to_string(spin->GetValue()); }};
    }
  }

  auto* text =
      new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(current_value));
  return {text, [text]() { return text->GetValue().utf8_string(); }};
}

wxBitmap MakeCancelBitmap(int size) {
  wxImage img(size, size);
  img.InitAlpha();
  std::memset(img.GetAlpha(), 0,
              static_cast<size_t>(img.GetWidth()) * img.GetHeight());
  wxBitmap bmp(img);
  wxMemoryDC dc(bmp);
  dc.SetBackground(wxBrush(wxColour(0, 0, 0, wxALPHA_TRANSPARENT)));
  std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
  if (gc) {
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetBrush(wxBrush(wxColour(160, 160, 160)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawEllipse(0, 0, size, size);
    gc->SetPen(wxPen(*wxWHITE, std::max(2, size / 8)));
    double pad = size * 0.3;
    gc->StrokeLine(pad, pad, size - pad, size - pad);
    gc->StrokeLine(size - pad, pad, pad, size - pad);
  }
  dc.SelectObject(wxNullBitmap);
  return bmp;
}

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Modal cvar picker with a live filter on top, similar to the game list
// filter bar. Returns the selected name, or empty on cancel.
std::string PickCvarWithFilter(wxWindow* parent,
                               const std::vector<std::string>& names,
                               const std::vector<std::string>& display) {
  wxDialog dlg(parent, wxID_ANY, _("Add Override"), wxDefaultPosition,
               wxSize(520, 420), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  auto* search = new wxSearchCtrl(&dlg, wxID_ANY);
  search->ShowCancelButton(true);
  search->SetDescriptiveText(_("Filter cvars"));
  sizer->Add(search, wxSizerFlags().Expand().Border(wxALL, 8));

  wxArrayString choices;
  for (const auto& d : display) {
    choices.Add(wxString::FromUTF8(d));
  }
  auto* list = new wxListBox(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             choices, wxLB_SINGLE);
  sizer->Add(list, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT, 8));

  auto* button_sizer = dlg.CreateButtonSizer(wxOK | wxCANCEL);
  if (button_sizer) {
    sizer->Add(button_sizer, wxSizerFlags().Right().Border(wxALL, 8));
  }
  dlg.SetSizer(sizer);

  std::vector<int> visible;
  visible.reserve(names.size());
  for (int i = 0; i < static_cast<int>(names.size()); ++i) {
    visible.push_back(i);
  }

  auto refilter = [&](const std::string& filter) {
    list->Clear();
    visible.clear();
    std::string f = ToLowerAscii(filter);
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
      if (!f.empty() && ToLowerAscii(display[i]).find(f) == std::string::npos) {
        continue;
      }
      list->Append(wxString::FromUTF8(display[i]));
      visible.push_back(i);
    }
    if (!visible.empty()) {
      list->SetSelection(0);
    }
  };

  search->Bind(wxEVT_TEXT, [&](wxCommandEvent&) {
    refilter(search->GetValue().utf8_string());
  });
  search->Bind(wxEVT_SEARCH_CANCEL, [&](wxCommandEvent&) { search->Clear(); });
  list->Bind(wxEVT_LISTBOX_DCLICK,
             [&](wxCommandEvent&) { dlg.EndModal(wxID_OK); });

  search->SetFocus();
  if (dlg.ShowModal() != wxID_OK) {
    return {};
  }
  int sel = list->GetSelection();
  if (sel < 0 || sel >= static_cast<int>(visible.size())) {
    return {};
  }
  return names[visible[sel]];
}

std::string StripTomlQuotes(std::string value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

void InsertTypedValue(toml::table& dest, const std::string& key,
                      const std::string& value_str) {
  if (value_str == "true" || value_str == "false") {
    dest.insert_or_assign(key, value_str == "true");
    return;
  }
  char* end = nullptr;
  long long iv = std::strtoll(value_str.c_str(), &end, 10);
  if (end != value_str.c_str() && *end == '\0') {
    dest.insert_or_assign(key, iv);
    return;
  }
  end = nullptr;
  double dv = std::strtod(value_str.c_str(), &end);
  if (end != value_str.c_str() && *end == '\0') {
    dest.insert_or_assign(key, dv);
    return;
  }
  dest.insert_or_assign(key, value_str);
}

}  // namespace

GameConfigDialog::GameConfigDialog(wxWindow* parent,
                                   EmulatorWindow* emulator_window,
                                   uint32_t title_id, std::string game_title)
    : wxDialog(
          parent, wxID_ANY,
          wxString::Format(
              _("%s (%08X) — Config Overrides"),
              wxString::FromUTF8(game_title.empty() ? std::to_string(title_id)
                                                    : game_title),
              title_id),
          wxDefaultPosition, wxSize(720, 500),
          wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      emulator_window_(emulator_window),
      title_id_(title_id),
      game_title_(std::move(game_title)) {
  Build();
  LoadOverrides();
}

void GameConfigDialog::Build() {
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  scroll_ = new wxScrolledWindow(this, wxID_ANY);
  scroll_->SetScrollRate(0, 16);
  rows_sizer_ = new wxBoxSizer(wxVERTICAL);
  scroll_->SetSizer(rows_sizer_);
  sizer->Add(scroll_, wxSizerFlags(1).Expand().Border(wxALL, 8));

  auto* button_row = new wxBoxSizer(wxHORIZONTAL);
  auto* add_btn = new wxButton(this, wxID_ANY, _("Add..."));
  add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnAdd(); });
  button_row->Add(add_btn);
  button_row->AddStretchSpacer(1);

  auto* save_btn = new wxButton(this, wxID_OK, _("Save"));
  auto* cancel_btn = new wxButton(this, wxID_CANCEL, _("Cancel"));
  save_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    SaveOverrides();
    EndModal(wxID_OK);
  });
  button_row->Add(save_btn, wxSizerFlags().Border(wxRIGHT, 4));
  button_row->Add(cancel_btn);
  sizer->Add(button_row,
             wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

  SetSizer(sizer);
}

void GameConfigDialog::AddRow(const std::string& name,
                              const std::string& value) {
  auto* row = new Row;
  row->name = name;
  row->sizer = new wxBoxSizer(wxHORIZONTAL);
  auto* label = new wxStaticText(scroll_, wxID_ANY, wxString::FromUTF8(name),
                                 wxDefaultPosition, wxSize(280, -1));
  auto* var = cvar::ConfigVars ? (*cvar::ConfigVars)[name] : nullptr;
  EditorBuild built = BuildEditor(scroll_, var, value);
  row->editor = built.editor;
  row->get_value = std::move(built.get_value);
  row->editor->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { dirty_ = true; });
  row->editor->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { dirty_ = true; });
  row->editor->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { dirty_ = true; });
  static const wxBitmap kCancelBitmap = MakeCancelBitmap(16);
  auto* del_btn =
      new wxBitmapButton(scroll_, wxID_ANY, kCancelBitmap, wxDefaultPosition,
                         wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT);
  del_btn->SetToolTip(_("Remove this override"));
  del_btn->Bind(wxEVT_BUTTON, [this, row](wxCommandEvent&) { RemoveRow(row); });
  row->sizer->Add(label, wxSizerFlags().CenterVertical().Border(wxALL, 4));
  row->sizer->Add(row->editor,
                  wxSizerFlags(1).CenterVertical().Border(wxALL, 4));
  row->sizer->Add(del_btn, wxSizerFlags().CenterVertical().Border(wxALL, 4));
  rows_sizer_->Add(row->sizer, wxSizerFlags().Expand());
  rows_.push_back(row);
  scroll_->FitInside();
  scroll_->Layout();
}

void GameConfigDialog::RemoveRow(Row* row) {
  auto it = std::find(rows_.begin(), rows_.end(), row);
  if (it == rows_.end()) {
    return;
  }
  rows_sizer_->Detach(row->sizer);
  row->sizer->Clear(true);
  delete row->sizer;
  delete row;
  rows_.erase(it);
  dirty_ = true;
  scroll_->FitInside();
  scroll_->Layout();
}

void GameConfigDialog::LoadOverrides() {
  for (auto* row : rows_) {
    rows_sizer_->Detach(row->sizer);
    row->sizer->Clear(true);
    delete row->sizer;
    delete row;
  }
  rows_.clear();

  toml::table table;
  try {
    table = config::LoadGameConfig(title_id_);
  } catch (const std::exception& e) {
    XELOGE("GameConfigDialog: failed to load config: {}", e.what());
    return;
  }
  if (!cvar::ConfigVars) {
    return;
  }
  std::map<std::string, std::string> rows;
  for (auto& [name, var] : *cvar::ConfigVars) {
    auto* config_var = static_cast<cvar::IConfigVar*>(var);
    auto path = toml::path(config_var->category() + "." + config_var->name());
    auto node = table.at_path(path);
    if (!node) {
      continue;
    }
    void* saved = config_var->SaveConfigValueState();
    config_var->LoadConfigValue(node.node());
    std::string value = StripTomlQuotes(config_var->config_value());
    config_var->RestoreConfigValueState(saved);
    rows.emplace(config_var->name(), value);
  }
  for (const auto& [k, v] : rows) {
    AddRow(k, v);
  }
  dirty_ = false;
}

void GameConfigDialog::SaveOverrides() {
  toml::table out;
  std::map<std::string, toml::table> by_category;
  for (auto* row : rows_) {
    std::string value = row->get_value ? row->get_value() : std::string();
    if (row->name.empty()) {
      continue;
    }
    auto* var = cvar::ConfigVars ? (*cvar::ConfigVars)[row->name] : nullptr;
    if (!var || var->is_transient()) {
      continue;
    }
    InsertTypedValue(by_category[var->category()], row->name, value);
  }
  for (auto& [cat, tbl] : by_category) {
    out.insert_or_assign(cat, tbl);
  }
  try {
    config::SaveGameConfig(title_id_, out);
  } catch (const std::exception& e) {
    wxMessageBox(
        wxString::Format(_("Failed to save: %s"), wxString::FromUTF8(e.what())),
        _("Error"), wxOK | wxICON_ERROR, this);
    return;
  }
  dirty_ = false;
}

void GameConfigDialog::OnAdd() {
  if (!cvar::ConfigVars) {
    return;
  }
  std::set<std::string> existing;
  for (auto* row : rows_) {
    existing.insert(row->name);
  }

  std::vector<std::string> names;
  std::vector<std::string> display;
  for (auto& [name, var] : *cvar::ConfigVars) {
    if (var->is_transient()) {
      continue;
    }
    if (existing.count(name)) {
      continue;
    }
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  display.reserve(names.size());
  for (const auto& n : names) {
    auto* var = (*cvar::ConfigVars)[n];
    display.push_back(fmt::format("{} ({})", n, var->category()));
  }
  if (names.empty()) {
    wxMessageBox(_("All cvars are already overridden."), _("Add Override"),
                 wxOK | wxICON_INFORMATION, this);
    return;
  }
  std::string name = PickCvarWithFilter(this, names, display);
  if (name.empty()) {
    return;
  }
  auto* var = (*cvar::ConfigVars)[name];
  std::string default_value = StripTomlQuotes(var->config_value());
  AddRow(name, default_value);
  dirty_ = true;
}

}  // namespace app
}  // namespace xe
