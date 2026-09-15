/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_TITLE_UPDATE_SELECTOR_DIALOG_H_
#define XENIA_APP_TITLE_UPDATE_SELECTOR_DIALOG_H_

#include "xenia/kernel/xam/content_manager.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/vfs/devices/stfs_xbox.h"

namespace xe {
namespace app {

class TitleUpdateSelectorDialog final : public xe::ui::ImGuiDialog {
 public:
  TitleUpdateSelectorDialog(
      xe::ui::ImGuiDrawer* imgui_drawer,
      kernel::xam::ContentManager* content_manager,
      const std::vector<kernel::xam::XCONTENT_DATA_INTERNAL>& title_updates,
      uint8_t* selected_entry_index);

  ~TitleUpdateSelectorDialog() = default;

 private:
  void OnDraw(ImGuiIO& io) override;
  void DrawEntry(ImGuiIO& io, uint8_t index);

  bool has_opened_ = false;
  uint8_t* selected_entry_index_ = nullptr;
  const std::vector<kernel::xam::XCONTENT_DATA_INTERNAL>& title_updates_;
  std::vector<kernel::xam::XContentContainerHeader> title_updates_metadata_;
};

}  // namespace app
}  // namespace xe

#endif
