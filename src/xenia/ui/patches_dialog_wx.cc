/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/patches_dialog_wx.h"

#include <fstream>
#include <regex>
#include <sstream>

#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"

#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"

namespace xe {
namespace app {

PatchesDialog::PatchesDialog(wxWindow* parent, EmulatorWindow* emulator_window,
                             uint32_t title_id,
                             patcher::BundledPatchFile bundled)
    : wxDialog(
          parent, wxID_ANY,
          wxString::Format(_("%s (%08X)"),
                           wxString::FromUTF8(bundled.entry.title_name.empty()
                                                  ? bundled.filename
                                                  : bundled.entry.title_name),
                           title_id),
          wxDefaultPosition, wxSize(640, 480),
          wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      emulator_window_(emulator_window),
      title_id_(title_id),
      bundled_(std::move(bundled)) {
  if (emulator_window_ && emulator_window_->emulator()) {
    storage_path_ = emulator_window_->emulator()->storage_root() / "patches" /
                    bundled_.filename;
  }
  Build();
}

void PatchesDialog::Build() {
  std::string source_text;
  if (!storage_path_.empty() && std::filesystem::exists(storage_path_)) {
    std::ifstream f(storage_path_, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    source_text = ss.str();
  } else {
    source_text = bundled_.toml_content;
  }
  {
    std::stringstream ss(source_text);
    std::string line;
    while (std::getline(ss, line)) {
      lines_.push_back(std::move(line));
    }
  }

  toml::table root;
  try {
    root = toml::parse(source_text);
  } catch (const std::exception& e) {
    XELOGE("PatchesDialog: failed to parse {}: {}", bundled_.filename,
           e.what());
  }

  auto* sizer = new wxBoxSizer(wxVERTICAL);

  auto* scroll = new wxScrolledWindow(this, wxID_ANY);
  scroll->SetScrollRate(0, 16);
  scroll_ = scroll;
  auto* scroll_sizer = new wxBoxSizer(wxVERTICAL);

  auto* arr = root["patch"].as_array();
  if (arr) {
    size_t patch_index = 0;
    for (const auto& node : *arr) {
      auto* tbl = node.as_table();
      if (!tbl) {
        ++patch_index;
        continue;
      }
      PatchInfo info;
      info.patch_index = patch_index++;
      if (auto v = (*tbl)["name"].value<std::string>()) info.name = *v;
      if (auto v = (*tbl)["desc"].value<std::string>()) info.description = *v;
      if (auto v = (*tbl)["author"].value<std::string>()) info.author = *v;
      if (auto v = (*tbl)["is_enabled"].value<bool>()) info.is_enabled = *v;
      wxString display_name;
      if (info.name.empty()) {
        display_name = wxString::Format(_("Patch #%zu"), info.patch_index + 1);
      } else {
        display_name = wxString::FromUTF8(info.name);
      }

      info.checkbox = new wxCheckBox(scroll, wxID_ANY, display_name);
      info.checkbox->SetValue(info.is_enabled);
      info.checkbox->Bind(
          wxEVT_CHECKBOX,
          [this, idx = info.patch_index, cb = info.checkbox](wxCommandEvent&) {
            OnToggle(idx, cb->GetValue());
          });
      scroll_sizer->Add(info.checkbox,
                        wxSizerFlags().Border(wxLEFT | wxTOP, 8));

      if (!info.description.empty() || !info.author.empty()) {
        wxString detail;
        if (!info.description.empty()) {
          detail = wxString::FromUTF8(info.description);
        }
        if (!info.author.empty()) {
          if (!detail.empty()) detail += wxT("\n");
          detail +=
              wxString::Format(_("by %s"), wxString::FromUTF8(info.author));
        }
        auto* lbl = new wxStaticText(scroll, wxID_ANY, detail);
        lbl->SetForegroundColour(*wxLIGHT_GREY);
        scroll_sizer->Add(lbl,
                          wxSizerFlags().Border(wxLEFT | wxRIGHT, 32).Expand());
        desc_labels_.emplace_back(lbl, detail);
      }

      patches_.push_back(std::move(info));
    }
  }

  scroll->SetSizer(scroll_sizer);
  scroll->Bind(wxEVT_SIZE, &PatchesDialog::OnScrollSize, this);
  sizer->Add(scroll, wxSizerFlags(1).Expand().Border(wxALL, 8));

  info_label_ = new wxStaticText(this, wxID_ANY,
                                 _("Toggles take effect on next launch."));
  sizer->Add(info_label_, wxSizerFlags().Border(wxLEFT | wxRIGHT, 12));

  auto* button_sizer = CreateButtonSizer(wxCLOSE);
  if (button_sizer) {
    sizer->Add(button_sizer, wxSizerFlags().Right().Border(wxALL, 8));
  }

  SetSizer(sizer);
}

bool PatchesDialog::UpdateEnabledLine(size_t patch_index, bool new_value) {
  static const std::regex patch_header(R"(^\s*\[\[patch\]\]\s*$)");
  static const std::regex section_header(R"(^\s*\[([^\[\]]+)\]\s*$)");
  static const std::regex is_enabled(R"(^\s*is_enabled\s*=\s*(.+))");
  static const std::regex comment_line(R"(^\s*#.*)");
  static const std::regex value_regex(
      R"((^\s*is_enabled\s*=\s*)(true|false)(.*)$)");

  std::vector<size_t> enabled_lines;
  bool in_patch = false;
  for (size_t i = 0; i < lines_.size(); ++i) {
    const std::string& line = lines_[i];
    if (std::regex_match(line, comment_line)) continue;
    if (std::regex_match(line, patch_header)) {
      in_patch = true;
      continue;
    }
    if (std::regex_match(line, section_header)) {
      in_patch = false;
      continue;
    }
    if (in_patch && std::regex_search(line, is_enabled)) {
      enabled_lines.push_back(i);
    }
  }
  if (patch_index >= enabled_lines.size()) {
    return false;
  }
  std::smatch m;
  std::string& line = lines_[enabled_lines[patch_index]];
  if (!std::regex_match(line, m, value_regex)) {
    return false;
  }
  line = m[1].str() + (new_value ? "true" : "false") + m[3].str();
  return true;
}

void PatchesDialog::OnToggle(size_t patch_index, bool new_value) {
  if (storage_path_.empty()) {
    XELOGE("PatchesDialog: no storage path; cannot save");
    return;
  }
  if (!UpdateEnabledLine(patch_index, new_value)) {
    XELOGE("PatchesDialog: failed to flip is_enabled for patch #{}",
           patch_index + 1);
    if (info_label_) {
      info_label_->SetLabel(_("Failed to update patch state."));
    }
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(storage_path_.parent_path(), ec);
  std::ofstream out(storage_path_, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    XELOGE("PatchesDialog: failed to write {}",
           xe::path_to_utf8(storage_path_));
    if (info_label_) {
      info_label_->SetLabel(_("Failed to save changes."));
    }
    return;
  }
  for (size_t i = 0; i < lines_.size(); ++i) {
    out << lines_[i];
    if (i + 1 < lines_.size()) out << "\n";
  }
  if (info_label_) {
    info_label_->SetLabel(_("Saved. Takes effect on next launch."));
  }
}

void PatchesDialog::OnScrollSize(wxSizeEvent& event) {
  event.Skip();
  RewrapDescriptions();
}

void PatchesDialog::RewrapDescriptions() {
  if (!scroll_ || desc_labels_.empty()) return;
  // 32px left + 32px right border on each label inside the scroll viewport.
  int width = scroll_->GetClientSize().GetWidth() - 64;
  if (width <= 0) return;
  if (width == last_wrap_width_) return;
  last_wrap_width_ = width;
  for (auto& [label, text] : desc_labels_) {
    label->SetLabel(text);
    label->Wrap(width);
  }
  if (auto* s = scroll_->GetSizer()) {
    s->Layout();
  }
  scroll_->FitInside();
}

}  // namespace app
}  // namespace xe
