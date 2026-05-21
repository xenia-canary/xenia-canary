/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_INSTALL_PROGRESS_DIALOG_WX_H_
#define XENIA_UI_INSTALL_PROGRESS_DIALOG_WX_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include <wx/dialog.h>
#include <wx/timer.h>

#include "xenia/emulator.h"

class wxButton;
class wxGauge;
class wxHyperlinkCtrl;
class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;

namespace xe {
namespace ui {

// Modal progress dialog used for content package installs. Each entry gets one
// row with icon / name / path / type / status / progress bar; a wxTimer polls
// the shared entry vector while the actual install thread runs in the
// background.
class InstallProgressDialog : public wxDialog {
 public:
  InstallProgressDialog(
      wxWindow* parent, const std::filesystem::path& content_root,
      std::shared_ptr<std::vector<Emulator::ContentInstallEntry>> entries);

  // Sets work to run on the UI event loop once the dialog is being shown
  // (typically: spawn the install thread). Without this, a fast install can
  // finish before ShowModal even renders the first frame.
  void StartWhenShown(std::function<void()> work) {
    kickoff_ = std::move(work);
  }

  ~InstallProgressDialog() override;

 private:
  struct EntryWidgets {
    wxStaticBitmap* icon = nullptr;
    wxStaticText* name = nullptr;
    wxStaticText* path_prefix = nullptr;
    wxHyperlinkCtrl* path_link = nullptr;
    wxStaticText* type = nullptr;
    wxStaticText* status = nullptr;
    wxGauge* progress = nullptr;
  };

  void Build();
  void Tick();
  void OnCancel();
  size_t EntryCount() const;
  bool IsEverythingDone() const;

  std::filesystem::path content_root_;
  std::shared_ptr<std::vector<Emulator::ContentInstallEntry>> content_entries_;

  wxScrolledWindow* list_panel_ = nullptr;
  wxButton* cancel_button_ = nullptr;
  wxButton* close_button_ = nullptr;
  wxTimer timer_;
  std::vector<EntryWidgets> rows_;
  std::function<void()> kickoff_;
  bool was_done_ = false;
  bool success_dialog_shown_ = false;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_INSTALL_PROGRESS_DIALOG_WX_H_
