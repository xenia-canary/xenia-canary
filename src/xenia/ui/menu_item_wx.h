/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_MENU_ITEM_WX_H_
#define XENIA_UI_MENU_ITEM_WX_H_

#include <functional>
#include <string>

#include <wx/wx.h>

#include "xenia/ui/menu_item.h"

namespace xe {
namespace ui {

class WxMenuItem : public MenuItem {
 public:
  WxMenuItem(Type type, const std::string& text, const std::string& hotkey,
             std::function<void()> callback);
  ~WxMenuItem() override;

  // For kNormal type, returns the wxMenuBar.
  wxMenuBar* GetMenuBar() const { return menu_bar_; }

  // For kPopup type, returns the wxMenu submenu.
  wxMenu* GetMenu() const { return menu_; }

  void SetEnabled(bool enabled) override;

  using MenuItem::OnSelected;

 protected:
  void OnChildAdded(MenuItem* child_item) override;
  void OnChildRemoved(MenuItem* child_item) override;

 private:
  // Allocates a unique wx menu item ID for event binding.
  static int AllocateId();

  int wx_id_ = wxID_NONE;

  // kNormal -> owns a wxMenuBar
  wxMenuBar* menu_bar_ = nullptr;

  // kPopup -> owns a wxMenu (submenu)
  wxMenu* menu_ = nullptr;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_MENU_ITEM_WX_H_
