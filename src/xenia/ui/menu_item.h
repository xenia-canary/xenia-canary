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
    kPopup,      // Popup menu (submenu)
    kSeparator,  // Seperator between elements
    kNormal,     // Root menu
    kString,     // Menu is just a string
    kChecked     // Menu is child of submenu with checkmarks
  };

  static std::unique_ptr<MenuItem> Create(Type type);
  static std::unique_ptr<MenuItem> Create(Type type, const std::string& text);
  static std::unique_ptr<MenuItem> Create(Type type, const std::string& text,
                                          std::function<void()> callback);
  static std::unique_ptr<MenuItem> Create(Type type, const std::string& text,
                                          const std::string& hotkey,
                                          std::function<void()> callback);

  virtual ~MenuItem();

  MenuItem* GetParentItem() const { return parent_item_; }
  MenuItem* GetPreviousItem() const { return previous_item_; }
  MenuItem* GetNextItem() const { return next_item_; }

  Type type() const { return type_; }
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
  void SetPreviousItem(MenuItem* previous_item);
  void SetNextItem(MenuItem* next_item);
  MenuItem* GetItem(uint32_t index);

  virtual void SetEnabledCascade(bool enabled) {}
  virtual void SetEnabled(bool enabled) {}
  virtual void SetEnabled(uint32_t position, bool enabled) {}
  virtual void SetChecked(bool checked) {}
  virtual void SetChecked(uint32_t identifier, bool checked) {}
  virtual void ResetChecked() {};
  virtual void ModifyString(std::string modify_str) {}

 protected:
  MenuItem(Type type, const std::string& text, const std::string& hotkey,
           std::function<void()> callback);

  virtual void OnChildAdded(MenuItem* child_item) {}
  virtual void OnChildRemoved(MenuItem* child_item) {}

  // This MenuItem may be destroyed as a result of the callback, don't do
  // anything with it after the call.
  void OnSelected();

  Type type_;
  MenuItem* parent_item_;
  MenuItem* previous_item_;
  MenuItem* next_item_;
  std::vector<MenuItemPtr> children_;
  std::string text_;
  std::string hotkey_;

 private:
  std::function<void()> callback_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_MENU_ITEM_H_
