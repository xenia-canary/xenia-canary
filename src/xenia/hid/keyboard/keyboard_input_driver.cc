/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/keyboard/keyboard_input_driver.h"

#include "xenia/base/logging.h"
#include "xenia/hid/hid_flags.h"
#include "xenia/hid/input_system.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window.h"

#define XE_HID_KEYBOARD_BINDING(button, description, cvar_name, \
                                cvar_default_value)             \
  DEFINE_string(cvar_name, cvar_default_value,                  \
                "List of keys to bind to " description          \
                ", separated by spaces",                        \
                "HID.Key")
#include "keyboard_binding_table.inc"
#undef XE_HID_KEYBOARD_BINDING

DEFINE_int32(keyboard_mode, 0,
             "Allows user do specify keyboard working mode. Possible values: 0 "
             "- Disabled, 1 - Enabled, 2 - Passthrough. Passthrough requires "
             "controller being connected!",
             "HID");

DEFINE_int32(
    keyboard_user_index, 0,
    "Controller port that keyboard emulates. [0, 3] - Keyboard is assigned to "
    "selected slot. Passthrough does not require assigning slot.",
    "HID");

namespace xe {
namespace hid {
namespace keyboard {

uint8_t KeyboardInputDriver::VirtualKeyToHIDUsage(uint16_t vk) const {
  // Letters: contiguous in both VK and HID space
  if (vk >= 'A' && vk <= 'Z') {
    return vk - 'A' + 0x04;
  }

  // Digits 1-9 (0 is irregular: 0x27)
  if (vk >= '1' && vk <= '9') {
    return vk - '1' + 0x1E;
  }

  // F1-F12
  if (vk >= static_cast<uint16_t>(ui::VirtualKey::kF1) &&
      vk <= static_cast<uint16_t>(ui::VirtualKey::kF12)) {
    return vk - static_cast<uint16_t>(ui::VirtualKey::kF1) + 0x3A;
  }

  // F13-F24
  if (vk >= static_cast<uint16_t>(ui::VirtualKey::kF13) &&
      vk <= static_cast<uint16_t>(ui::VirtualKey::kF24)) {
    return vk - static_cast<uint16_t>(ui::VirtualKey::kF13) + 0x68;
  }

  // Numpad 1-9 (0 is irregular: 0x62)
  if (vk >= static_cast<uint16_t>(ui::VirtualKey::kNumpad1) &&
      vk <= static_cast<uint16_t>(ui::VirtualKey::kNumpad9)) {
    return vk - static_cast<uint16_t>(ui::VirtualKey::kNumpad1) + 0x59;
  }

  // Modifiers (Left side starts at 0xE0, Right at 0xE4)
  if (vk >= static_cast<uint16_t>(ui::VirtualKey::kLControl) &&
      vk <= static_cast<uint16_t>(ui::VirtualKey::kLWin)) {
    return vk - static_cast<uint16_t>(ui::VirtualKey::kLControl) + 0xE0;
  }
  if (vk >= static_cast<uint16_t>(ui::VirtualKey::kRControl) &&
      vk <= static_cast<uint16_t>(ui::VirtualKey::kRWin)) {
    return vk - static_cast<uint16_t>(ui::VirtualKey::kRControl) + 0xE4;
  }

  switch (vk) {
    case '0':
      return 0x27;
    case static_cast<uint16_t>(ui::VirtualKey::kReturn):
      return 0x28;
    case static_cast<uint16_t>(ui::VirtualKey::kEscape):
      return 0x29;
    case static_cast<uint16_t>(ui::VirtualKey::kBack):
      return 0x2A;
    case static_cast<uint16_t>(ui::VirtualKey::kTab):
      return 0x2B;
    case static_cast<uint16_t>(ui::VirtualKey::kSpace):
      return 0x2C;
    case static_cast<uint16_t>(ui::VirtualKey::kOemMinus):
      return 0x2D;
    case static_cast<uint16_t>(ui::VirtualKey::kOemPlus):
      return 0x2E;
    case static_cast<uint16_t>(ui::VirtualKey::kOem4):
      return 0x2F;
    case static_cast<uint16_t>(ui::VirtualKey::kOem6):
      return 0x30;
    case static_cast<uint16_t>(ui::VirtualKey::kOem5):
      return 0x31;
    case static_cast<uint16_t>(ui::VirtualKey::kOem1):
      return 0x33;
    case static_cast<uint16_t>(ui::VirtualKey::kOem7):
      return 0x34;
    case static_cast<uint16_t>(ui::VirtualKey::kOem3):
      return 0x35;
    case static_cast<uint16_t>(ui::VirtualKey::kOemComma):
      return 0x36;
    case static_cast<uint16_t>(ui::VirtualKey::kOemPeriod):
      return 0x37;
    case static_cast<uint16_t>(ui::VirtualKey::kOem2):
      return 0x38;
    case static_cast<uint16_t>(ui::VirtualKey::kCapital):
      return 0x39;
    case static_cast<uint16_t>(ui::VirtualKey::kSnapshot):
      return 0x46;
    case static_cast<uint16_t>(ui::VirtualKey::kScroll):
      return 0x47;
    case static_cast<uint16_t>(ui::VirtualKey::kPause):
      return 0x48;
    case static_cast<uint16_t>(ui::VirtualKey::kInsert):
      return 0x49;
    case static_cast<uint16_t>(ui::VirtualKey::kHome):
      return 0x4A;
    case static_cast<uint16_t>(ui::VirtualKey::kPrior):
      return 0x4B;
    case static_cast<uint16_t>(ui::VirtualKey::kDelete):
      return 0x4C;
    case static_cast<uint16_t>(ui::VirtualKey::kEnd):
      return 0x4D;
    case static_cast<uint16_t>(ui::VirtualKey::kNext):
      return 0x4E;
    case static_cast<uint16_t>(ui::VirtualKey::kRight):
      return 0x4F;
    case static_cast<uint16_t>(ui::VirtualKey::kLeft):
      return 0x50;
    case static_cast<uint16_t>(ui::VirtualKey::kDown):
      return 0x51;
    case static_cast<uint16_t>(ui::VirtualKey::kUp):
      return 0x52;
    case static_cast<uint16_t>(ui::VirtualKey::kNumLock):
      return 0x53;
    case static_cast<uint16_t>(ui::VirtualKey::kDivide):
      return 0x54;
    case static_cast<uint16_t>(ui::VirtualKey::kMultiply):
      return 0x55;
    case static_cast<uint16_t>(ui::VirtualKey::kSubtract):
      return 0x56;
    case static_cast<uint16_t>(ui::VirtualKey::kAdd):
      return 0x57;
    case static_cast<uint16_t>(ui::VirtualKey::kNumpad0):
      return 0x62;
    case static_cast<uint16_t>(ui::VirtualKey::kDecimal):
      return 0x63;
    case static_cast<uint16_t>(ui::VirtualKey::kApps):
      return 0x65;
    default:
      break;
  }
  return 0x00;
}

bool KeyboardInputDriver::IsPassthroughEnabled() {
  return static_cast<KeyboardMode>(cvars::keyboard_mode) ==
         KeyboardMode::Passthrough;
}

bool KeyboardInputDriver::IsKeyboardForUserEnabled(uint32_t user_index) {
  if (static_cast<KeyboardMode>(cvars::keyboard_mode) !=
      KeyboardMode::Enabled) {
    return false;
  }

  return cvars::keyboard_user_index == user_index;
}

void KeyboardInputDriver::ParseKeyBinding(
    ui::VirtualKey output_key, const std::string_view description,
    const std::string_view source_tokens) {
  for (const std::string_view source_token :
       utf8::split(source_tokens, " ", true)) {
    KeyBinding key_binding;
    key_binding.output_key = output_key;

    std::string_view token = source_token;

    if (utf8::starts_with(token, "_")) {
      key_binding.lowercase = true;
      token = token.substr(1);
    } else if (utf8::starts_with(token, "^")) {
      key_binding.uppercase = true;
      token = token.substr(1);
    }

    if (utf8::starts_with(token, "0x")) {
      token = token.substr(2);
      key_binding.input_key = static_cast<ui::VirtualKey>(
          string_util::from_string<uint16_t>(token, true));
    } else if (token.size() == 1 && (token[0] >= 'A' && token[0] <= 'Z') ||
               (token[0] >= '0' && token[0] <= '9')) {
      key_binding.input_key = static_cast<ui::VirtualKey>(token[0]);
    }

    if (key_binding.input_key == ui::VirtualKey::kNone) {
      XELOGW(
          "keyboard: failed to parse binding \"{}\" for controller input {}.",
          source_token, description);
      continue;
    }

    key_bindings_.push_back(key_binding);
    XELOGI("keyboard: \"{}\" binds key 0x{:X} to controller input {}.",
           source_token, static_cast<uint16_t>(key_binding.input_key),
           description);
  }
}

KeyboardInputDriver::KeyboardInputDriver(xe::ui::Window* window,
                                         size_t window_z_order)
    : InputDriver(window, window_z_order), window_input_listener_(*this) {
#define XE_HID_KEYBOARD_BINDING(button, description, cvar_name,        \
                                cvar_default_value)                    \
  ParseKeyBinding(xe::ui::VirtualKey::kXInputPad##button, description, \
                  cvars::cvar_name);
#include "keyboard_binding_table.inc"
#undef XE_HID_KEYBOARD_BINDING

  window->AddInputListener(&window_input_listener_, window_z_order);
}

KeyboardInputDriver::~KeyboardInputDriver() {
  window()->RemoveInputListener(&window_input_listener_);
}

X_STATUS KeyboardInputDriver::Setup() { return X_STATUS_SUCCESS; }

X_RESULT KeyboardInputDriver::GetCapabilities(uint32_t user_index,
                                              uint32_t flags,
                                              X_INPUT_CAPABILITIES* out_caps) {
  if (!IsKeyboardForUserEnabled(user_index) && !IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (IsPassthroughEnabled()) {
    out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_KEYBOARD;
    out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_USB_KEYBOARD;
    return X_ERROR_SUCCESS;
  }

  out_caps->type = X_INPUT_DEVTYPE::XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = X_INPUT_DEVSUBTYPE::XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_ly = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_rx = (int16_t)0xFFFFu;
  out_caps->gamepad.thumb_ry = (int16_t)0xFFFFu;
  out_caps->vibration.left_motor_speed = 0;
  out_caps->vibration.right_motor_speed = 0;
  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardInputDriver::GetState(uint32_t user_index,
                                       X_INPUT_STATE* out_state) {
  if (!IsKeyboardForUserEnabled(user_index)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  packet_number_++;

  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;

  if (window()->HasFocus()) {
    for (const KeyBinding& b : key_bindings_) {
      if (b.is_pressed) {
        switch (b.output_key) {
          case ui::VirtualKey::kXInputPadA:
            buttons |= X_INPUT_GAMEPAD_A;
            break;
          case ui::VirtualKey::kXInputPadY:
            buttons |= X_INPUT_GAMEPAD_Y;
            break;
          case ui::VirtualKey::kXInputPadB:
            buttons |= X_INPUT_GAMEPAD_B;
            break;
          case ui::VirtualKey::kXInputPadX:
            buttons |= X_INPUT_GAMEPAD_X;
            break;
          case ui::VirtualKey::kXInputPadGuide:
            buttons |= X_INPUT_GAMEPAD_GUIDE;
            break;
          case ui::VirtualKey::kXInputPadDpadLeft:
            buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
            break;
          case ui::VirtualKey::kXInputPadDpadRight:
            buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
            break;
          case ui::VirtualKey::kXInputPadDpadDown:
            buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
            break;
          case ui::VirtualKey::kXInputPadDpadUp:
            buttons |= X_INPUT_GAMEPAD_DPAD_UP;
            break;
          case ui::VirtualKey::kXInputPadRThumbPress:
            buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
            break;
          case ui::VirtualKey::kXInputPadLThumbPress:
            buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
            break;
          case ui::VirtualKey::kXInputPadBack:
            buttons |= X_INPUT_GAMEPAD_BACK;
            break;
          case ui::VirtualKey::kXInputPadStart:
            buttons |= X_INPUT_GAMEPAD_START;
            break;
          case ui::VirtualKey::kXInputPadLShoulder:
            buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
            break;
          case ui::VirtualKey::kXInputPadRShoulder:
            buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
            break;
          case ui::VirtualKey::kXInputPadLTrigger:
            left_trigger = 0xFF;
            break;
          case ui::VirtualKey::kXInputPadRTrigger:
            right_trigger = 0xFF;
            break;
          case ui::VirtualKey::kXInputPadLThumbLeft:
            thumb_lx += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadLThumbRight:
            thumb_lx += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadLThumbDown:
            thumb_ly += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadLThumbUp:
            thumb_ly += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbUp:
            thumb_ry += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbDown:
            thumb_ry += SHRT_MIN;
            break;
          case ui::VirtualKey::kXInputPadRThumbRight:
            thumb_rx += SHRT_MAX;
            break;
          case ui::VirtualKey::kXInputPadRThumbLeft:
            thumb_rx += SHRT_MIN;
            break;
          default:
            assert_unhandled_case(b.output_key);
        }
      }
    }
  }

  out_state->packet_number = packet_number_;
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger = left_trigger;
  out_state->gamepad.right_trigger = right_trigger;
  out_state->gamepad.thumb_lx = thumb_lx;
  out_state->gamepad.thumb_ly = thumb_ly;
  out_state->gamepad.thumb_rx = thumb_rx;
  out_state->gamepad.thumb_ry = thumb_ry;

  if (IsPassthroughEnabled()) {
    memset(out_state, 0, sizeof(X_INPUT_STATE));
  }

  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardInputDriver::SetState(uint32_t user_index,
                                       X_INPUT_VIBRATION* vibration) {
  if (!IsKeyboardForUserEnabled(user_index) && !IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return X_ERROR_SUCCESS;
}

X_RESULT KeyboardInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                           X_INPUT_KEYSTROKE* out_keystroke) {
  if (!IsKeyboardForUserEnabled(user_index) && !IsPassthroughEnabled()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Pop from the queue.
  KeyEvent evt;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (key_events_.empty()) {
      // No keys!
      return X_ERROR_EMPTY;
    }
    evt = key_events_.front();
    key_events_.pop_front();
  }

  X_RESULT result = X_ERROR_EMPTY;

  ui::VirtualKey xinput_virtual_key = ui::VirtualKey::kNone;
  uint16_t unicode = 0;
  uint16_t keystroke_flags = 0;
  uint8_t hid_code = 0;

  if (!IsPassthroughEnabled()) {
    if (IsKeyboardForUserEnabled(user_index)) {
      for (const KeyBinding& key_binding : key_bindings_) {
        if (key_binding.input_key != evt.virtual_key) {
          continue;
        }

        if (key_binding.is_pressed) {
          xinput_virtual_key = key_binding.output_key;
          break;
        }
      }
    }
  } else {
    xinput_virtual_key = evt.virtual_key;

    if (evt.shift_pressed) {
      keystroke_flags |= 0x0008;  // XINPUT_KEYSTROKE_SHIFT
    }

    if (evt.ctrl_pressed) {
      keystroke_flags |= 0x0010;  // XINPUT_KEYSTROKE_CTRL
    }

    if (evt.alt_pressed) {
      keystroke_flags |= 0x0020;  // XINPUT_KEYSTROKE_ALT
    }
  }

  if (xinput_virtual_key != ui::VirtualKey::kNone) {
    if (evt.transition == true) {
      keystroke_flags |= 0x0001;  // XINPUT_KEYSTROKE_KEYDOWN
      if (evt.prev_state == evt.transition) {
        keystroke_flags |= 0x0004;  // XINPUT_KEYSTROKE_REPEAT
      }
    } else if (evt.transition == false) {
      keystroke_flags |= 0x0002;  // XINPUT_KEYSTROKE_KEYUP
    }

    if (IsPassthroughEnabled()) {
      hid_code =
          VirtualKeyToHIDUsage(static_cast<uint16_t>(xinput_virtual_key));

      if (evt.unicode) {
        unicode = evt.unicode;
        keystroke_flags |= 0x1000;  // XINPUT_KEYSTROKE_VALIDUNICODE
      }
    }

    result = X_ERROR_SUCCESS;
  }

  out_keystroke->virtual_key = static_cast<uint16_t>(xinput_virtual_key);
  out_keystroke->unicode = unicode;
  out_keystroke->flags = keystroke_flags;
  out_keystroke->user_index = user_index;
  out_keystroke->hid_code = hid_code;

  // X_ERROR_EMPTY if no new keys
  // X_ERROR_DEVICE_NOT_CONNECTED if no device
  // X_ERROR_SUCCESS if key
  return result;
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnKeyDown(
    ui::KeyEvent& e) {
  driver_.OnKey(e, true);
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnKeyUp(
    ui::KeyEvent& e) {
  driver_.OnKey(e, false);
}

void KeyboardInputDriver::KeyboardWindowInputListener::OnKeyChar(
    ui::KeyEvent& e) {
  driver_.OnChar(e);
}

void KeyboardInputDriver::OnKey(ui::KeyEvent& e, bool is_down) {
  if (static_cast<KeyboardMode>(cvars::keyboard_mode) ==
      KeyboardMode::Disabled) {
    return;
  }

  KeyEvent key;
  key.virtual_key = e.virtual_key();
  key.transition = is_down;
  key.prev_state = e.prev_state();
  key.repeat_count = e.repeat_count();
  key.shift_pressed = e.is_shift_pressed();
  key.ctrl_pressed = e.is_ctrl_pressed();
  key.alt_pressed = e.is_alt_pressed();
  key.capital_pressed = e.is_capital_pressed();

  auto global_lock = global_critical_region_.Acquire();
  key_events_.push_back(key);

  bool uppercase = key.capital_pressed || key.shift_pressed;

  // Update key binding state for the correct input case, otherwise we will
  // trigger both inputs for uppercase and lowercase causing input to be
  // registered twice.
  for (auto& key_binding : key_bindings_) {
    if (key_binding.input_key != key.virtual_key) {
      continue;
    }

    if (key_binding.uppercase && uppercase) {
      key_binding.is_pressed = is_down;
      break;
    }

    if (key_binding.lowercase && !uppercase) {
      key_binding.is_pressed = is_down;
      break;
    }

    if (!key_binding.uppercase && !key_binding.lowercase) {
      key_binding.is_pressed = is_down;
      break;
    }
  }
}

void KeyboardInputDriver::OnChar(ui::KeyEvent& e) {
  if (!IsPassthroughEnabled()) {
    return;
  }

  auto global_lock = global_critical_region_.Acquire();
  for (auto& key_event : key_events_) {
    if (key_event.virtual_key == e.virtual_key() && key_event.transition &&
        key_event.unicode == 0) {
      key_event.unicode = e.unicode();
      break;
    }
  }
}

InputType KeyboardInputDriver::GetInputType() const {
  switch (static_cast<KeyboardMode>(cvars::keyboard_mode)) {
    case KeyboardMode::Disabled:
      return InputType::None;
    case KeyboardMode::Enabled:
      return InputType::Controller;
    case KeyboardMode::Passthrough:
      return InputType::Keyboard;
    default:
      break;
  }
  return InputType::Controller;
}

}  // namespace keyboard
}  // namespace hid
}  // namespace xe
