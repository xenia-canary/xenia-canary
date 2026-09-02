/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOW_MAC_H_
#define XENIA_UI_WINDOW_MAC_H_

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "xenia/ui/menu_item.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window.h"

#ifdef __OBJC__
@class NSWindow;
@class NSView;
@class XeniaWindowDelegate;
@class XeniaContentView;
#else
typedef void NSWindow;
typedef void NSView;
typedef void XeniaWindowDelegate;
typedef void XeniaContentView;
#endif

namespace xe {
namespace ui {

class MacWindow : public Window {
 public:
  MacWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~MacWindow() override;

  uint32_t GetLatestDpiImpl() const override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void LoadAndApplyIcon(const void* buffer, size_t size,
                        bool can_apply_state_in_current_phase) override;
  void ApplyNewMainMenu(MenuItem* old_main_menu) override;
  void CompleteMainMenuItemsUpdateImpl() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

  NSWindow* native_window() const { return window_; }
  NSView* content_view() const { return reinterpret_cast<NSView*>(content_view_); }

  // True while the pointer is decoupled from the OS cursor for relative-motion
  // (FPS mouse-look) input; see ApplyNewMouseCapture().
  bool is_mouse_captured() const { return mouse_captured_; }

  // Event handlers invoked from Cocoa delegate & view.
  void HandleMouseDown(int32_t x, int32_t y, MouseEvent::Button button);
  void HandleMouseUp(int32_t x, int32_t y, MouseEvent::Button button);
  void HandleMouseMove(int32_t x, int32_t y);
  // Relative pointer motion (already decoupled from the OS cursor). Fed as a
  // synthetic absolute position so downstream delta consumers keep working and
  // never stall at a screen edge.
  void HandleMouseMoveRelative(double dx, double dy);
  void HandleMouseWheel(int32_t x, int32_t y, int32_t dx, int32_t dy);
  void HandleKeyDown(VirtualKey key, bool shift, bool ctrl, bool alt, bool super);
  void HandleKeyUp(VirtualKey key, bool shift, bool ctrl, bool alt, bool super);
  void HandleKeyChar(VirtualKey key, bool shift, bool ctrl, bool alt, bool super);
  void HandleResize(uint32_t logical_w, uint32_t logical_h, uint32_t phys_w, uint32_t phys_h);
  void HandleFocus(bool has_focus);

 private:
  NSWindow* window_ = nullptr;
  XeniaContentView* content_view_ = nullptr;
  XeniaWindowDelegate* delegate_ = nullptr;
  std::atomic<bool> paint_pending_{false};

  // Mouse capture (relative pointer / FPS mouse-look) state.
  bool mouse_captured_ = false;
  bool mouse_captured_hid_cursor_ = false;
  double synth_mouse_x_ = 0.0;
  double synth_mouse_y_ = 0.0;
};

class MacMenuItem : public MenuItem {
 public:
  MacMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback);
  ~MacMenuItem() override;

  void* native_menu() const { return menu_; }
  void* native_item() const { return item_; }

  void SetEnabled(bool enabled) override;

 protected:
  void OnChildAdded(MenuItem* generic_child_item) override;
  void OnChildRemoved(MenuItem* generic_child_item) override;

 private:
  void* menu_ = nullptr;    // NSMenu*
  void* item_ = nullptr;    // NSMenuItem*
  void* target_ = nullptr;  // XeniaMenuItemActionTarget*
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOW_MAC_H_
