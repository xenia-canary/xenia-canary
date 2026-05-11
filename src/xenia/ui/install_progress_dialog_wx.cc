/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/install_progress_dialog_wx.h"

#include <mutex>

#include <thread>

#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/system.h"
#include "xenia/ui/icon_decode.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {

namespace {

constexpr int kIconSize = 80;
constexpr int kTimerMs = 100;

}  // namespace

InstallProgressDialog::InstallProgressDialog(
    wxWindow* parent, const std::filesystem::path& content_root,
    std::shared_ptr<std::vector<Emulator::ContentInstallEntry>> entries)
    : wxDialog(parent, wxID_ANY, _("Installation Progress"), wxDefaultPosition,
               wxSize(640, 460), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      content_root_(content_root),
      content_entries_(std::move(entries)),
      timer_(this) {
  Build();
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Tick(); });
  Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {
    if (evt.IsShown() && kickoff_) {
      auto work = std::move(kickoff_);
      CallAfter([work = std::move(work)] { work(); });
    }
    evt.Skip();
  });
  timer_.Start(kTimerMs);
  Tick();
  Centre();
}

InstallProgressDialog::InstallProgressDialog(
    wxWindow* parent,
    std::shared_ptr<std::vector<Emulator::ZarchiveEntry>> entries)
    : wxDialog(parent, wxID_ANY, _("Zarchive Progress"), wxDefaultPosition,
               wxSize(640, 460), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      zarchive_entries_(std::move(entries)),
      is_zarchive_mode_(true),
      timer_(this) {
  Build();
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Tick(); });
  Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {
    if (evt.IsShown() && kickoff_) {
      auto work = std::move(kickoff_);
      CallAfter([work = std::move(work)] { work(); });
    }
    evt.Skip();
  });
  timer_.Start(kTimerMs);
  Tick();
  Centre();
}

InstallProgressDialog::~InstallProgressDialog() { timer_.Stop(); }

size_t InstallProgressDialog::EntryCount() const {
  if (is_zarchive_mode_) {
    return zarchive_entries_ ? zarchive_entries_->size() : 0;
  }
  return content_entries_ ? content_entries_->size() : 0;
}

void InstallProgressDialog::Build() {
  auto* root = new wxBoxSizer(wxVERTICAL);

  list_panel_ = new wxScrolledWindow(this, wxID_ANY);
  list_panel_->SetScrollRate(0, 16);
  auto* list_sizer = new wxBoxSizer(wxVERTICAL);

  rows_.resize(EntryCount());
  for (size_t i = 0; i < EntryCount(); ++i) {
    auto* row_box = new wxStaticBox(list_panel_, wxID_ANY, "");
    auto* row_sizer = new wxStaticBoxSizer(row_box, wxHORIZONTAL);
    auto& widgets = rows_[i];

    widgets.icon = new wxStaticBitmap(row_box, wxID_ANY, wxBitmapBundle());
    widgets.icon->SetMinSize(wxSize(kIconSize, kIconSize));
    if (is_zarchive_mode_) {
      widgets.icon->SetBitmap(
          DecodePngIcon((*zarchive_entries_)[i].icon_data_, kIconSize));
    } else {
      widgets.icon->SetBitmap(
          DecodePngIcon((*content_entries_)[i].icon_data_, kIconSize));
    }
    row_sizer->Add(widgets.icon, 0, wxALL, 6);

    auto* col = new wxBoxSizer(wxVERTICAL);
    widgets.name = new wxStaticText(row_box, wxID_ANY, "");
    widgets.path_prefix = new wxStaticText(row_box, wxID_ANY, "");
    widgets.path_link =
        new wxHyperlinkCtrl(row_box, wxID_ANY, " ", " ", wxDefaultPosition,
                            wxDefaultSize, wxHL_ALIGN_LEFT);
    widgets.path_link->Bind(wxEVT_HYPERLINK, [this, i](wxHyperlinkEvent&) {
      std::filesystem::path target;
      if (is_zarchive_mode_) {
        target = (*zarchive_entries_)[i].data_installation_path_.parent_path();
      } else {
        target = content_root_ / (*content_entries_)[i].data_installation_path_;
      }
      std::thread(xe::LaunchFileExplorer, std::move(target)).detach();
    });
    auto* path_row = new wxBoxSizer(wxHORIZONTAL);
    path_row->Add(widgets.path_prefix, 0, wxALIGN_CENTER_VERTICAL);
    path_row->Add(widgets.path_link, 1, wxALIGN_CENTER_VERTICAL);
    widgets.type = new wxStaticText(row_box, wxID_ANY, "");
    widgets.status = new wxStaticText(row_box, wxID_ANY, "");
    widgets.progress =
        new wxGauge(row_box, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 12),
                    wxGA_HORIZONTAL | wxGA_SMOOTH);
    col->Add(widgets.name, 0, wxBOTTOM, 2);
    col->Add(path_row, 0, wxEXPAND | wxBOTTOM, 2);
    col->Add(widgets.type, 0, wxBOTTOM, 2);
    col->Add(widgets.status, 0, wxBOTTOM, 2);
    col->Add(widgets.progress, 0, wxEXPAND | wxTOP, 4);
    row_sizer->Add(col, 1, wxEXPAND | wxALL, 4);

    list_sizer->Add(row_sizer, 0, wxEXPAND | wxALL, 4);
  }

  list_panel_->SetSizer(list_sizer);
  list_panel_->FitInside();
  root->Add(list_panel_, 1, wxEXPAND | wxALL, 6);

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  cancel_button_ = new wxButton(this, wxID_ANY, _("Cancel"));
  cancel_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCancel(); });
  buttons->Add(cancel_button_, 0, wxALL, 4);
  buttons->AddStretchSpacer(1);
  close_button_ = new wxButton(this, wxID_OK, _("Close"));
  close_button_->Disable();
  buttons->Add(close_button_, 0, wxALL, 4);
  root->Add(buttons, 0, wxEXPAND | wxALL, 4);

  SetSizer(root);
}

void InstallProgressDialog::Tick() {
  for (size_t i = 0; i < EntryCount(); ++i) {
    auto& widgets = rows_[i];
    std::string name;
    std::filesystem::path dest;
    Emulator::InstallState state{};
    X_STATUS result{};
    std::string error;
    uint64_t total = 0;
    uint64_t current = 0;
    bool cancelled = false;
    wxString type_text;

    if (is_zarchive_mode_) {
      auto& entry = (*zarchive_entries_)[i];
      {
        std::lock_guard<std::mutex> lock(*entry.mutex_);
        name = entry.name_;
        dest = entry.data_installation_path_;
        state = entry.installation_state_;
        result = entry.installation_result_;
        error = entry.installation_error_message_;
      }
      total = entry.content_size_.load();
      current = entry.currently_installed_size_.load();
      cancelled = entry.cancelled_.load();
      type_text = entry.operation_ == Emulator::ZarchiveOperation::Create
                      ? _("Operation: Create Archive")
                      : _("Operation: Extract Archive");
    } else {
      auto& entry = (*content_entries_)[i];
      XContentType content_type{};
      {
        std::lock_guard<std::mutex> lock(*entry.mutex_);
        name = entry.name_;
        dest = entry.data_installation_path_;
        state = entry.installation_state_;
        result = entry.installation_result_;
        error = entry.installation_error_message_;
        content_type = entry.content_type_;
      }
      total = entry.content_size_.load();
      current = entry.currently_installed_size_.load();
      cancelled = entry.cancelled_.load();
      if (content_type != XContentType::kInvalid) {
        auto it = XContentTypeMap.find(content_type);
        if (it != XContentTypeMap.end()) {
          type_text =
              wxString::Format(_("Content Type: %s"),
                               wxString::FromUTF8(std::string(it->second)));
        }
      }
    }

    widgets.name->SetLabel(
        wxString::Format(_("Name: %s"), wxString::FromUTF8(name)));
    wxString path_label = is_zarchive_mode_
                              ? (zarchive_entries_->at(i).operation_ ==
                                         Emulator::ZarchiveOperation::Create
                                     ? _("Output: ")
                                     : _("Extract To: "))
                              : _("Installation Path: ");
    std::string dest_str = xe::path_to_utf8(dest);
    widgets.path_prefix->SetLabel(path_label);
    widgets.path_link->SetLabel(wxString::FromUTF8(dest_str));
    widgets.path_link->SetURL(wxString::FromUTF8(dest_str));
    widgets.type->SetLabel(type_text);

    wxString status_text = wxString::Format(
        _("Status: %s"),
        wxString::FromUTF8(
            Emulator::installStateStringName[static_cast<uint8_t>(state)]));
    if (state == Emulator::InstallState::failed) {
      status_text += wxString::Format(_(" - %s (%08X)"),
                                      wxString::FromUTF8(error), result);
    }
    widgets.status->SetLabel(status_text);

    if (total > 0) {
      int percent =
          static_cast<int>(static_cast<double>(current) / total * 100.0);
      widgets.progress->SetValue(std::clamp(percent, 0, 100));
    } else {
      widgets.progress->SetValue(0);
    }
  }

  list_panel_->Layout();

  bool done = IsEverythingDone();
  cancel_button_->Enable(!done);
  close_button_->Enable(done);

  // One-shot success notification on the transition to done.
  if (done && !was_done_ && !success_dialog_shown_ && EntryCount() > 0) {
    success_dialog_shown_ = true;
    int success_count = 0;
    auto is_installed = [](auto& e) {
      std::lock_guard<std::mutex> lock(*e.mutex_);
      return e.installation_state_ == Emulator::InstallState::installed;
    };
    if (is_zarchive_mode_) {
      for (auto& e : *zarchive_entries_) {
        if (is_installed(e)) {
          ++success_count;
        }
      }
    } else {
      for (auto& e : *content_entries_) {
        if (is_installed(e)) {
          ++success_count;
        }
      }
    }
    if (success_count > 0) {
      wxMessageBox(
          is_zarchive_mode_
              ? wxString::Format(_("Successfully completed %d operation(s)."),
                                 success_count)
              : wxString::Format(_("Successfully installed %d package(s)."),
                                 success_count),
          _("Complete"), wxOK | wxICON_INFORMATION, this);
    }
  }
  was_done_ = done;
}

void InstallProgressDialog::OnCancel() {
  auto cancel_if_active = [](auto& e) {
    Emulator::InstallState state;
    {
      std::lock_guard<std::mutex> lock(*e.mutex_);
      state = e.installation_state_;
    }
    if (state == Emulator::InstallState::preparing ||
        state == Emulator::InstallState::pending ||
        state == Emulator::InstallState::installing) {
      e.cancelled_.store(true);
    }
  };
  if (is_zarchive_mode_) {
    for (auto& e : *zarchive_entries_) {
      cancel_if_active(e);
    }
  } else {
    for (auto& e : *content_entries_) {
      cancel_if_active(e);
    }
  }
  cancel_button_->Disable();
  cancel_button_->SetLabel(_("Cancelling..."));
}

bool InstallProgressDialog::IsEverythingDone() const {
  auto entry_done = [](auto& e) {
    if (e.cancelled_.load()) {
      return true;
    }
    Emulator::InstallState state;
    {
      std::lock_guard<std::mutex> lock(*e.mutex_);
      state = e.installation_state_;
    }
    if (state == Emulator::InstallState::failed) {
      return true;
    }
    uint64_t total = e.content_size_.load();
    uint64_t current = e.currently_installed_size_.load();
    if (total > 0 && current == total) {
      return true;
    }
    return false;
  };
  if (is_zarchive_mode_) {
    for (auto& e : *zarchive_entries_) {
      if (!entry_done(e)) {
        return false;
      }
    }
  } else {
    for (auto& e : *content_entries_) {
      if (!entry_done(e)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace ui
}  // namespace xe
