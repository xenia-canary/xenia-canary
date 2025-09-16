/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_PATCHES_DIALOG_H_
#define XENIA_APP_PATCHES_DIALOG_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xenia/patcher/patch_db.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

class PatchesDialog final : public ui::ImGuiDialog {
 public:
  PatchesDialog(ui::ImGuiDrawer* imgui_drawer, EmulatorWindow* emulator_window);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  struct PatchInfo {
    uint32_t id;
    std::string name;
    std::string description;
    std::string author;
    bool is_enabled;
  };

  struct TitlePatchData {
    uint32_t title_id;
    std::string title_name;
    std::string filename;
    std::vector<uint64_t> hashes;
    std::vector<PatchInfo> patches;
    bool is_expanded = false;
  };

  void LoadPatchFiles();
  void SavePatchToggle(const std::string& filename, size_t patch_index,
                       bool new_value);
  bool UpdateSinglePatchEnabledLine(std::vector<std::string>& lines,
                                    size_t patch_index, bool new_value);

  EmulatorWindow* emulator_window_;
  std::vector<TitlePatchData> title_patches_;
  std::filesystem::path patches_directory_;
  std::unique_ptr<patcher::PatchDB> patch_db_;
  bool needs_reload_ = false;
  std::string filter_text_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_PATCHES_DIALOG_H_
