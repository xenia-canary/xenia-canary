/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_TITLE_UPDATE_SELECTOR_DIALOG_H_
#define XENIA_KERNEL_XAM_UI_TITLE_UPDATE_SELECTOR_DIALOG_H_

#include "xenia/kernel/xam/xcontent/xcontent.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/vfs/devices/stfs_xbox.h"

namespace xe {
namespace kernel {
namespace xam {

class ContentManager;

namespace ui {

class TitleUpdateSelectorDialog final : public xe::ui::ImGuiDialog {
 public:
  TitleUpdateSelectorDialog(
      xe::ui::ImGuiDrawer* imgui_drawer, ContentManager* content_manager,
      const xex2_opt_execution_info* exec_info,
      const std::vector<XCONTENT_DATA_INTERNAL>& title_updates,
      const XLanguage user_language, uint8_t* selected_entry_index);

  ~TitleUpdateSelectorDialog() = default;

 private:
  struct TitleUpdateData {
    size_t index_;
    XCONTENT_DATA_INTERNAL lookup_data_;
    XContentContainerHeader container_header_;
  };

  void OnDraw(ImGuiIO& io) override;
  void DrawEntry(ImGuiIO& io, const TitleUpdateData& entry);

  bool has_opened_ = false;
  uint8_t* selected_entry_index_ = nullptr;
  const xex2_opt_execution_info* exec_info_ = nullptr;
  const XLanguage user_language_ = XLanguage::kEnglish;
  std::vector<TitleUpdateData> title_update_data_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
