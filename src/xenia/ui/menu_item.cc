/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/menu_item.h"

namespace xe {
namespace ui {

std::unique_ptr<MenuItem> MenuItem::Create(Type type) {
  return MenuItem::Create(type, "", "", nullptr);
}

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text) {
  return MenuItem::Create(type, text, "", nullptr);
}

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           std::function<void()> callback) {
  return MenuItem::Create(type, text, "", std::move(callback));
}

MenuItem::MenuItem(Type type, const std::string& text,
                   const std::string& hotkey, std::function<void()> callback)
    : type_(type),
      parent_item_(nullptr),
      previous_item_(nullptr),
      next_item_(nullptr),
      text_(text),
      hotkey_(hotkey),
      callback_(std::move(callback)) {}

MenuItem::~MenuItem() = default;

void MenuItem::AddChild(MenuItem* child_item) {
  AddChild(MenuItemPtr(child_item, [](MenuItem* item) {}));
}

void MenuItem::AddChild(std::unique_ptr<MenuItem> child_item) {
  AddChild(
      MenuItemPtr(child_item.release(), [](MenuItem* item) { delete item; }));
}

void MenuItem::AddChild(MenuItemPtr child_item) {
  auto child_item_ptr = child_item.get();
  child_item_ptr->parent_item_ = this;

  // Doubly Linked List
  if (children_.size()) {
    child_item_ptr->previous_item_ = children_.back().get();
    child_item_ptr->previous_item_->next_item_ = child_item_ptr;
  }

  children_.emplace_back(std::move(child_item));
  OnChildAdded(child_item_ptr);
}

void MenuItem::RemoveChild(MenuItem* child_item) {
  for (auto it = children_.begin(); it != children_.end(); ++it) {
    if (it->get() == child_item) {
      children_.erase(it);
      OnChildRemoved(child_item);
      break;
    }
  }
}

MenuItem* MenuItem::GetItem(uint32_t index) { return children_[index].get(); }

void MenuItem::SetPreviousItem(MenuItem* previous_item) {
  previous_item_ = previous_item;
}

void MenuItem::SetNextItem(MenuItem* next_item) { next_item_ = next_item; }

void MenuItem::OnSelected() {
  if (type() == Type::kChecked) {
    ResetChecked();
    SetChecked(true);
  }

  if (callback_) {
    callback_();
    // Note that this MenuItem might have been destroyed by the callback.
    // Must not do anything with *this in this function from now on.
  }
}

}  // namespace ui
}  // namespace xe
