/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOW_WX_H_
#define XENIA_UI_WINDOW_WX_H_

#include <memory>
#include <string>

#include <wx/dnd.h>
#include <wx/wx.h>

#include "xenia/base/platform.h"
#include "xenia/ui/window.h"

#if XE_PLATFORM_WIN32
#include <Dbt.h>
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace ui {

class WxWindow : public Window {
 public:
  WxWindow(WindowedAppContext& app_context, const std::string_view title,
           uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~WxWindow() override;

  wxFrame* frame() const { return frame_; }

#if XE_PLATFORM_WIN32
  // Returns the top-level HWND for use by the presenter (monitor detection).
  HWND hwnd() const {
    return frame_ ? static_cast<HWND>(frame_->GetHandle()) : nullptr;
  }
#endif

  uint32_t GetMediumDpi() const override { return 96; }

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  uint32_t GetLatestDpiImpl() const override;
  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void LoadAndApplyIcon(const void* buffer, size_t size,
                        bool can_apply_state_in_current_phase) override;
  void ApplyNewMainMenu(MenuItem* old_main_menu) override;
  void CompleteMainMenuItemsUpdateImpl() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(
      CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
#if XE_PLATFORM_WIN32
  class XeniaFrame;
#endif
  class FileDropTargetImpl;

  // Translate wxKeyCode to Xenia VirtualKey.
  static VirtualKey TranslateKeyCode(int wx_key);

  // Event handlers bound to the frame.
  void OnFrameClose(wxCloseEvent& event);
  void OnFrameSize(wxSizeEvent& event);
  void OnFrameDpiChanged(wxDPIChangedEvent& event);
  void OnFrameSetFocus(wxFocusEvent& event);
  void OnFrameKillFocus(wxFocusEvent& event);

  void OnPanelKeyDown(wxKeyEvent& event);
  void OnPanelKeyUp(wxKeyEvent& event);
  void OnPanelChar(wxKeyEvent& event);
  void OnPanelDoubleClick(wxMouseEvent& event);
  void OnPanelMouseDown(wxMouseEvent& event);
  void OnPanelMouseUp(wxMouseEvent& event);
  void OnPanelMouseMove(wxMouseEvent& event);
  void OnPanelMouseWheel(wxMouseEvent& event);
  void OnPanelPaint(wxPaintEvent& event);
  void OnPanelEraseBackground(wxEraseEvent& event);

  // Cursor auto-hide timer.
  void OnCursorAutoHideTimer(wxTimerEvent& event);

  void DropStartupCoverIfPresent();

  wxWindow* render_target() const;

  wxFrame* frame_ = nullptr;
  // Black child covering the client area until the swap chain has presented
  // its first frame. Dropped from the WM_PAINT path when HasSurface() flips
  // true.
  wxWindow* startup_cover_ = nullptr;
  wxTimer cursor_auto_hide_timer_;
  bool cursor_currently_auto_hidden_ = false;
  bool menu_open_ = false;
  uint32_t dpi_ = 96;

#if XE_PLATFORM_WIN32
  HDEVNOTIFY usb_device_notify_ = nullptr;
#endif
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOW_WX_H_
