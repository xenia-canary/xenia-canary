/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_EMULATOR_WINDOW_H_
#define XENIA_APP_EMULATOR_WINDOW_H_

#include <memory>
#include <string>

#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/ui/imgui_confirm_dialog.h"
#include "xenia/ui/imgui_context_menu.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_performance_dialog.h"
#include "xenia/ui/imgui_postprocessing_dialog.h"
#include "xenia/ui/imgui_xmp_dialog.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/menu_item.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/profile_dialogs.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
namespace app {

class GameListPanel;
struct WxToolbarState;

class EmulatorWindow {
 public:
  using steady_clock = std::chrono::steady_clock;  // stdlib steady clock

  enum : size_t {
    // The UI is on top of the game and is open in special cases, so
    // lowest-priority.
    kZOrderHidInput,
    kZOrderImGui,
    kZOrderProfiler,
    // Emulator window controls are expected to be always accessible by the
    // user, so highest-priority.
    kZOrderEmulatorWindowInput,
  };

  virtual ~EmulatorWindow();

  static std::unique_ptr<EmulatorWindow> Create(
      Emulator* emulator, ui::WindowedAppContext& app_context, uint32_t width,
      uint32_t height);

  std::unique_ptr<xe::threading::Thread> Gamepad_HotKeys_Listener;

  static constexpr int64_t diff_in_ms(
      const steady_clock::time_point t1,
      const steady_clock::time_point t2) noexcept {
    using ms = std::chrono::milliseconds;
    return std::chrono::duration_cast<ms>(t1 - t2).count();
  }

  steady_clock::time_point last_mouse_up = steady_clock::now();
  steady_clock::time_point last_mouse_down = steady_clock::now();

  Emulator* emulator() const { return emulator_; }
  ui::WindowedAppContext& app_context() const { return app_context_; }
  ui::Window* window() const { return window_.get(); }
  ui::ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }

  ui::Presenter* GetGraphicsSystemPresenter() const;
  void SetupGraphicsSystemPresenterPainting();
  void ShutdownGraphicsSystemPresenterPainting();

  void OnEmulatorInitialized();

  void LaunchTitleInNewProcess(const std::filesystem::path& path_to_file);
  xe::X_STATUS RunTitle(const std::filesystem::path& path_to_file);
  void UpdateTitle();

  void SetFullscreen(bool fullscreen);
  void ToggleFullscreen();
  void SetInitializingShaderStorage(bool initializing);

  void TakeScreenshot();
  void ExportScreenshot(const xe::ui::RawImage& image);
  void SaveImage(const std::filesystem::path& path,
                 const xe::ui::RawImage& image);

  void ToggleProfilesConfigDialog();
  void ToggleXMPConfigDialog();
  void ToggleConfigDialog();
  void OpenConfigDialog(const std::string& category = "");
  void ToggleControllerVibration();
  void SetHotkeysState(bool enabled) { disable_hotkeys_ = !enabled; }
  void FileOpen();

  // Helper methods for updating cvars from config dialogs.
  void UpdateAntiAliasingCvar(gpu::CommandProcessor::SwapPostEffect effect);
  void UpdateScalingAndSharpeningCvar(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  void UpdateFsrSharpnessCvar(float value);
  void UpdateFsrMaxUpsamplingPassesCvar(uint32_t value);
  void UpdateCasSharpnessCvar(float value);
  void UpdateDitherCvar(bool value);

  // Types of button functions for hotkeys.
  enum class ButtonFunctions {
    ToggleFullscreen,
    CpuTimeScalarSetHalf,
    CpuTimeScalarSetDouble,
    CpuTimeScalarReset,
    ClearGPUCache,
    ToggleControllerVibration,
    ClearMemoryPageState,
    ReadbackResolve,
    ToggleLogging,
    Unknown
  };

  class ControllerHotKey {
   public:
    // If true the hotkey can be activated while a title is running, otherwise
    // false.
    bool title_passthru;

    // If true vibrate the controller after activating the hotkey, otherwise
    // false.
    bool rumble;
    std::string pretty;
    ButtonFunctions function;

    ControllerHotKey(ButtonFunctions fn = ButtonFunctions::Unknown,
                     std::string pretty = "", bool rumble = false,
                     bool active = true) {
      function = fn;
      this->pretty = pretty;
      title_passthru = active;
      this->rumble = rumble;
    }
  };

  // For comparisons, use GetSwapPostEffectForCvarValue instead as the default
  // fallback may be used for multiple values.
  static const char* GetCvarValueForSwapPostEffect(
      gpu::CommandProcessor::SwapPostEffect effect);
  static gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
      const std::string& cvar_value);
  // For comparisons, use GetGuestOutputPaintEffectForCvarValue instead as the
  // default fallback may be used for multiple values.
  static const char* GetCvarValueForGuestOutputPaintEffect(
      ui::Presenter::GuestOutputPaintConfig::Effect effect);
  static ui::Presenter::GuestOutputPaintConfig::Effect
  GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value);
  static ui::Presenter::GuestOutputPaintConfig
  GetGuestOutputPaintConfigForCvars();
  void ApplyDisplayConfigForCvars();

 private:
  class EmulatorWindowListener final : public ui::WindowListener,
                                       public ui::WindowInputListener {
   public:
    explicit EmulatorWindowListener(EmulatorWindow& emulator_window)
        : emulator_window_(emulator_window) {}

    void OnClosing(ui::UIEvent& e) override;
    void OnFileDrop(ui::FileDropEvent& e) override;
    void OnResize(ui::UISetupEvent& e) override;

    void OnKeyDown(ui::KeyEvent& e) override;

    void OnMouseDown(ui::MouseEvent& e) override;
    void OnMouseUp(ui::MouseEvent& e) override;
    void OnMouseDoubleClick(ui::MouseEvent& e) override;
    void OnUsbDeviceChanged(bool is_arrival) override;

   private:
    EmulatorWindow& emulator_window_;
  };

  explicit EmulatorWindow(Emulator* emulator,
                          ui::WindowedAppContext& app_context, uint32_t width,
                          uint32_t height);

  bool Initialize();

  void OnKeyDown(ui::KeyEvent& e);
  void OnMouseDown(const ui::MouseEvent& e);
  void OnMouseDoubleClick(const ui::MouseEvent& e);
  void FileDrop(const std::filesystem::path& filename);
  void OnMouseUp(const ui::MouseEvent& e);
  void FileClose();
  void InstallContent();
  void ShowContentDirectory();
  void CpuTimeScalarReset();
  void CpuTimeScalarSetHalf();
  void CpuTimeScalarSetDouble();
  void CpuBreakIntoDebugger();
  void CpuBreakIntoHostDebugger();
  void GpuTraceFrame();
  void GpuClearCaches();
  void ToggleDisplayConfigDialog();
  void TogglePerformanceTuningDialog();
  void ToggleContextMenu(bool use_cursor_position = true);
  void ShowCompatibility();
  void ShowFAQ();
  void ShowBuildCommit();
  void ShowAbout();

  EmulatorWindow::ControllerHotKey ProcessControllerHotkey(int buttons);
  void VibrateController(xe::hid::InputSystem* input_sys, uint32_t user_index,
                         bool vibrate = true);
  void GamepadHotKeys();
  void ToggleGPUSetting(gpu::GPUSetting setting);
  void CycleReadbackResolve();

  static std::string CanonicalizeFileExtension(
      const std::filesystem::path& path);

  // Get initial directory for file pickers based on most recently played game
  std::filesystem::path GetFilePickerInitialDirectory() const;

  void ClearDialogs();

  Emulator* emulator_;
  ui::WindowedAppContext& app_context_;
  EmulatorWindowListener window_listener_;

  std::unique_ptr<ui::Window> window_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  // Creation may fail, in this case immediate drawer UI must not be drawn.
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  ui::Presenter* presenter_painting_ = nullptr;

  bool emulator_initialized_ = false;
  std::atomic<bool> disable_hotkeys_ = false;

  std::string base_title_;
  bool initializing_shader_storage_ = false;
  // Disc number after disc swap (0 = use XEX header value)
  uint8_t swapped_disc_number_ = 0;

  ui::ImGuiPostProcessingDialog* postprocessing_dialog_ = nullptr;
  ui::ImGuiPerformanceDialog* performance_dialog_ = nullptr;
  ProfileConfigDialog* profile_dialog_ = nullptr;
  ui::ImGuiContextMenu* context_menu_ = nullptr;
  ui::ImGuiXmpDialog* xmp_dialog_ = nullptr;

  GameListPanel* game_list_panel_ = nullptr;
  std::unique_ptr<WxToolbarState> wx_toolbar_state_;
  // True when the app was started with --target and the title hasn't been
  // explicitly stopped yet — keeps the render pane visible during the gap
  // between the window appearing and on_launch firing.
  bool target_pending_launch_ = false;
  // Render dimensions captured at app start, restored after Stop.
  uint32_t default_logical_width_ = 0;
  uint32_t default_logical_height_ = 0;
  ui::MenuItem* file_open_menu_item_ = nullptr;
  ui::MenuItem* file_stop_menu_item_ = nullptr;
  ui::MenuItem* profile_menu_ = nullptr;
  ui::MenuItem* config_menu_ = nullptr;
  ui::MenuItem* tools_menu_ = nullptr;
  ui::MenuItem* view_show_toolbar_item_ = nullptr;
  bool show_toolbar_ = true;
  void RefreshProfileMenu();
  void PopulateProfileMenu(ui::MenuItem* parent);
  void RefreshProfileIcon();
  void ShowProfilePopupMenu();
  // Show or hide the icon toolbar pane and persist the preference.
  void SetToolbarVisible(bool visible);
  // Reapply the "render visible iff title open or fullscreen, game list
  // otherwise" invariant. Called on every relevant state change.
  void ApplyContentVisibility();
  // Tear down the running title on a non-guest thread and refresh the UI to
  // show the game list. Skips the user prompt — caller is responsible for
  // confirmation.
  void StopTitleAndReturnToList();
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_EMULATOR_WINDOW_H_
