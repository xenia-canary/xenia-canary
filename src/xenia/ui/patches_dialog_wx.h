/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_PATCHES_DIALOG_WX_H_
#define XENIA_UI_PATCHES_DIALOG_WX_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <wx/dialog.h>
#include <wx/string.h>

#include "xenia/patcher/patch_db.h"

class wxCheckBox;
class wxScrolledWindow;
class wxSizeEvent;
class wxStaticText;

namespace xe {
namespace app {

class EmulatorWindow;

class PatchesDialog : public wxDialog {
 public:
  PatchesDialog(wxWindow* parent, EmulatorWindow* emulator_window,
                uint32_t title_id, patcher::BundledPatchFile bundled);

 private:
  struct PatchInfo {
    std::string name;
    std::string description;
    std::string author;
    bool is_enabled = false;
    size_t patch_index = 0;
    wxCheckBox* checkbox = nullptr;
  };

  void Build();
  void OnToggle(size_t patch_index, bool new_value);
  bool UpdateEnabledLine(size_t patch_index, bool new_value);
  void OnScrollSize(wxSizeEvent& event);
  void RewrapDescriptions();

  EmulatorWindow* emulator_window_;
  uint32_t title_id_;
  patcher::BundledPatchFile bundled_;
  std::filesystem::path storage_path_;
  std::vector<std::string> lines_;
  std::vector<PatchInfo> patches_;
  wxStaticText* info_label_ = nullptr;
  wxScrolledWindow* scroll_ = nullptr;
  std::vector<std::pair<wxStaticText*, wxString>> desc_labels_;
  int last_wrap_width_ = -1;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_PATCHES_DIALOG_WX_H_
