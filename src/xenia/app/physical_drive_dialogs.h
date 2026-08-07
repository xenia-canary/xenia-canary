/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_OMNIDRIVE_DIALOGS_H_
#define XENIA_APP_OMNIDRIVE_DIALOGS_H_

#include <filesystem>
#include <string>
#include <vector>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/vfs/devices/disc_omnidrive_device.h"

namespace xe {
namespace app {

void ShowSecuritySectorSanityWarningOnce();

class EmulatorWindow;

class PhysicalDriveSelectionDialog final : public ui::ImGuiDialog {
 public:
  PhysicalDriveSelectionDialog(ui::ImGuiDrawer* imgui_drawer,
                                EmulatorWindow* emulator_window,
                                std::vector<vfs::OmnidriveDriveInfo> drives)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        drives_(std::move(drives)) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  EmulatorWindow* emulator_window_;
  std::vector<vfs::OmnidriveDriveInfo> drives_;
  int selected_index_ = -1;
};

class PhysicalDriveDiscEjectedDialog final : public ui::ImGuiDialog {
 public:
  PhysicalDriveDiscEjectedDialog(ui::ImGuiDrawer* imgui_drawer,
                                 EmulatorWindow* emulator_window,
                                 std::filesystem::path drive_path)
      : ui::ImGuiDialog(imgui_drawer),
        emulator_window_(emulator_window),
        drive_path_(std::move(drive_path)) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  EmulatorWindow* emulator_window_;
  std::filesystem::path drive_path_;
  bool retry_failed_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_OMNIDRIVE_DIALOGS_H_
