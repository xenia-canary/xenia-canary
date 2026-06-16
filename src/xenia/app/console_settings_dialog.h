/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_CONSOLE_SETTING_DIALOG_H_
#define XENIA_APP_CONSOLE_SETTING_DIALOG_H_

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/xbox.h"

#include "xenia/kernel/xconfig.h"

namespace xe {
namespace app {

class EmulatorWindow;

class ConsoleSettingsDialog final : public ui::ImGuiDialog {
 public:
  ConsoleSettingsDialog(ui::ImGuiDrawer* imgui_drawer,
                        EmulatorWindow& emulator_window,
                        kernel::XConfig* xconfig)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        xconfig_(xconfig),
        xconfig_data_(*xconfig->GetXConfig()) {
    const auto profiles = emulator_window.emulator()
                              ->kernel_state()
                              ->xam_state()
                              ->profile_manager()
                              ->GetAccounts();

    for (const auto& [xuid, profile] : *profiles) {
      profiles_.insert({xuid, profile.GetGamertagString()});
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void SaveConfig();

  EmulatorWindow& emulator_window_;
  kernel::XConfig* xconfig_ = nullptr;
  kernel::XConfigData xconfig_data_{};
  std::map<uint64_t, std::string> profiles_;

  // UI specific variables
  double save_confirmation_disappearance_ = 0.0;
};

}  // namespace app
}  // namespace xe

#endif
