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

#include <memory>
#include <string>

#include "xenia/ui/menu_item.h"
#include "xenia/ui/window.h"

// The Objective-C objects are stored as retained CFTypeRef-like void pointers
// so this header can be included from C++ code too.

namespace xe {
namespace ui {

class MacWindow : public Window {
  using super = Window;

 public:
  MacWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~MacWindow() override;

  // NSWindow*, null if the window hasn't been opened yet or has been closed.
  void* ns_window() const { return ns_window_; }

  // Handlers for the Objective-C window delegate and content view.
  void HandleClose();
  void HandleSizeUpdate();
  void HandleFocusUpdate(bool has_focus);
  void HandleFullscreenUpdate(bool is_fullscreen);
  void HandleDpiChange();
  void HandlePaint();
  // NSEvent*.
  void HandleKeyEvent(void* event);
  void HandleMouseEvent(void* event);
  void HandleFileDrop(const std::filesystem::path& path);

 protected:
  uint32_t GetLatestDpiImpl() const override;

  bool OpenImpl() override;
  void RequestCloseImpl() override;

  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void ApplyNewMainMenu(MenuItem* old_main_menu) override;
  void CompleteMainMenuItemsUpdateImpl() override;
  void ApplyNewCursorVisibility(
      CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;

  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void DestroyNativeWindow();
  void ApplyCursorVisibility();
  // Makes the main menu of this window the application's menu bar.
  void InstallMainMenu();

  // NSWindow*, retained.
  void* ns_window_ = nullptr;
  // XeniaMetalView*, retained.
  void* ns_view_ = nullptr;
  // XeniaWindowDelegate*, retained.
  void* ns_window_delegate_ = nullptr;

  bool cursor_hidden_ = false;
  // Modifier flags from the last NSEventTypeFlagsChanged.
  uint64_t last_modifier_flags_ = 0;
};

class MacMenuItem : public MenuItem {
 public:
  MacMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback);
  ~MacMenuItem() override;

  // For kNormal (the root of the main menu), an NSMenu*, otherwise an
  // NSMenuItem*. Retained.
  void* handle() const { return handle_; }

  void SetEnabled(bool enabled) override;

  // Called by the Objective-C target of the NSMenuItem.
  void Activate() { OnSelected(); }

 protected:
  void OnChildAdded(MenuItem* child_item) override;
  void OnChildRemoved(MenuItem* child_item) override;

 private:
  void* handle_ = nullptr;
  // XeniaMenuItemTarget*, retained.
  void* target_ = nullptr;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOW_MAC_H_
