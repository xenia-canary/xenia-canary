/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_ADVANCED_SETTINGS_DIALOG_WX_H_
#define XENIA_UI_ADVANCED_SETTINGS_DIALOG_WX_H_

#include <map>
#include <string>
#include <vector>

#include <wx/dialog.h>

#include "xenia/base/cvar.h"

class wxListBox;
class wxSimplebook;
class wxStaticText;
class wxWindow;

namespace xe {
namespace app {

class EmulatorWindow;

// Browse-everything cvar editor: a category list on the left, a stack of
// per-category scrolling pages on the right with one editor per cvar
// (checkbox / dropdown / spin / text / path-with-Browse).
class AdvancedSettingsDialog : public wxDialog {
 public:
  AdvancedSettingsDialog(wxWindow* parent, EmulatorWindow* emulator_window);
  ~AdvancedSettingsDialog() override;

  // Selects the named category in the list. Safe to call before/after Show.
  void SelectCategory(const std::string& category_name);

 private:
  struct VarInfo {
    cvar::IConfigVar* var = nullptr;
    std::string name;
    std::string description;
    std::string category;
    std::string current_value;
    std::string pending_value;
    bool is_modified = false;
    wxWindow* editor = nullptr;
    wxStaticText* label = nullptr;
  };

  void LoadConfigValues();
  void Build();
  void BuildCategoryPage(wxWindow* parent, const std::string& category,
                         const std::vector<VarInfo*>& vars);
  wxWindow* CreateEditor(wxWindow* parent, VarInfo* info);
  std::string ReadEditor(VarInfo* info);
  void OnAnyChanged();
  void Save();
  void DoReset();
  void Discard();
  void UpdateLabelBold(VarInfo* info);
  void UpdateModifiedTitle();

  EmulatorWindow* emulator_window_ = nullptr;
  std::vector<VarInfo> config_vars_;
  std::map<std::string, std::vector<VarInfo*>> categories_;
  std::vector<std::string> category_order_;

  wxListBox* category_list_ = nullptr;
  wxSimplebook* settings_stack_ = nullptr;
  bool has_unsaved_changes_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_ADVANCED_SETTINGS_DIALOG_WX_H_
