/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_context_menu.h"

#include <algorithm>
#include <cfloat>

#include "third_party/imgui/imgui.h"
#include "xenia/hid/input_system.h"

namespace xe {
namespace ui {

ImGuiContextMenu::ImGuiContextMenu(ImGuiDrawer* drawer,
                                   hid::InputSystem* input_system)
    : ImGuiDialog(drawer), input_system_(input_system) {
  if (input_system_) {
    input_system_->AddUIInputBlocker();

    // Initialize prev_buttons_ to current state so held buttons aren't
    // detected as "just pressed" when the menu opens
    hid::X_INPUT_STATE state;
    for (uint32_t user_index = 0; user_index < 4; user_index++) {
      if (input_system_->GetStateForUI(user_index, 1, &state) == 0) {
        prev_buttons_ = state.gamepad.buttons;
        break;
      }
    }
  }
}

ImGuiContextMenu::~ImGuiContextMenu() {
  if (input_system_) {
    input_system_->RemoveUIInputBlocker();
  }
}

void ImGuiContextMenu::OnClose() {
  if (on_close_callback_) {
    on_close_callback_();
  }
  // Defer action callback to execute after the current frame completes
  // This prevents issues with operations like fullscreen that need the
  // ImGui state to be fully cleaned up first
  if (pending_callback_) {
    imgui_drawer()->PostDeferredCallback(std::move(pending_callback_));
  }
}

void ImGuiContextMenu::AddAction(const std::string& text,
                                 std::function<void()> callback,
                                 const std::string& shortcut) {
  items_.push_back({text, shortcut, callback, false});
}

void ImGuiContextMenu::AddSeparator() {
  items_.push_back({"", "", nullptr, true});
}

void ImGuiContextMenu::Show() {
  center_on_screen_ = true;
  // Find first selectable item
  focused_index_ = GetNextSelectableItem(-1, 1);
}

void ImGuiContextMenu::ShowAt(float x, float y) {
  center_on_screen_ = false;
  position_x_ = x;
  position_y_ = y;
  // Find first selectable item
  focused_index_ = GetNextSelectableItem(-1, 1);
}

int ImGuiContextMenu::GetNextSelectableItem(int current, int direction) {
  if (items_.empty()) {
    return -1;
  }

  int count = static_cast<int>(items_.size());
  int next = current;

  for (int i = 0; i < count; i++) {
    next += direction;
    if (next < 0) {
      next = count - 1;
    }
    if (next >= count) {
      next = 0;
    }

    if (!items_[next].is_separator) {
      return next;
    }
  }

  return current;  // No selectable items found
}

void ImGuiContextMenu::PollGamepad() {
  if (!input_system_ || items_.empty()) {
    return;
  }

  for (uint32_t i = 0; i < 4; ++i) {
    hid::X_INPUT_STATE state;
    if (input_system_->GetStateForUI(i, 1, &state) == 0) {
      uint16_t buttons = state.gamepad.buttons;
      uint16_t pressed = buttons & ~prev_buttons_;

      // D-pad navigation
      if (pressed & 0x0001) {  // D-pad up
        focused_index_ = GetNextSelectableItem(focused_index_, -1);
      }
      if (pressed & 0x0002) {  // D-pad down
        focused_index_ = GetNextSelectableItem(focused_index_, 1);
      }

      // A button to activate focused item
      if (pressed & 0x1000) {
        ActivateItem(focused_index_);
      }

      // B button, Back button, or Guide button to close
      if (pressed & (0x2000 | 0x0020 | 0x0400)) {
        Close();
      }

      prev_buttons_ = buttons;
      break;
    }
  }
}

void ImGuiContextMenu::ActivateItem(int index) {
  if (activation_pending_) {
    return;  // Already activating an item
  }
  if (index >= 0 && index < static_cast<int>(items_.size()) &&
      !items_[index].is_separator && items_[index].callback) {
    activation_pending_ = true;
    // Store callback to execute after dialog is fully closed
    pending_callback_ = items_[index].callback;
    Close();
  }
}

void ImGuiContextMenu::OnDraw(ImGuiIO& io) {
  // Poll gamepad input
  PollGamepad();

  // Open popup on first draw
  if (!has_opened_) {
    ImGui::OpenPopup("##ContextMenu");
    has_opened_ = true;
  }

  // Style the popup - white background, black text
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  // Position the popup
  if (center_on_screen_) {
    ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  } else {
    ImGui::SetNextWindowPos(ImVec2(position_x_, position_y_),
                            ImGuiCond_Appearing);
  }

  // Calculate required width for menu items
  float max_text_width = 0.0f;
  float max_shortcut_width = 0.0f;
  for (const auto& item : items_) {
    if (!item.is_separator) {
      float text_width = ImGui::CalcTextSize(item.text.c_str()).x;
      max_text_width = std::max(max_text_width, text_width);
      if (!item.shortcut.empty()) {
        float shortcut_width = ImGui::CalcTextSize(item.shortcut.c_str()).x;
        max_shortcut_width = std::max(max_shortcut_width, shortcut_width);
      }
    }
  }
  float padding = ImGui::GetStyle().WindowPadding.x;
  float item_spacing = ImGui::GetStyle().ItemSpacing.x;
  float gap = item_spacing * 3.0f;  // Gap between text and shortcut
  float min_width = max_text_width + gap + max_shortcut_width + padding * 2;

  bool is_open = true;
  if (ImGui::BeginPopupModal("##ContextMenu", &is_open,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove)) {
    // Handle keyboard input
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
      Close();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
      focused_index_ = GetNextSelectableItem(focused_index_, -1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
      focused_index_ = GetNextSelectableItem(focused_index_, 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
      ActivateItem(focused_index_);
    }

    // Draw menu items
    for (int i = 0; i < static_cast<int>(items_.size()); i++) {
      const auto& item = items_[i];

      if (item.is_separator) {
        ImGui::PushStyleColor(ImGuiCol_Separator,
                              ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        continue;
      }

      bool is_focused = (i == focused_index_);

      // Item background - Xbox green (#107C10) for highlight
      const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
      if (is_focused) {
        ImGui::PushStyleColor(ImGuiCol_Header, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, xbox_green);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
      }

      // Create selectable item with minimum width
      ImGui::PushID(i);
      if (ImGui::Selectable("##item", is_focused,
                            ImGuiSelectableFlags_SpanAllColumns,
                            ImVec2(min_width, 0))) {
        ActivateItem(i);
      }

      // Update focus on hover
      if (ImGui::IsItemHovered()) {
        focused_index_ = i;
      }

      // Draw text on same line
      ImGui::SameLine();
      ImGui::SetCursorPosX(padding);
      ImGui::TextUnformatted(item.text.c_str());

      // Draw shortcut if present
      if (!item.shortcut.empty()) {
        float shortcut_width =
            ImGui::CalcTextSize(item.shortcut.c_str()).x + padding;
        float window_width = ImGui::GetWindowWidth();
        ImGui::SameLine(window_width - shortcut_width);
        // Lighter text for shortcut, white if focused for contrast on green
        ImGui::PushStyleColor(ImGuiCol_Text,
                              is_focused ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f)
                                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
        ImGui::TextUnformatted(item.shortcut.c_str());
        ImGui::PopStyleColor();
      }

      ImGui::PopID();
      ImGui::PopStyleColor(4);
    }

    ImGui::EndPopup();
  } else {
    // Popup was closed (clicked outside)
    Close();
  }

  ImGui::PopStyleVar(1);
  ImGui::PopStyleColor(3);
}

}  // namespace ui
}  // namespace xe
