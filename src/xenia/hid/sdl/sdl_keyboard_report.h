/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_SDL_KEYBOARD_REPORT_H_
#define XENIA_HID_SDL_SDL_KEYBOARD_REPORT_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "xenia/hid/input.h"

// A keyboard instrument (XInput gamepad, subtype 15) carries bitfields in
// every axis, not positions. The layout is from PlasticBand's community
// protocol notes,
// https://github.com/TheNathannator/PlasticBand/blob/a7b5ca304969717a6486fa21f1a42fb044d1150f/Docs/Instruments/Pro%20Keyboard/Xbox%20360.md
// and matches the SDL values recorded for one press of each of the 25 keys on
// a real device (sdl_keyboard_report_test.cc):
//   keys 1-8   = left trigger bits, MSB to LSB
//   keys 9-16  = right trigger bits, MSB to LSB
//   keys 17-24 = left stick X low byte, MSB to LSB
//   key 25     = left stick X bit 15
//   velocities = left stick X bits 8-14, left stick Y, right stick X
//   overdrive  = right stick Y bit 7; pedal = right stick Y bits 8-15
// So its state is copied bit for bit: no deadzone, no clamping, no whammy
// handling. This file is free of SDL so it can be tested without a device.

namespace xe {
namespace hid {
namespace sdl {

// One value per SDL game controller axis, as SDL_GameControllerGetAxis and
// SDL_CONTROLLERAXISMOTION report it (triggers 0..32767, sticks -32768..32767).
struct SdlAxes {
  int16_t lx = 0;
  int16_t ly = 0;
  int16_t rx = 0;
  int16_t ry = 0;
  int16_t lt = 0;
  int16_t rt = 0;
};

// Index of each axis; the same numbers as SDL_GameControllerAxis
// (sdl_input_driver.cc checks that).
enum SdlAxis : uint8_t {
  kSdlAxisLeftX = 0,
  kSdlAxisLeftY = 1,
  kSdlAxisRightX = 2,
  kSdlAxisRightY = 3,
  kSdlAxisTriggerLeft = 4,
  kSdlAxisTriggerRight = 5,
  kSdlAxisCount = 6,
};

// The value of one axis; `axis` must be < kSdlAxisCount.
int16_t& SdlAxisValue(SdlAxes& axes, size_t axis);

// What SDL reads for an idle keyboard instrument with no pedal (measured on a
// real device): every axis 0 except RY = -32513 (0x80FF), the pedal bits 8-14
// set ("unplugged"), pedal bit 15 and overdrive bit 7 clear. The keyboard
// starts here until SDL knows an axis's real value.
inline constexpr SdlAxes kKeyboardIdleSdlAxes = {0, 0, 0, -32513, 0, 0};

// The X_INPUT_GAMEPAD a title reads for a keyboard: the same transform Xenia's
// SDL event path applies to every controller (triggers >> 7, which gives back
// the device's byte exactly; both Y axes inverted with ~; X axes as they are),
// and nothing else.
X_INPUT_GAMEPAD KeyboardGamepadFromSdl(const SdlAxes& axes, uint16_t buttons);

// What SDL can say about one axis without having sent an event for it.
struct SdlAxisReading {
  // SDL has had a report for the axis (SDL_JoystickGetAxisInitialState on the
  // joystick axis it is bound to returned true), so `value` is real. Before
  // that SDL holds a zero-filled placeholder, which for a keyboard instrument
  // would read as overdrive, the pedal and 14 keys held down.
  bool known = false;
  // SDL_GameControllerGetAxis.
  int16_t value = 0;
};

// Brings every axis that has not sent an event (bit `1 << axis` clear in
// `axes_with_events`) up to date: the reading's value when it is known, the
// idle value otherwise. An axis that has sent an event keeps the event's
// value. Returns true when any value changed, so the caller can bump the
// packet number (a title that sees the same packet number keeps its old
// state).
bool SeedKeyboardAxes(
    SdlAxes* axes, uint8_t axes_with_events,
    const std::array<SdlAxisReading, kSdlAxisCount>& readings);

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_SDL_KEYBOARD_REPORT_H_
