/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_H_
#define XENIA_HID_INPUT_H_

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"

namespace xe {
namespace hid {

constexpr int32_t X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
constexpr int32_t X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;
constexpr uint8_t X_INPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;

enum X_INPUT_CAPS {
  X_INPUT_CAPS_FFB_SUPPORTED = 0x0001,
  X_INPUT_CAPS_WIRELESS = 0x0002,
  X_INPUT_CAPS_VOICE_SUPPORTED = 0x0004,
  X_INPUT_CAPS_PMD_SUPPORTED = 0x0008,
  X_INPUT_CAPS_NO_NAVIGATION = 0x0010,
};

enum X_INPUT_FLAG {
  X_INPUT_FLAG_GAMEPAD = 0x00000001,
  X_INPUT_FLAG_KEYBOARD = 0x00000002,
  X_INPUT_FLAG_UNKNOWN = 0x00000004,
  X_INPUT_FLAG_BIG_BUTTON = 0x00000008,
  X_INPUT_FLAG_MIC = 0x00000020,
  X_INPUT_FLAG_ANYDEVICE = 0x000000FF,
  X_INPUT_FLAG_ANY_USER = 1 << 30
};

enum X_USER_DEVICE_CLASS : uint32_t {
  DEVICE_CLASS_GAMEPAD = 0x00000000,  // X_INPUT_FLAG_GAMEPAD uses this
  DEVICE_CLASS_BIG_BUTTON = 0x00000003,
  DEVICE_CLASS_MIC = 0x00000004,
  DEVICE_CLASS_NUI = 0x00000005,
  DEVICE_CLASS_MASK = 0x000000FF,
};

enum X_USER_DEVICE_TYPE : uint32_t {
  // DEVICE_TYPE_NUI_MAX = 0x00000008 // DEVICE_CLASS_NUI, any value less than
  // 8, XamUserNuiGetUserIndexForBind, XamUserNuiBind
  DEVICE_TYPE_UNK1 = 0x40000000,   // DEVICE_CLASS == 1
  DEVICE_TYPE_MIC_1 = 0x50000000,  // DEVICE_CLASS_MIC
  DEVICE_TYPE_MIC_2 = 0x60000000,  // DEVICE_CLASS_MIC
  DEVICE_TYPE_UNK2 = 0x70000000,   // DEVICE_CLASS == 1
  DEVICE_TYPE_MASK = 0xF0000000,
};

enum X_INPUT_DEVTYPE {
  XINPUT_DEVTYPE_GAMEPAD = 0x00000001,
  XINPUT_DEVTYPE_KEYBOARD = 0x00000002,
};

enum X_INPUT_DEVSUBTYPE {
  XINPUT_DEVSUBTYPE_USB_KEYBOARD = 0x00000000,
  XINPUT_DEVSUBTYPE_GAMEPAD = 0x00000001,
  XINPUT_DEVSUBTYPE_WHEEL = 0x00000002,
  XINPUT_DEVSUBTYPE_ARCADE_STICK = 0x00000003,
  XINPUT_DEVSUBTYPE_FLIGHT_STICK = 0x00000004,
  XINPUT_DEVSUBTYPE_DANCE_PAD = 0x00000005,
  XINPUT_DEVSUBTYPE_GUITAR = 0x00000006,
  XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE = 0x00000007,
  XINPUT_DEVSUBTYPE_DRUM_KIT = 0x00000008,
  XINPUT_DEVSUBTYPE_UNK = 0x00000009,
  XINPUT_DEVSUBTYPE_GUITAR_BASS = 0x0000000B,
  XINPUT_DEVSUBTYPE_ARCADE_PAD = 0x00000013,
};

enum X_INPUT_GAMEPAD_BUTTON {
  X_INPUT_GAMEPAD_DPAD_UP = 0x0001,
  X_INPUT_GAMEPAD_DPAD_DOWN = 0x0002,
  X_INPUT_GAMEPAD_DPAD_LEFT = 0x0004,
  X_INPUT_GAMEPAD_DPAD_RIGHT = 0x0008,
  X_INPUT_GAMEPAD_START = 0x0010,
  X_INPUT_GAMEPAD_BACK = 0x0020,
  X_INPUT_GAMEPAD_LEFT_THUMB = 0x0040,
  X_INPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
  X_INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
  X_INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
  X_INPUT_GAMEPAD_GUIDE = 0x0400,
  X_INPUT_GAMEPAD_A = 0x1000,
  X_INPUT_GAMEPAD_B = 0x2000,
  X_INPUT_GAMEPAD_X = 0x4000,
  X_INPUT_GAMEPAD_Y = 0x8000,
};

// For VK_PAD, use ui::VirtualKey.

enum X_INPUT_KEYSTROKE_FLAGS {
  X_INPUT_KEYSTROKE_KEYDOWN = 0x0001,
  X_INPUT_KEYSTROKE_KEYUP = 0x0002,
  X_INPUT_KEYSTROKE_REPEAT = 0x0004,
};

struct X_INPUT_GAMEPAD {
  be<uint16_t> buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  be<int16_t> thumb_lx;
  be<int16_t> thumb_ly;
  be<int16_t> thumb_rx;
  be<int16_t> thumb_ry;
};
static_assert_size(X_INPUT_GAMEPAD, 12);

struct X_INPUT_STATE {
  be<uint32_t> packet_number;
  X_INPUT_GAMEPAD gamepad;
};
static_assert_size(X_INPUT_STATE, sizeof(X_INPUT_GAMEPAD) + 4);

struct X_INPUT_VIBRATION {
  be<uint16_t> left_motor_speed;
  be<uint16_t> right_motor_speed;
};
static_assert_size(X_INPUT_VIBRATION, 4);

struct X_INPUT_CAPABILITIES {
  uint8_t type;
  uint8_t sub_type;
  be<uint16_t> flags;
  X_INPUT_GAMEPAD gamepad;
  X_INPUT_VIBRATION vibration;
};
static_assert_size(X_INPUT_CAPABILITIES,
                   sizeof(X_INPUT_GAMEPAD) + sizeof(X_INPUT_VIBRATION) + 4);

struct X_INPUT_CAPABILITIES_EX : X_INPUT_CAPABILITIES {
  uint8_t unk[0xC];
};
static_assert_size(X_INPUT_CAPABILITIES_EX, sizeof(X_INPUT_CAPABILITIES) + 0xC);

// https://learn.microsoft.com/en-gb/windows/win32/api/xinput/nf-xinput-xinputgetkeystroke
struct X_INPUT_KEYSTROKE {
  be<uint16_t> virtual_key;
  be<uint16_t> unicode;
  be<uint16_t> flags;
  uint8_t user_index;
  uint8_t hid_code;
};
static_assert_size(X_INPUT_KEYSTROKE, 8);

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_H_
