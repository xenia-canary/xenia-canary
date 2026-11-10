/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/keyboard_ui.h"

#include "xenia/base/string.h"
#include "xenia/base/string_util.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

KeyboardInputDialog::KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                                         std::string& title,
                                         std::string& description,
                                         std::string& default_text,
                                         size_t max_length)
    : XamModalDialog(imgui_drawer),
      description_(description),
      max_chars_(max_length ? max_length - 1 : 0) {
  if (!title.empty()) {
    set_title(title, "XamKeyboard");
  } else if (!description_.empty()) {
    set_title(description_, "XamKeyboard");
    description_.clear();
  } else {
    set_title("Keyboard Input", "XamKeyboard");
  }

  text_buffer_.resize(max_chars_ * kMaxUtf8BytesPerUtf16Unit + 1);
  SetText(default_text);
}

size_t KeyboardInputDialog::FitUtf16Length(std::string_view text,
                                           size_t max_units) {
  size_t units = 0;

  for (size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<uint8_t>(text[i]);

    if ((byte & kUtf8ContinuationMask) == kUtf8ContinuationBits) {
      continue;
    }

    const size_t width = byte >= kUtf8FourByteLead ? 2 : 1;

    if (units + width > max_units) {
      return i;
    }

    units += width;
  }

  return text.size();
}

int KeyboardInputDialog::LimitTextLength(ImGuiInputTextCallbackData* data) {
  const size_t max_units = *static_cast<const size_t*>(data->UserData);
  const int keep = static_cast<int>(
      FitUtf16Length(std::string_view(data->Buf, data->BufTextLen), max_units));

  if (keep < data->BufTextLen) {
    data->DeleteChars(keep, data->BufTextLen - keep);
  }

  return 0;
}

void KeyboardInputDialog::SetText(std::string_view text) {
  xe::string_util::copy_truncating(
      text_buffer_.data(), text.substr(0, FitUtf16Length(text, max_chars_)),
      text_buffer_.size());
}

void KeyboardInputDialog::DrawContextMenu() {
  context_menu_open_ = ImGui::BeginPopupContextItem("##TextContextMenu");
  if (!context_menu_open_) {
    return;
  }

  const char* clipboard_text = ImGui::GetClipboardText();
  const std::string_view clipboard =
      clipboard_text ? clipboard_text : std::string_view();

  if (ImGui::MenuItem("Paste", nullptr, false, !clipboard.empty())) {
    SetText(clipboard);
  }

  if (ImGui::MenuItem("Clear", nullptr, false, text_buffer_[0] != '\0')) {
    SetText("");
  }

  ImGui::EndPopup();
}

void KeyboardInputDialog::DrawCharacterCount(float right_x) {
  const std::string count = fmt::format(
      "{} / {}", xe::to_utf16(text_buffer_.data()).size(), max_chars_);

  ImGui::SetCursorPosX(right_x - ImGui::CalcTextSize(count.c_str()).x);
  ImGui::TextDisabled("%s", count.c_str());
}

void KeyboardInputDialog::Finish(bool cancelled) {
  cancelled_ = cancelled;
  text_ = cancelled ? "" : std::string(text_buffer_.data());
  Dismiss();
}

void KeyboardInputDialog::OnDraw(ImGuiIO& io) {
  if (!BeginModal()) {
    return;
  }

  // The context menu closes on the same key press, so don't cancel with it.
  if (!first_draw() && !context_menu_open_ && IsCancelPressed()) {
    Finish(true);
    ImGui::EndPopup();
    return;
  }

  DrawDescription(description_);
  ImGui::Spacing();

  const float input_width = ContentWidth();
  const float input_x = ImGui::GetCursorPosX();

  if (first_draw()) {
    ImGui::SetKeyboardFocusHere();
  }

  ImGui::SetNextItemWidth(input_width);
  const bool submitted = ImGui::InputText(
      "##Text", text_buffer_.data(), text_buffer_.size(),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit,
      LimitTextLength, &max_chars_);

  DrawContextMenu();
  DrawCharacterCount(input_x + input_width);

  static const std::vector<std::string> buttons = {"OK", "Cancel"};
  const int pressed = DrawButtons(buttons);

  if (submitted || pressed == 0) {
    Finish(false);
  } else if (pressed == 1) {
    Finish(true);
  }

  ImGui::EndPopup();
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
