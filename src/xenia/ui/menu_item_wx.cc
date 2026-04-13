/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/menu_item_wx.h"

#include <atomic>
#include <string>

namespace xe {
namespace ui {

// MenuItem::Create factory -- returns WxMenuItem on all desktop platforms.
std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  return std::make_unique<WxMenuItem>(type, text, hotkey, std::move(callback));
}

int WxMenuItem::AllocateId() {
  static std::atomic<int> next_id{wxID_HIGHEST + 1};
  return next_id.fetch_add(1);
}

WxMenuItem::WxMenuItem(Type type, const std::string& text,
                       const std::string& hotkey,
                       std::function<void()> callback)
    : MenuItem(type, text, hotkey, std::move(callback)) {
  switch (type) {
    case Type::kNormal:
      menu_bar_ = new wxMenuBar();
      break;
    case Type::kPopup:
      menu_ = new wxMenu();
      break;
    case Type::kString:
      wx_id_ = AllocateId();
      break;
    case Type::kSeparator:
      break;
  }
}

WxMenuItem::~WxMenuItem() {
  // wxMenuBar and wxMenu ownership:
  // - If a wxMenuBar has been attached to a wxFrame via SetMenuBar(), the frame
  //   owns it. We must not delete it here in that case.
  // - If a wxMenu has been appended to a wxMenuBar or parent wxMenu, the parent
  //   owns it. We must not delete it.
  // - Unattached menu bars / menus must be cleaned up.
  //
  // wxWidgets manages the ownership tree, so we only delete top-level
  // unattached objects. For now, rely on wxWidgets' parent-child ownership.
  // The wxMenuBar is owned by the wxFrame once SetMenuBar is called.
  // Individual wxMenus are owned by their parent menu/bar once Append'd.
}

void WxMenuItem::SetEnabled(bool enabled) {
  if (type_ == Type::kString && parent_item_) {
    auto* parent_wx = static_cast<WxMenuItem*>(parent_item_);
    if (parent_wx->menu_) {
      parent_wx->menu_->Enable(wx_id_, enabled);
    }
  }
}

void WxMenuItem::OnChildAdded(MenuItem* child_item) {
  auto* child = static_cast<WxMenuItem*>(child_item);

  if (type_ == Type::kNormal && menu_bar_) {
    // Adding a top-level popup menu to the menu bar.
    if (child->type_ == Type::kPopup && child->menu_) {
      menu_bar_->Append(child->menu_, wxString::FromUTF8(child->text_));
    }
  } else if (menu_) {
    // Adding to a submenu (kPopup type).
    switch (child->type_) {
      case Type::kPopup:
        if (child->menu_) {
          menu_->AppendSubMenu(child->menu_, wxString::FromUTF8(child->text_));
        }
        break;
      case Type::kString: {
        // Build the label: "Text\tHotkey" if hotkey is provided.
        wxString label = wxString::FromUTF8(child->text_);
        if (!child->hotkey_.empty()) {
          label += wxT("\t") + wxString::FromUTF8(child->hotkey_);
        }
        menu_->Append(child->wx_id_, label);
        // Bind the menu event to invoke the callback.
        menu_->Bind(
            wxEVT_MENU,
            [child](wxCommandEvent&) {
              // OnSelected may destroy the menu item via its callback,
              // so do not access child after this call.
              child->OnSelected();
            },
            child->wx_id_);
        break;
      }
      case Type::kSeparator:
        menu_->AppendSeparator();
        break;
      default:
        break;
    }
  }
}

void WxMenuItem::OnChildRemoved(MenuItem* child_item) {
  auto* child = static_cast<WxMenuItem*>(child_item);
  if (menu_ && child->wx_id_ != wxID_NONE) {
    menu_->Unbind(wxEVT_MENU, [](wxCommandEvent&) {}, child->wx_id_);
    menu_->Delete(child->wx_id_);
  }
}

}  // namespace ui
}  // namespace xe
