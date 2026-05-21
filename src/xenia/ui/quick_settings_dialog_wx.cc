/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/quick_settings_dialog_wx.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"
#include "xenia/config.h"
#include "xenia/ui/config_helpers.h"

namespace xe {
namespace app {

namespace {

// Strip surrounding quotes that toml::value uses for strings.
std::string StripQuotes(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    return v.substr(1, v.size() - 2);
  }
  return v;
}

cvar::IConfigVar* FindCvar(const std::string& name) {
  if (!cvar::ConfigVars) {
    return nullptr;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return nullptr;
  }
  return static_cast<cvar::IConfigVar*>(it->second);
}

std::string ReadCvarString(const std::string& name) {
  auto* var = FindCvar(name);
  if (!var) {
    return {};
  }
  return StripQuotes(var->config_value());
}

const std::string& VrrCvarName(const std::string& gpu) {
  static const std::string kVulkan = "vulkan_allow_present_mode_immediate";
  static const std::string kD3d12 =
      "d3d12_allow_variable_refresh_rate_and_tearing";
  static const std::string kMetal = "metal_allow_tearing";
  if (gpu == "vulkan") {
    return kVulkan;
  }
  if (gpu == "metal") {
    return kMetal;
  }
  return kD3d12;
}

}  // namespace

QuickSettingsDialog::QuickSettingsDialog(wxWindow* parent,
                                         EmulatorWindow* emulator_window)
    : wxDialog(parent, wxID_ANY, _("Emulator Settings"), wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      emulator_window_(emulator_window) {
  SetMinSize(wxSize(500, -1));
  // Reload the global config from disk so we don't show stale state from a
  // game-config dialog session.
  config::ReloadConfig();
  Build();
  LoadValuesFromCvars();
  Fit();
  Centre();
}

void QuickSettingsDialog::Build() {
  auto* main_sizer = new wxBoxSizer(wxVERTICAL);

  const auto& enum_options = ui::GetKnownEnumOptions();

  auto add_label = [&](wxStaticBox* box, const wxString& text) {
    auto* st = new wxStaticText(box, wxID_ANY, text);
    return st;
  };

  auto add_combo = [&](wxStaticBox* box, const char* cvar,
                       const std::vector<std::string>* items) {
    auto* combo = new wxChoice(box, wxID_ANY);
    if (items) {
      for (const auto& s : *items) {
        combo->Append(wxString::FromUTF8(s));
      }
    }
    combo->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnAnyChanged(); });
    Option opt;
    opt.cvar_name = cvar;
    opt.editor = combo;
    options_[cvar] = opt;
    return combo;
  };

  auto add_check = [&](wxStaticBox* box, const char* cvar) {
    auto* check = new wxCheckBox(box, wxID_ANY, "");
    check->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { OnAnyChanged(); });
    Option opt;
    opt.cvar_name = cvar;
    opt.editor = check;
    options_[cvar] = opt;
    return check;
  };

  auto add_spin = [&](wxStaticBox* box, const char* cvar, int min, int max) {
    auto* spin = new wxSpinCtrl(box, wxID_ANY, "", wxDefaultPosition,
                                wxDefaultSize, wxSP_ARROW_KEYS, min, max);
    spin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { OnAnyChanged(); });
    spin->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { OnAnyChanged(); });
    Option opt;
    opt.cvar_name = cvar;
    opt.editor = spin;
    options_[cvar] = opt;
    return spin;
  };

  auto remember_label = [&](const char* cvar, wxStaticText* st) {
    options_[cvar].label = st;
  };

  auto find_options = [&](const char* name) -> const std::vector<std::string>* {
    auto it = enum_options.find(name);
    return it == enum_options.end() ? nullptr : &it->second;
  };

  auto add_form_row = [](wxFlexGridSizer* grid, wxStaticText* label,
                         wxWindow* control) {
    grid->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    grid->Add(control, 0, wxEXPAND);
  };

  // ---- Graphics ----
  {
    auto* box = new wxStaticBox(this, wxID_ANY, _("Graphics"));
    auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 6, 8);
    grid->AddGrowableCol(1, 1);

    auto* gpu = add_combo(box, "gpu", find_options("gpu"));
    auto* gpu_label = add_label(box, _("Graphics Backend:"));
    remember_label("gpu", gpu_label);
    add_form_row(grid, gpu_label, gpu);

    auto* rt = add_combo(box, "render_target_path",
                         find_options("render_target_path"));
    auto* rt_label = add_label(box, _("Rendering:"));
    remember_label("render_target_path", rt_label);
    add_form_row(grid, rt_label, rt);

    auto* scale = new wxChoice(box, wxID_ANY);
    for (int i = 1; i <= 8; ++i) {
      scale->Append(i == 1 ? _("Native") : wxString::Format("%dx", i),
                    reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    scale->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnAnyChanged(); });
    {
      Option opt;
      opt.cvar_name = "draw_resolution_scale";
      opt.editor = scale;
      options_["draw_resolution_scale"] = opt;
    }
    auto* scale_label = add_label(box, _("Resolution Scale:"));
    remember_label("draw_resolution_scale", scale_label);
    add_form_row(grid, scale_label, scale);

#if XE_PLATFORM_MAC
    // Metal and the null backend don't implement render target path
    // selection or resolution scaling.
    rt->Disable();
    rt_label->Disable();
    scale->Disable();
    scale_label->Disable();
#endif

    auto* fps = add_spin(box, "framerate_limit", 0, 1000);
    auto* fps_label = add_label(box, _("Framerate Limit (0 = unlimited):"));
    remember_label("framerate_limit", fps_label);
    add_form_row(grid, fps_label, fps);

    // Refresh rate radios cover guest_display_refresh_cap + use_50Hz_mode.
    auto* refresh_panel = new wxPanel(box);
    auto* refresh_sizer = new wxBoxSizer(wxHORIZONTAL);
    refresh_uncapped_ =
        new wxRadioButton(refresh_panel, wxID_ANY, _("Uncapped"),
                          wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    refresh_50hz_ = new wxRadioButton(refresh_panel, wxID_ANY, _("50Hz"));
    refresh_60hz_ = new wxRadioButton(refresh_panel, wxID_ANY, _("60Hz"));
    refresh_sizer->Add(refresh_uncapped_, 0, wxRIGHT, 8);
    refresh_sizer->Add(refresh_50hz_, 0, wxRIGHT, 8);
    refresh_sizer->Add(refresh_60hz_, 0);
    refresh_panel->SetSizer(refresh_sizer);
    auto bind_radio = [this](wxRadioButton* rb) {
      rb->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { OnAnyChanged(); });
    };
    bind_radio(refresh_uncapped_);
    bind_radio(refresh_50hz_);
    bind_radio(refresh_60hz_);
    {
      Option opt;
      opt.cvar_name = "guest_refresh_rate";
      opt.editor = refresh_panel;
      options_["guest_refresh_rate"] = opt;
    }
    auto* refresh_label = add_label(box, _("Emulated Display Refresh Rate:"));
    remember_label("guest_refresh_rate", refresh_label);
    add_form_row(grid, refresh_label, refresh_panel);

    auto* vsync = add_check(box, "vsync");
    auto* vsync_label = add_label(box, _("VSync:"));
    remember_label("vsync", vsync_label);
    add_form_row(grid, vsync_label, vsync);

    auto* fs = add_check(box, "fullscreen");
    auto* fs_label = add_label(box, _("Full Screen:"));
    remember_label("fullscreen", fs_label);
    add_form_row(grid, fs_label, fs);

    auto* lb = add_check(box, "present_letterbox");
    auto* lb_label = add_label(box, _("Letterbox:"));
    remember_label("present_letterbox", lb_label);
    add_form_row(grid, lb_label, lb);

    box_sizer->Add(grid, 1, wxEXPAND | wxALL, 6);
    main_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 6);
  }

  // ---- Audio ----
  {
    auto* box = new wxStaticBox(this, wxID_ANY, _("Audio"));
    auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 6, 8);
    grid->AddGrowableCol(1, 1);

    auto* apu = add_combo(box, "apu", find_options("apu"));
    auto* apu_label = add_label(box, _("Audio Backend:"));
    remember_label("apu", apu_label);
    add_form_row(grid, apu_label, apu);

    auto* xma = add_combo(box, "xma_decoder", find_options("xma_decoder"));
    auto* xma_label = add_label(box, _("Audio Decoder:"));
    remember_label("xma_decoder", xma_label);
    add_form_row(grid, xma_label, xma);

    auto* xma_thread = add_check(box, "use_dedicated_xma_thread");
    auto* xma_thread_label = add_label(box, _("Dedicated Thread:"));
    remember_label("use_dedicated_xma_thread", xma_thread_label);
    add_form_row(grid, xma_thread_label, xma_thread);

    box_sizer->Add(grid, 1, wxEXPAND | wxALL, 6);
    main_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 6);
  }

  // ---- Other ----
  {
    auto* box = new wxStaticBox(this, wxID_ANY, _("Other"));
    auto* box_sizer = new wxStaticBoxSizer(box, wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 6, 8);
    grid->AddGrowableCol(1, 1);

    auto* license = new wxChoice(box, wxID_ANY);
    license->Append(_("None"),
                    reinterpret_cast<void*>(static_cast<intptr_t>(0)));
    license->Append(_("Full"),
                    reinterpret_cast<void*>(static_cast<intptr_t>(1)));
    license->Append(_("All"),
                    reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
    license->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnAnyChanged(); });
    {
      Option opt;
      opt.cvar_name = "license_mask";
      opt.editor = license;
      options_["license_mask"] = opt;
    }
    auto* license_label = add_label(box, _("License:"));
    remember_label("license_mask", license_label);
    add_form_row(grid, license_label, license);

    auto* discord = add_check(box, "discord");
    auto* discord_label = add_label(box, _("Discord Rich Presence:"));
    remember_label("discord", discord_label);
    add_form_row(grid, discord_label, discord);

#if XE_PLATFORM_WIN32
    // Other platforms always relaunch out-of-process (see RunTitle), so the
    // cvar has no effect there.
    auto* in_proc = add_check(box, "in_process_title_relaunch");
    auto* in_proc_label = add_label(box, _("In-Process Title Relaunch:"));
    remember_label("in_process_title_relaunch", in_proc_label);
    add_form_row(grid, in_proc_label, in_proc);
#endif

    auto* lang = add_combo(box, "user_language", find_options("user_language"));
    auto* lang_label = add_label(box, _("Language:"));
    remember_label("user_language", lang_label);
    add_form_row(grid, lang_label, lang);

    auto* country =
        add_combo(box, "user_country", find_options("user_country"));
    auto* country_label = add_label(box, _("Country:"));
    remember_label("user_country", country_label);
    add_form_row(grid, country_label, country);

    box_sizer->Add(grid, 1, wxEXPAND | wxALL, 6);
    main_sizer->Add(box_sizer, 0, wxEXPAND | wxALL, 6);
  }

  // ---- Buttons ----
  auto* button_row = new wxBoxSizer(wxHORIZONTAL);
  auto* advanced = new wxButton(this, wxID_ANY, _("Advanced..."));
  advanced->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnAdvanced(); });
  auto* reset = new wxButton(this, wxID_ANY, _("Reset to Default"));
  reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ResetToDefaults(); });
  auto* save = new wxButton(this, wxID_ANY, _("Save"));
  save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    Save();
    EndModal(wxID_OK);
  });
  auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
  cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (has_unsaved_changes_) {
      auto reply =
          wxMessageBox(_("You have unsaved changes. Discard them?"),
                       _("Unsaved Changes"), wxYES_NO | wxICON_QUESTION, this);
      if (reply != wxYES) {
        return;
      }
    }
    EndModal(wxID_CANCEL);
  });
  button_row->Add(advanced, 0, wxRIGHT, 6);
  button_row->Add(reset, 0, wxRIGHT, 6);
  button_row->AddStretchSpacer(1);
  button_row->Add(save, 0, wxRIGHT, 6);
  button_row->Add(cancel, 0);
  main_sizer->Add(button_row, 0, wxEXPAND | wxALL, 6);

  SetSizerAndFit(main_sizer);
}

void QuickSettingsDialog::LoadValuesFromCvars() {
  // Pull current cvar values into the editors.
  auto set_combo_text = [](wxChoice* combo, const std::string& text) {
    int idx = combo->FindString(wxString::FromUTF8(text));
    if (idx >= 0) {
      combo->SetSelection(idx);
    }
  };
  auto set_combo_int = [](wxChoice* combo, intptr_t value) {
    for (unsigned int i = 0; i < combo->GetCount(); ++i) {
      if (reinterpret_cast<intptr_t>(combo->GetClientData(i)) == value) {
        combo->SetSelection(i);
        return;
      }
    }
  };

  for (auto& [name, opt] : options_) {
    if (name == "draw_resolution_scale") {
      std::string v = ReadCvarString("draw_resolution_scale_x");
      opt.current_value = v;
      opt.pending_value = v;
      try {
        set_combo_int(static_cast<wxChoice*>(opt.editor), std::stoi(v));
      } catch (...) {
      }
      continue;
    }
    if (name == "vsync") {
      std::string gpu = ReadCvarString("gpu");
      std::string vrr = ReadCvarString(VrrCvarName(gpu));
      // VSync = !VRR
      std::string inverted = (vrr == "true") ? "false" : "true";
      opt.current_value = inverted;
      opt.pending_value = inverted;
      static_cast<wxCheckBox*>(opt.editor)->SetValue(inverted == "true");
      continue;
    }
    if (name == "guest_refresh_rate") {
      bool cap = ReadCvarString("guest_display_refresh_cap") == "true";
      bool hz50 = ReadCvarString("use_50Hz_mode") == "true";
      int selection = cap ? (hz50 ? 1 : 2) : 0;
      opt.current_value = std::to_string(selection);
      opt.pending_value = opt.current_value;
      refresh_uncapped_->SetValue(selection == 0);
      refresh_50hz_->SetValue(selection == 1);
      refresh_60hz_->SetValue(selection == 2);
      continue;
    }
    // Integer cvars shown as string dropdowns display their option name.
    std::string v = ui::IntCvarValueToDisplayName(name, ReadCvarString(name));
    opt.current_value = v;
    opt.pending_value = v;
    if (auto* combo = dynamic_cast<wxChoice*>(opt.editor)) {
      if (name == "license_mask") {
        try {
          set_combo_int(combo, std::stoi(v));
        } catch (...) {
        }
      } else {
        set_combo_text(combo, v);
      }
    } else if (auto* check = dynamic_cast<wxCheckBox*>(opt.editor)) {
      check->SetValue(v == "true");
    } else if (auto* spin = dynamic_cast<wxSpinCtrl*>(opt.editor)) {
      try {
        spin->SetValue(std::stoi(v));
      } catch (...) {
      }
    }
  }

  // Just-loaded matches the file, so no labels are bold.
  for (auto& [name, opt] : options_) {
    if (opt.label) {
      auto font = opt.label->GetFont();
      font.SetWeight(wxFONTWEIGHT_NORMAL);
      opt.label->SetFont(font);
    }
  }
  has_unsaved_changes_ = false;
}

std::string QuickSettingsDialog::ReadEditor(const Option& opt) {
  if (opt.cvar_name == "guest_refresh_rate") {
    if (refresh_50hz_->GetValue()) {
      return "1";
    }
    if (refresh_60hz_->GetValue()) {
      return "2";
    }
    return "0";
  }
  if (auto* combo = dynamic_cast<wxChoice*>(opt.editor)) {
    if (opt.cvar_name == "draw_resolution_scale" ||
        opt.cvar_name == "license_mask") {
      int sel = combo->GetSelection();
      if (sel < 0) {
        return "0";
      }
      auto v = reinterpret_cast<intptr_t>(combo->GetClientData(sel));
      return std::to_string(static_cast<int>(v));
    }
    return std::string(combo->GetStringSelection().utf8_string());
  }
  if (auto* check = dynamic_cast<wxCheckBox*>(opt.editor)) {
    return check->GetValue() ? "true" : "false";
  }
  if (auto* spin = dynamic_cast<wxSpinCtrl*>(opt.editor)) {
    return std::to_string(spin->GetValue());
  }
  return {};
}

void QuickSettingsDialog::UpdateLabelBold(Option& opt) {
  if (!opt.label) {
    return;
  }
  bool modified = (opt.pending_value != opt.current_value);
  auto font = opt.label->GetFont();
  font.SetWeight(modified ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
  opt.label->SetFont(font);
}

void QuickSettingsDialog::OnAnyChanged() {
  for (auto& [name, opt] : options_) {
    opt.pending_value = ReadEditor(opt);
    UpdateLabelBold(opt);
  }
  has_unsaved_changes_ = true;
}

void QuickSettingsDialog::Save() {
  if (!cvar::ConfigVars) {
    return;
  }
  auto apply = [](cvar::IConfigVar* var, const toml::node& tv) {
    var->LoadConfigValue(&tv);
    var->ClearGameConfigValue();
  };
  for (auto& [name, opt] : options_) {
    opt.pending_value = ReadEditor(opt);

    if (name == "draw_resolution_scale") {
      try {
        int v = std::stoi(opt.pending_value);
        toml::value tv(v);
        if (auto* x = FindCvar("draw_resolution_scale_x")) {
          apply(x, tv);
        }
        if (auto* y = FindCvar("draw_resolution_scale_y")) {
          apply(y, tv);
        }
      } catch (...) {
      }
      continue;
    }

    if (name == "vsync") {
      std::string gpu = ReadCvarString("gpu");
      if (auto* vrr = FindCvar(VrrCvarName(gpu))) {
        // VSync enabled => VRR disabled.
        apply(vrr, toml::value(opt.pending_value != "true"));
      }
      continue;
    }

    if (name == "guest_refresh_rate") {
      int sel = 0;
      try {
        sel = std::stoi(opt.pending_value);
      } catch (...) {
      }
      bool cap = (sel != 0);
      bool hz50 = (sel == 1);
      if (auto* v = FindCvar("guest_display_refresh_cap")) {
        apply(v, toml::value(cap));
      }
      if (auto* v = FindCvar("use_50Hz_mode")) {
        apply(v, toml::value(hz50));
      }
      continue;
    }

    auto* var = FindCvar(name);
    if (!var) {
      continue;
    }
    if (name == "framerate_limit") {
      try {
        apply(var, toml::value(
                       static_cast<int64_t>(std::stoull(opt.pending_value))));
      } catch (...) {
      }
    } else if (name == "license_mask") {
      try {
        apply(var, toml::value(std::stoi(opt.pending_value)));
      } catch (...) {
      }
    } else if (name == "fullscreen" || name == "present_letterbox" ||
               name == "discord" || name == "use_dedicated_xma_thread" ||
               name == "in_process_title_relaunch") {
      apply(var, toml::value(opt.pending_value == "true"));
    } else if (ui::FindIntCvarEnumOptions(name)) {
      // Dropdown int cvar: convert the selected option name to its id.
      try {
        apply(var,
              toml::value(static_cast<int64_t>(std::stoll(
                  ui::DisplayNameToIntCvarValue(name, opt.pending_value)))));
      } catch (...) {
      }
    } else {
      apply(var, toml::value(opt.pending_value));
    }
  }
  config::SaveConfig();
  has_unsaved_changes_ = false;
}

void QuickSettingsDialog::ResetToDefaults() {
  auto reply =
      wxMessageBox(_("Reset all settings to their default values?"),
                   _("Reset to Defaults"), wxYES_NO | wxICON_QUESTION, this);
  if (reply != wxYES) {
    return;
  }
  if (!cvar::ConfigVars) {
    return;
  }
  // Snapshot the on-disk current_value so we can show the bold-modified state
  // after replaying defaults into the UI.
  std::map<std::string, std::string> originals;
  for (auto& [name, opt] : options_) {
    originals[name] = opt.current_value;
  }
  for (auto& [name, var] : *cvar::ConfigVars) {
    // Don't clear which profile is logged into which slot — those are not
    // user-facing "settings", just persisted login state.
    if (name.find("logged_profile_slot_") != std::string::npos) {
      continue;
    }
    static_cast<cvar::IConfigVar*>(var)->ResetConfigValueToDefault();
  }
  LoadValuesFromCvars();
  for (auto& [name, opt] : options_) {
    opt.current_value = originals[name];
    UpdateLabelBold(opt);
  }
  has_unsaved_changes_ = true;
}

void QuickSettingsDialog::OnAdvanced() {
  if (has_unsaved_changes_) {
    auto reply = wxMessageBox(
        _("You have unsaved changes. Save them before opening advanced "
          "settings?"),
        _("Unsaved Changes"),
        wxYES_NO | wxCANCEL | wxICON_QUESTION | wxYES_DEFAULT, this);
    if (reply == wxCANCEL) {
      return;
    }
    if (reply == wxYES) {
      Save();
    }
  }
  EndModal(kReturnAdvancedRequested);
}

}  // namespace app
}  // namespace xe
