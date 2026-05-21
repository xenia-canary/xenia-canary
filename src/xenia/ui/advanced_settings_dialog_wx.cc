/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/advanced_settings_dialog_wx.h"

#include <algorithm>
#include <filesystem>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tooltip.h>

#include "third_party/tomlplusplus/toml.hpp"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/config_helpers.h"

#if XE_PLATFORM_LINUX
#include <unistd.h>
#endif

namespace xe {
namespace app {

namespace {

#if XE_PLATFORM_LINUX
bool IsCommandAvailable(const std::string& command) {
  const char* path_env = std::getenv("PATH");
  if (!path_env || !*path_env) {
    return false;
  }
  std::string p(path_env);
  size_t start = 0;
  while (start <= p.size()) {
    size_t end = p.find(':', start);
    std::string dir = p.substr(start, end - start);
    if (!dir.empty()) {
      std::filesystem::path full = std::filesystem::path(dir) / command;
      if (std::filesystem::exists(full) && access(full.c_str(), X_OK) == 0) {
        return true;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return false;
}
#endif

std::string TrimWhitespace(std::string s) {
  size_t a = s.find_first_not_of(" \t\n\r");
  if (a == std::string::npos) {
    return {};
  }
  size_t b = s.find_last_not_of(" \t\n\r");
  return s.substr(a, b - a + 1);
}

bool IsIntCvar(cvar::IConfigVar* var) {
  return dynamic_cast<cvar::ConfigVar<int32_t>*>(var) ||
         dynamic_cast<cvar::ConfigVar<uint32_t>*>(var) ||
         dynamic_cast<cvar::ConfigVar<int64_t>*>(var) ||
         dynamic_cast<cvar::ConfigVar<uint64_t>*>(var);
}

std::string GetCurrentValue(cvar::IConfigVar* var) {
  if (auto* s = dynamic_cast<cvar::ConfigVar<std::string>*>(var)) {
    return s->GetTypedConfigValue();
  }
  if (auto* p = dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
    return xe::path_to_utf8(p->GetTypedConfigValue());
  }
  // Integer cvars shown as string dropdowns (e.g. user_language) display their
  // option name rather than the raw id.
  return ui::IntCvarValueToDisplayName(var->name(), var->config_value());
}

}  // namespace

AdvancedSettingsDialog::AdvancedSettingsDialog(wxWindow* parent,
                                               EmulatorWindow* emulator_window)
    : wxDialog(parent, wxID_ANY, _("Configuration Manager"), wxDefaultPosition,
               wxSize(900, 650), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      emulator_window_(emulator_window) {
  wxToolTip::SetAutoPop(30000);
  LoadConfigValues();
  Build();
  Centre();
}

AdvancedSettingsDialog::~AdvancedSettingsDialog() {
  // No getter on wxToolTip; restore the documented wx default.
  wxToolTip::SetAutoPop(5000);
}

void AdvancedSettingsDialog::SelectCategory(const std::string& category_name) {
  for (size_t i = 0; i < category_order_.size(); ++i) {
    if (category_order_[i] == category_name) {
      category_list_->SetSelection(static_cast<int>(i));
      settings_stack_->SetSelection(static_cast<size_t>(i));
      return;
    }
  }
}

void AdvancedSettingsDialog::LoadConfigValues() {
  config_vars_.clear();
  categories_.clear();
  category_order_.clear();
  has_unsaved_changes_ = false;
  if (!cvar::ConfigVars) {
    return;
  }

  // Reserve so VarInfo* pointers stored in categories_ stay valid as we push.
  size_t total = 0;
  for (const auto& [_, var] : *cvar::ConfigVars) {
    if (!var->is_transient() && var->category() != "Config") {
      ++total;
    }
  }
  config_vars_.reserve(total);

  for (const auto& [name, var] : *cvar::ConfigVars) {
    if (var->is_transient() || var->category() == "Config") {
      continue;
    }
    VarInfo info;
    info.var = var;
    info.name = var->name();
    info.description = var->description();
    info.category = var->category();
    info.current_value = GetCurrentValue(var);
    info.pending_value = info.current_value;
    config_vars_.push_back(std::move(info));
  }

  for (auto& info : config_vars_) {
    auto& bucket = categories_[info.category];
    if (bucket.empty()) {
      category_order_.push_back(info.category);
    }
    bucket.push_back(&info);
  }

  std::sort(category_order_.begin(), category_order_.end());
  for (auto& [_, vars] : categories_) {
    std::sort(vars.begin(), vars.end(), [](const VarInfo* a, const VarInfo* b) {
      return a->name < b->name;
    });
  }
}

void AdvancedSettingsDialog::Build() {
  auto* root = new wxBoxSizer(wxVERTICAL);

  auto* info_label = new wxStaticText(
      this, wxID_ANY,
      _("Manage emulator configuration. Changes are saved to config.toml; some "
        "settings need a relaunch to take effect."));
  info_label->Wrap(860);
  root->Add(info_label, 0, wxEXPAND | wxALL, 8);

  auto* split = new wxBoxSizer(wxHORIZONTAL);
  category_list_ =
      new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(170, -1));
  category_list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& evt) {
    int sel = evt.GetSelection();
    if (sel >= 0) {
      settings_stack_->SetSelection(static_cast<size_t>(sel));
    }
  });
  split->Add(category_list_, 0, wxEXPAND | wxALL, 4);

  settings_stack_ = new wxSimplebook(this, wxID_ANY);
  for (const auto& cat : category_order_) {
    category_list_->Append(wxString::FromUTF8(cat));
    auto* page = new wxScrolledWindow(settings_stack_, wxID_ANY);
    page->SetScrollRate(0, 16);
    BuildCategoryPage(page, cat, categories_[cat]);
    settings_stack_->AddPage(page, wxString::FromUTF8(cat));
  }
  if (!category_order_.empty()) {
    category_list_->SetSelection(0);
    settings_stack_->SetSelection(0);
  }
  split->Add(settings_stack_, 1, wxEXPAND | wxALL, 4);
  root->Add(split, 1, wxEXPAND);

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  auto* save = new wxButton(this, wxID_ANY, _("Save Changes"));
  save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    Save();
    if (!has_unsaved_changes_) {
      EndModal(wxID_OK);
    }
  });
  buttons->Add(save, 0, wxALL, 4);
  auto* discard = new wxButton(this, wxID_ANY, _("Discard Changes"));
  discard->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Discard(); });
  buttons->Add(discard, 0, wxALL, 4);
  auto* reset = new wxButton(this, wxID_ANY, _("Reset to Defaults"));
  reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoReset(); });
  buttons->Add(reset, 0, wxALL, 4);
  buttons->AddStretchSpacer(1);
  auto* close = new wxButton(this, wxID_CANCEL, _("Close"));
  buttons->Add(close, 0, wxALL, 4);
  root->Add(buttons, 0, wxEXPAND | wxALL, 4);

  SetSizer(root);
}

void AdvancedSettingsDialog::BuildCategoryPage(
    wxWindow* parent, const std::string& category,
    const std::vector<VarInfo*>& vars) {
  auto* grid = new wxFlexGridSizer(2, 4, 8);
  grid->AddGrowableCol(1, 1);

  for (auto* info : vars) {
    auto* label =
        new wxStaticText(parent, wxID_ANY, wxString::FromUTF8(info->name));
    if (!info->description.empty()) {
      label->SetToolTip(wxString::FromUTF8(info->description));
    }
    info->label = label;
    auto* editor = CreateEditor(parent, info);
    info->editor = editor;
    grid->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    grid->Add(editor, 0, wxEXPAND);
  }

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(grid, 1, wxEXPAND | wxALL, 8);
  parent->SetSizer(sizer);
  parent->FitInside();
}

wxWindow* AdvancedSettingsDialog::CreateEditor(wxWindow* parent,
                                               VarInfo* info) {
  std::string trimmed = TrimWhitespace(info->current_value);
  const auto& enums = ui::GetKnownEnumOptions();
  auto enum_it = enums.find(info->name);

  if (trimmed == "true" || trimmed == "false") {
    auto* check = new wxCheckBox(parent, wxID_ANY, "");
    check->SetValue(info->pending_value == "true");
    check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { OnAnyChanged(); });
#if XE_PLATFORM_LINUX
    if (info->name == "use_mangohud" && !IsCommandAvailable("mangohud")) {
      check->Disable();
      check->SetToolTip(_("MangoHUD not in PATH; install to enable."));
    } else if (info->name == "use_gamemode" &&
               !IsCommandAvailable("gamemoderun")) {
      check->Disable();
      check->SetToolTip(_("GameMode not in PATH; install to enable."));
    }
#endif
    return check;
  }

  if (enum_it != enums.end()) {
    auto* combo = new wxChoice(parent, wxID_ANY);
    int sel = -1;
    for (size_t i = 0; i < enum_it->second.size(); ++i) {
      combo->Append(wxString::FromUTF8(enum_it->second[i]));
      if (enum_it->second[i] == info->pending_value) {
        sel = static_cast<int>(i);
      }
    }
    if (sel >= 0) {
      combo->SetSelection(sel);
    }
    combo->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnAnyChanged(); });
    return combo;
  }

  if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(info->var)) {
    auto* container = new wxPanel(parent, wxID_ANY);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* text = new wxTextCtrl(container, wxID_ANY,
                                wxString::FromUTF8(info->pending_value));
    text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { OnAnyChanged(); });
    row->Add(text, 1, wxEXPAND | wxRIGHT, 4);
    auto* browse = new wxButton(container, wxID_ANY, _("Browse..."));
    browse->Bind(wxEVT_BUTTON, [this, text, info](wxCommandEvent&) {
      const auto* path_info = ui::GetCvarPathInfo(info->name);
      wxString picked;
      if (path_info && path_info->kind == ui::CvarPathKind::kFileOpen) {
        wxFileDialog dlg(
            this,
            wxString::Format(_("Select %s"), wxString::FromUTF8(info->name)),
            wxEmptyString, text->GetValue(),
            wxString::FromUTF8(path_info->wildcard),
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
          picked = dlg.GetPath();
        }
      } else if (path_info && path_info->kind == ui::CvarPathKind::kFileSave) {
        wxFileDialog dlg(
            this,
            wxString::Format(_("Select %s"), wxString::FromUTF8(info->name)),
            wxEmptyString, text->GetValue(),
            wxString::FromUTF8(path_info->wildcard),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() == wxID_OK) {
          picked = dlg.GetPath();
        }
      } else {
        wxDirDialog dlg(this,
                        wxString::Format(_("Select directory for %s"),
                                         wxString::FromUTF8(info->name)),
                        text->GetValue());
        if (dlg.ShowModal() == wxID_OK) {
          picked = dlg.GetPath();
        }
      }
      if (!picked.IsEmpty()) {
        text->ChangeValue(picked);
        OnAnyChanged();
      }
    });
    row->Add(browse, 0);
    container->SetSizer(row);
    return container;
  }

  if (IsIntCvar(info->var)) {
    auto* spin = new wxSpinCtrl(
        parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS,
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    try {
      spin->SetValue(std::stoi(info->pending_value));
    } catch (...) {
      spin->SetValue(0);
    }
    spin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { OnAnyChanged(); });
    spin->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { OnAnyChanged(); });
    return spin;
  }

  auto* text =
      new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(info->pending_value));
  text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { OnAnyChanged(); });
  return text;
}

std::string AdvancedSettingsDialog::ReadEditor(VarInfo* info) {
  if (auto* check = dynamic_cast<wxCheckBox*>(info->editor)) {
    return check->GetValue() ? "true" : "false";
  }
  if (auto* combo = dynamic_cast<wxChoice*>(info->editor)) {
    return std::string(combo->GetStringSelection().utf8_string());
  }
  if (auto* spin = dynamic_cast<wxSpinCtrl*>(info->editor)) {
    return std::to_string(spin->GetValue());
  }
  if (auto* text = dynamic_cast<wxTextCtrl*>(info->editor)) {
    return std::string(text->GetValue().utf8_string());
  }
  // Path container: find the wxTextCtrl child.
  if (auto* panel = dynamic_cast<wxPanel*>(info->editor)) {
    for (auto* c : panel->GetChildren()) {
      if (auto* text = dynamic_cast<wxTextCtrl*>(c)) {
        return std::string(text->GetValue().utf8_string());
      }
    }
  }
  return {};
}

void AdvancedSettingsDialog::OnAnyChanged() {
  for (auto& info : config_vars_) {
    if (!info.editor) {
      continue;
    }
    info.pending_value = ReadEditor(&info);
    info.is_modified = info.pending_value != info.current_value;
    UpdateLabelBold(&info);
  }
  has_unsaved_changes_ =
      std::any_of(config_vars_.begin(), config_vars_.end(),
                  [](const VarInfo& v) { return v.is_modified; });
  UpdateModifiedTitle();
}

void AdvancedSettingsDialog::UpdateLabelBold(VarInfo* info) {
  if (!info->label) {
    return;
  }
  auto font = info->label->GetFont();
  font.SetWeight(info->is_modified ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
  info->label->SetFont(font);
}

void AdvancedSettingsDialog::UpdateModifiedTitle() {
  SetTitle(has_unsaved_changes_ ? _("Configuration Manager *")
                                : _("Configuration Manager"));
}

void AdvancedSettingsDialog::Save() {
  if (!cvar::ConfigVars) {
    return;
  }
  bool any_applied = false;
  for (auto& info : config_vars_) {
    if (!info.is_modified) {
      continue;
    }
    auto* var = info.var;
    std::string trimmed = TrimWhitespace(info.pending_value);
    toml::table tbl;
    bool inserted = true;
    try {
      if (dynamic_cast<cvar::ConfigVar<bool>*>(var)) {
        tbl.insert(var->name(), trimmed == "true");
      } else if (dynamic_cast<cvar::ConfigVar<std::string>*>(var) ||
                 dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
        tbl.insert(var->name(), trimmed);
      } else if (IsIntCvar(var)) {
        // Dropdown int cvars hold an option name; convert back to its id.
        trimmed = ui::DisplayNameToIntCvarValue(var->name(), trimmed);
        int64_t val = (trimmed.size() > 2 && trimmed[0] == '0' &&
                       (trimmed[1] == 'x' || trimmed[1] == 'X'))
                          ? std::stoll(trimmed, nullptr, 16)
                          : std::stoll(trimmed);
        tbl.insert(var->name(), val);
      } else if (dynamic_cast<cvar::ConfigVar<float>*>(var) ||
                 dynamic_cast<cvar::ConfigVar<double>*>(var)) {
        tbl.insert(var->name(), std::stod(trimmed));
      } else {
        tbl.insert(var->name(), trimmed);
      }
    } catch (...) {
      XELOGE("AdvancedSettingsDialog: failed to parse {}: {}", info.name,
             info.pending_value);
      inserted = false;
    }
    if (!inserted) {
      continue;
    }
    if (auto* node = tbl.get(var->name())) {
      var->LoadConfigValue(node);
      var->ClearGameConfigValue();
      info.current_value = GetCurrentValue(var);
      info.pending_value = info.current_value;
      info.is_modified = false;
      UpdateLabelBold(&info);
      any_applied = true;
    }
  }
  if (any_applied) {
    config::SaveConfig();
    has_unsaved_changes_ = false;
    UpdateModifiedTitle();
  }
}

void AdvancedSettingsDialog::DoReset() {
  if (wxMessageBox(_("Reset all settings to their default values?"),
                   _("Reset to Defaults"), wxYES_NO | wxICON_QUESTION,
                   this) != wxYES) {
    return;
  }
  for (auto& info : config_vars_) {
    // Don't blow away which profile is logged into which slot.
    if (info.name.find("logged_profile_slot_") != std::string::npos) {
      continue;
    }
    info.var->ResetConfigValueToDefault();
    info.pending_value = GetCurrentValue(info.var);
    info.is_modified = info.pending_value != info.current_value;

    // Push the default into the editor so the user sees it. We don't touch
    // info.current_value — that still reflects the on-disk value, so the
    // bold-modified state correctly reports "changed" until Save runs.
    if (info.editor) {
      if (auto* check = dynamic_cast<wxCheckBox*>(info.editor)) {
        check->SetValue(info.pending_value == "true");
      } else if (auto* combo = dynamic_cast<wxChoice*>(info.editor)) {
        int idx = combo->FindString(wxString::FromUTF8(info.pending_value));
        if (idx >= 0) {
          combo->SetSelection(idx);
        }
      } else if (auto* spin = dynamic_cast<wxSpinCtrl*>(info.editor)) {
        try {
          spin->SetValue(std::stoi(info.pending_value));
        } catch (...) {
        }
      } else if (auto* text = dynamic_cast<wxTextCtrl*>(info.editor)) {
        text->ChangeValue(wxString::FromUTF8(info.pending_value));
      } else if (auto* panel = dynamic_cast<wxPanel*>(info.editor)) {
        for (auto* c : panel->GetChildren()) {
          if (auto* t = dynamic_cast<wxTextCtrl*>(c)) {
            t->ChangeValue(wxString::FromUTF8(info.pending_value));
            break;
          }
        }
      }
      UpdateLabelBold(&info);
    }
  }
  has_unsaved_changes_ =
      std::any_of(config_vars_.begin(), config_vars_.end(),
                  [](const VarInfo& v) { return v.is_modified; });
  UpdateModifiedTitle();
}

void AdvancedSettingsDialog::Discard() {
  if (!has_unsaved_changes_) {
    return;
  }
  if (wxMessageBox(_("Discard all unsaved changes?"), _("Discard Changes"),
                   wxYES_NO | wxICON_QUESTION, this) != wxYES) {
    return;
  }
  for (auto& info : config_vars_) {
    if (!info.editor) {
      continue;
    }
    info.pending_value = info.current_value;
    info.is_modified = false;
    if (auto* check = dynamic_cast<wxCheckBox*>(info.editor)) {
      check->SetValue(info.pending_value == "true");
    } else if (auto* combo = dynamic_cast<wxChoice*>(info.editor)) {
      int idx = combo->FindString(wxString::FromUTF8(info.pending_value));
      if (idx >= 0) {
        combo->SetSelection(idx);
      }
    } else if (auto* spin = dynamic_cast<wxSpinCtrl*>(info.editor)) {
      try {
        spin->SetValue(std::stoi(info.pending_value));
      } catch (...) {
      }
    } else if (auto* text = dynamic_cast<wxTextCtrl*>(info.editor)) {
      text->ChangeValue(wxString::FromUTF8(info.pending_value));
    } else if (auto* panel = dynamic_cast<wxPanel*>(info.editor)) {
      for (auto* c : panel->GetChildren()) {
        if (auto* t = dynamic_cast<wxTextCtrl*>(c)) {
          t->ChangeValue(wxString::FromUTF8(info.pending_value));
          break;
        }
      }
    }
    UpdateLabelBold(&info);
  }
  has_unsaved_changes_ = false;
  UpdateModifiedTitle();
}

}  // namespace app
}  // namespace xe
