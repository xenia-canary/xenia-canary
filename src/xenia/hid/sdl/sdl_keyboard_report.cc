/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/sdl/sdl_keyboard_report.h"

#include "xenia/base/assert.h"

namespace xe {
namespace hid {
namespace sdl {

int16_t& SdlAxisValue(SdlAxes& axes, size_t axis) {
  switch (axis) {
    case kSdlAxisLeftX:
      return axes.lx;
    case kSdlAxisLeftY:
      return axes.ly;
    case kSdlAxisRightX:
      return axes.rx;
    case kSdlAxisRightY:
      return axes.ry;
    case kSdlAxisTriggerLeft:
      return axes.lt;
    case kSdlAxisTriggerRight:
      return axes.rt;
    default:
      assert_always();
      return axes.lx;
  }
}

X_INPUT_GAMEPAD KeyboardGamepadFromSdl(const SdlAxes& axes, uint16_t buttons) {
  X_INPUT_GAMEPAD pad = {};
  pad.buttons = buttons;
  // The same transforms as SDLInputDriver::OnControllerDeviceAxisMotion.
  pad.left_trigger = static_cast<uint8_t>(axes.lt >> 7);
  pad.right_trigger = static_cast<uint8_t>(axes.rt >> 7);
  pad.thumb_lx = axes.lx;
  pad.thumb_ly = static_cast<int16_t>(~axes.ly);
  pad.thumb_rx = axes.rx;
  pad.thumb_ry = static_cast<int16_t>(~axes.ry);
  return pad;
}

bool SeedKeyboardAxes(
    SdlAxes* axes, uint8_t axes_with_events,
    const std::array<SdlAxisReading, kSdlAxisCount>& readings) {
  SdlAxes idle = kKeyboardIdleSdlAxes;
  bool changed = false;
  for (size_t axis = 0; axis < kSdlAxisCount; ++axis) {
    if (axes_with_events & (1u << axis)) {
      continue;
    }
    const int16_t value =
        readings[axis].known ? readings[axis].value : SdlAxisValue(idle, axis);
    int16_t& current = SdlAxisValue(*axes, axis);
    if (current != value) {
      current = value;
      changed = true;
    }
  }
  return changed;
}

}  // namespace sdl
}  // namespace hid
}  // namespace xe
