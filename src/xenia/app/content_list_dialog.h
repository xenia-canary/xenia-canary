/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CONTENT_LIST_DIALOG_H_
#define XENIA_APP_CONTENT_LIST_DIALOG_H_

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {
class ContentManager;
}
}  // namespace kernel
}  // namespace xe

namespace xe {
namespace app {

class EmulatorWindow;

struct ContentListEntry {
  size_t index;
  uint32_t title_id;
  size_t size;
  bool is_package;
  std::u16string name;
  std::u16string title_name;
  std::filesystem::path path;
  std::unique_ptr<ui::ImmediateTexture> icon;
};

class ContentListDialog final : public ui::ImGuiDialog {
 public:
  ContentListDialog(ui::ImGuiDrawer* imgui_drawer,
                    EmulatorWindow& emulator_window,
                    kernel::xam::ContentManager* content_manager)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        content_manager_(content_manager) {
    Initialize();
  }

  ~ContentListDialog() {
    for (auto& type : enumerated_entries_) {
      for (auto& entry : type.second) {
        entry.icon.release();
      }
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnCategoryDraw(ImGuiIO& io, const XContentType type,
                      const std::vector<ContentListEntry>& entries);
  void OnEntryDraw(ImGuiIO& io, const XContentType type,
                   const ContentListEntry& entry);

 private:
  void Initialize();

  void ExecutePendingStart();
  void ExecutePendingDeletion();

  EmulatorWindow& emulator_window_;
  kernel::xam::ContentManager* content_manager_;

  uint32_t selected_entry_ = 0;

  std::map<XContentType, std::vector<ContentListEntry>> enumerated_entries_;
  std::optional<const ContentListEntry*> pending_boot_ = std::nullopt;
  std::optional<std::pair<XContentType, const ContentListEntry*>>
      pending_deletion_ = std::nullopt;

  const std::vector<std::pair<std::string, std::set<XContentType>>>
      typesToEnumerate = {
          {"Xbox 360 Games",
           {XContentType::kXbox360Title, XContentType::kInstalledGame}},
          {"XBLA Games", {XContentType::kArcadeTitle}},
          {"Demos", {XContentType::kGameDemo}},
          {"DLCs", {XContentType::kMarketplaceContent}},
          {"Title Updates", {XContentType::kInstaller}},
      };
};

}  // namespace app
}  // namespace xe

#endif
