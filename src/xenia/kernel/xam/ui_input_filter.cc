/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui_input_filter.h"

#include "xenia/hid/input_system.h"

namespace xe {
namespace kernel {
namespace xam {

using hid::X_INPUT_FLAG;
using hid::X_INPUT_KEYSTROKE;
using hid::X_INPUT_STATE;

void UIInputFilter::OnUIClosed(hid::InputSystem* input_system) {
  auto lock = input_system->lock();

  for (uint32_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    X_INPUT_STATE state = {};
    const X_RESULT result = input_system->GetState(
        user_index, X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD, &state);

    held_buttons_[user_index] = 0;

    if (result == X_ERROR_SUCCESS) {
      held_buttons_[user_index] = state.gamepad.buttons;
    }

    X_INPUT_KEYSTROKE keystroke = {};
    for (int i = 0; i < kMaxQueuedKeystrokes; i++) {
      if (input_system->GetKeystroke(user_index,
                                     X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD,
                                     &keystroke) != X_ERROR_SUCCESS) {
        break;
      }
    }
  }
}

void UIInputFilter::FilterState(uint32_t user_index, X_INPUT_STATE* state) {
  const uint16_t buttons = state->gamepad.buttons;
  uint16_t& held = held_buttons_[user_index];

  held &= buttons;
  state->gamepad.buttons = static_cast<uint16_t>(buttons & ~held);
}

bool UIInputFilter::ShouldDropKeystroke(hid::InputSystem* input_system,
                                        const X_INPUT_KEYSTROKE& keystroke) {
  const uint32_t user_index = keystroke.user_index;

  if (user_index >= XUserMaxUserCount ||
      !(keystroke.flags & hid::X_INPUT_KEYSTROKE_REPEAT)) {
    return false;
  }

  uint16_t& held = held_buttons_[user_index];
  if (!held) {
    return false;
  }

  // Keystroke-only games never call FilterState, so check for releases here.
  X_INPUT_STATE state = {};
  if (input_system->GetState(user_index, X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD,
                             &state) == X_ERROR_SUCCESS) {
    held &= state.gamepad.buttons;
  } else {
    held = 0;
  }

  return held != 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
