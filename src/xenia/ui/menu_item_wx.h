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
#include <unordered_map>

#include <wx/wx.h>

#include "xenia/ui/menu_item.h"

namespace xe {
namespace ui {

class WxMenuItem : public MenuItem {
 public:
  WxMenuItem(Type type, const std::string& text, const std::string& hotkey,
             std::function<void()> callback);
  WxMenuItem(Type type, const std::string& text, bool initially_checked,
             std::function<void(bool)> on_toggle);
  ~WxMenuItem() override;

  // wxString factory overloads so _("...") drops in directly at call sites.
  static std::unique_ptr<MenuItem> Create(MenuItem::Type type);
  static std::unique_ptr<MenuItem> Create(MenuItem::Type type,
                                          const wxString& text);
  static std::unique_ptr<MenuItem> Create(MenuItem::Type type,
                                          const wxString& text,
                                          std::function<void()> callback);
  static std::unique_ptr<MenuItem> Create(MenuItem::Type type,
                                          const wxString& text,
                                          const wxString& hotkey,
                                          std::function<void()> callback);
  static std::unique_ptr<MenuItem> CreateCheck(
      const wxString& text, bool initially_checked,
      std::function<void(bool)> on_toggle);

  // For kNormal type, returns the wxMenuBar.
  wxMenuBar* GetMenuBar() const { return menu_bar_; }

  // For kPopup type, returns the wxMenu submenu.
  wxMenu* GetMenu() const { return menu_; }

  void SetEnabled(bool enabled) override;
  void SetChecked(bool checked) override;

  using MenuItem::OnSelected;
  using MenuItem::OnToggled;

 protected:
  void OnChildAdded(MenuItem* child_item) override;
  void OnChildRemoved(MenuItem* child_item) override;

 private:
  static int AllocateId();

  void OnMenuItemSelected(wxCommandEvent& event);

  int wx_id_ = wxID_NONE;
  wxMenuBar* menu_bar_ = nullptr;
  wxMenu* menu_ = nullptr;
  wxMenuItem* wx_menu_item_ = nullptr;
  std::unordered_map<int, WxMenuItem*> id_to_child_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_MENU_ITEM_WX_H_
