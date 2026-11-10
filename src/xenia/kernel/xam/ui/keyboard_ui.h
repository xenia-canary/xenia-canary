/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_KEYBOARD_UI_H_
#define XENIA_KERNEL_XAM_UI_KEYBOARD_UI_H_

#include "xenia/kernel/xam/ui/modal_dialog_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

class KeyboardInputDialog : public XamModalDialog {
 public:
  KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                      std::string& description, std::string& default_text,
                      size_t max_length);
  virtual ~KeyboardInputDialog() {}

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;

 private:
  // UTF-8 continuation bytes are 10xxxxxx.
  static constexpr uint8_t kUtf8ContinuationMask = 0xC0;
  static constexpr uint8_t kUtf8ContinuationBits = 0x80;
  // 11110xxx starts a 4-byte sequence, which is a surrogate pair in UTF-16.
  static constexpr uint8_t kUtf8FourByteLead = 0xF0;
  static constexpr size_t kMaxUtf8BytesPerUtf16Unit = 3;

  // Returns the byte length of the longest UTF-8 prefix that fits in
  // max_units UTF-16 code units.
  static size_t FitUtf16Length(std::string_view text, size_t max_units);
  static int LimitTextLength(ImGuiInputTextCallbackData* data);

  void SetText(std::string_view text);
  void DrawContextMenu();
  void DrawCharacterCount(float right_x);
  void Finish(bool cancelled);

  std::string description_;
  size_t max_chars_ = 0;
  std::vector<char> text_buffer_;
  std::string text_;
  bool cancelled_ = true;
  bool context_menu_open_ = false;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
