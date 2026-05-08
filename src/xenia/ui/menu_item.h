/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_MENU_ITEM_H_
#define XENIA_UI_MENU_ITEM_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "xenia/ui/ui_event.h"

namespace xe {
namespace ui {

class Window;

class MenuItem {
 public:
  typedef std::unique_ptr<MenuItem, void (*)(MenuItem*)> MenuItemPtr;

  enum class Type {
    kPopup,  // Popup menu (submenu)
    kSeparator,
    kNormal,  // Root menu
    kString,  // Menu is just a string
    kCheck,   // Checkable item with on/off state
  };

  static std::unique_ptr<MenuItem> Create(Type type);
  static std::unique_ptr<MenuItem> Create(Type type, const std::string& text);
  static std::unique_ptr<MenuItem> Create(Type type, const std::string& text,
                                          std::function<void()> callback);
  static std::unique_ptr<MenuItem> Create(Type type, const std::string& text,
                                          const std::string& hotkey,
                                          std::function<void()> callback);
  // Creates a checkable item. `on_toggle` is invoked with the new checked
  // state every time the user clicks the item.
  static std::unique_ptr<MenuItem> CreateCheck(
      const std::string& text, bool initially_checked,
      std::function<void(bool)> on_toggle);

  virtual ~MenuItem();

  MenuItem* parent_item() const { return parent_item_; }
  Type type() { return type_; }
  const std::string& text() { return text_; }
  const std::string& hotkey() { return hotkey_; }

  // If the menu is currently attached to a Window, changes to it (such as the
  // elements and the enabled / disabled state) may be not reflected
  // immediately - call Window::CompleteMainMenuItemsUpdate when the
  // modifications are done.

  void AddChild(MenuItem* child_item);
  void AddChild(std::unique_ptr<MenuItem> child_item);
  void AddChild(MenuItemPtr child_item);
  void RemoveChild(MenuItem* child_item);
  MenuItem* child(size_t index) const;
  size_t child_count() const { return children_.size(); }

  virtual void SetEnabled(bool enabled) {}
  // Sets the checked state of a kCheck item. No-op for other types and does
  // not invoke the toggle callback.
  virtual void SetChecked(bool checked) { checked_ = checked; }
  bool IsChecked() const { return checked_; }

 protected:
  MenuItem(Type type, const std::string& text, const std::string& hotkey,
           std::function<void()> callback);
  MenuItem(Type type, const std::string& text, bool initially_checked,
           std::function<void(bool)> on_toggle);

  virtual void OnChildAdded(MenuItem* child_item) {}
  virtual void OnChildRemoved(MenuItem* child_item) {}

  // This MenuItem may be destroyed as a result of the callback, don't do
  // anything with it after the call.
  void OnSelected();
  // For kCheck — flips checked_ and invokes the toggle callback with the new
  // value. Same destruction caveat as OnSelected.
  void OnToggled();

  Type type_;
  MenuItem* parent_item_;
  std::vector<MenuItemPtr> children_;
  std::string text_;
  std::string hotkey_;
  bool checked_ = false;

 private:
  std::function<void()> callback_;
  std::function<void(bool)> toggle_callback_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_MENU_ITEM_H_
