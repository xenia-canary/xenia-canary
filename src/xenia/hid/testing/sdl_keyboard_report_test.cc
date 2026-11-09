/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "xenia/base/cvar.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/sdl/sdl_keyboard_report.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace hid {

// Defined inside xe::hid (input_system.cc), so declared there too.
DECLARE_double(left_stick_deadzone_percentage);

namespace test {

using sdl::KeyboardGamepadFromSdl;
using sdl::kKeyboardIdleSdlAxes;
using sdl::kSdlAxisCount;
using sdl::kSdlAxisLeftX;
using sdl::kSdlAxisRightY;
using sdl::kSdlAxisTriggerLeft;
using sdl::kSdlAxisTriggerRight;
using sdl::SdlAxes;
using sdl::SdlAxisReading;
using sdl::SeedKeyboardAxes;

uint16_t U16(int16_t value) { return static_cast<uint16_t>(value); }

// Keys held down, bit (n - 1) for key n, decoded from what the title reads
// with the PlasticBand layout: keys 1-8 = left trigger bits MSB to LSB, 9-16 =
// right trigger bits, 17-24 = left stick X low byte MSB to LSB, 25 = left
// stick X bit 15.
uint32_t KeysDown(const X_INPUT_GAMEPAD& pad) {
  const uint8_t lt = pad.left_trigger;
  const uint8_t rt = pad.right_trigger;
  const uint16_t lx = U16(pad.thumb_lx);
  uint32_t keys = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t mask = 0x80u >> i;
    if (lt & mask) {
      keys |= 1u << i;
    }
    if (rt & mask) {
      keys |= 1u << (8 + i);
    }
    if (lx & mask) {
      keys |= 1u << (16 + i);
    }
  }
  if (lx & 0x8000u) {
    keys |= 1u << 24;
  }
  return keys;
}

uint32_t Key(uint32_t n) { return 1u << (n - 1); }

// The SDL values recorded for one press of each key on a real keyboard
// instrument (wireless receiver, Linux evdev through Wine to SDL 2.32): the
// axis carrying the key, and the left stick X value at that moment (it
// carries the first key's velocity).
struct RecordedPress {
  uint32_t key;
  uint16_t lx;
  int16_t lt;
  int16_t rt;
};
constexpr std::array<RecordedPress, 25> kRecordedPresses = {{
    {1, 0x0A00, 16447, 0}, {2, 0x0A00, 8223, 0},  {3, 0x6100, 4111, 0},
    {4, 0x0A00, 2055, 0},  {5, 0x7100, 1027, 0},  {6, 0x0A00, 513, 0},
    {7, 0x4100, 256, 0},   {8, 0x0A00, 128, 0},   {9, 0x0A00, 0, 16447},
    {10, 0x1300, 0, 8223}, {11, 0x4100, 0, 4111}, {12, 0x0A00, 0, 2055},
    {13, 0x1100, 0, 1027}, {14, 0x5100, 0, 513},  {15, 0x0A00, 0, 256},
    {16, 0x0A00, 0, 128},  {17, 0x3180, 0, 0},    {18, 0x0A40, 0, 0},
    {19, 0x4320, 0, 0},    {20, 0x7110, 0, 0},    {21, 0x0A08, 0, 0},
    {22, 0x7104, 0, 0},    {23, 0x0A02, 0, 0},    {24, 0x0A01, 0, 0},
    {25, 0x8A00, 0, 0},
}};

TEST_CASE("KeyCBitsRoundTrip", "[hid][keyboard]") {
  for (const auto& press : kRecordedPresses) {
    INFO("key " << press.key);
    SdlAxes axes = kKeyboardIdleSdlAxes;
    axes.lx = static_cast<int16_t>(press.lx);
    axes.lt = press.lt;
    axes.rt = press.rt;
    const X_INPUT_GAMEPAD pad = KeyboardGamepadFromSdl(axes, 0);
    CHECK(KeysDown(pad) == Key(press.key));
  }

  // Every trigger byte SDL can deliver (value = byte * 32767 / 255, as
  // recorded) comes back as the same byte, so any chord of keys 1-16 does too.
  for (uint32_t byte = 0; byte <= 0xFF; ++byte) {
    INFO("trigger byte " << byte);
    SdlAxes axes = kKeyboardIdleSdlAxes;
    axes.lt = static_cast<int16_t>(byte * 32767 / 255);
    axes.rt = static_cast<int16_t>(byte * 32767 / 255);
    const X_INPUT_GAMEPAD pad = KeyboardGamepadFromSdl(axes, 0);
    CHECK(uint32_t(pad.left_trigger) == byte);
    CHECK(uint32_t(pad.right_trigger) == byte);
  }
}

TEST_CASE("KeyboardIdleReportIsAllKeysUp", "[hid][keyboard]") {
  // SDL reads lx = ly = rx = lt = rt = 0 and ry = -32513 (0x80FF) for an
  // idle device (recorded).
  CHECK(kKeyboardIdleSdlAxes.lx == 0);
  CHECK(kKeyboardIdleSdlAxes.ly == 0);
  CHECK(kKeyboardIdleSdlAxes.rx == 0);
  CHECK(kKeyboardIdleSdlAxes.ry == -32513);
  CHECK(kKeyboardIdleSdlAxes.lt == 0);
  CHECK(kKeyboardIdleSdlAxes.rt == 0);

  const X_INPUT_GAMEPAD pad = KeyboardGamepadFromSdl(kKeyboardIdleSdlAxes, 0);
  CHECK(uint32_t(pad.left_trigger) == 0);
  CHECK(uint32_t(pad.right_trigger) == 0);
  CHECK(U16(pad.thumb_lx) == 0x0000);
  // ~0, what the event path stores for SDL's 0, not necessarily what the
  // device sends (it may be 0x0000); LY carries only velocities, so either is
  // harmless.
  CHECK(U16(pad.thumb_ly) == 0xFFFF);
  CHECK(U16(pad.thumb_rx) == 0x0000);
  // ~0x80FF: pedal bits 8-14 set (unplugged), pedal bit 15 and overdrive
  // bit 7 clear.
  CHECK(U16(pad.thumb_ry) == 0x7F00);
  CHECK((U16(pad.thumb_ry) & 0x8000) == 0);
  CHECK((U16(pad.thumb_ry) & 0x0080) == 0);
  CHECK(KeysDown(pad) == 0);
  CHECK(U16(pad.buttons) == 0);
}

// Every axis known to SDL with an idle device's values.
std::array<SdlAxisReading, kSdlAxisCount> IdleReadings() {
  std::array<SdlAxisReading, kSdlAxisCount> readings = {};
  for (auto& reading : readings) {
    reading = {true, 0};
  }
  readings[kSdlAxisRightY] = {true, -32513};
  return readings;
}

TEST_CASE("KeyboardAxesSeededWithoutEvents", "[hid][keyboard]") {
  // The state starts zeroed at device add; SDL never sends an event for an
  // idle device's RY, only SDL_GameControllerGetAxis shows -32513.
  SdlAxes axes = {};
  uint8_t axes_with_events = 0;
  auto readings = IdleReadings();

  CHECK(SeedKeyboardAxes(&axes, axes_with_events, readings));
  CHECK(U16(KeyboardGamepadFromSdl(axes, 0).thumb_ry) == 0x7F00);
  // Nothing new: nothing changed, so the packet number need not move.
  CHECK_FALSE(SeedKeyboardAxes(&axes, axes_with_events, readings));

  // An axis without events keeps following SDL.
  readings[kSdlAxisTriggerLeft] = {true, 16447};
  CHECK(SeedKeyboardAxes(&axes, axes_with_events, readings));
  CHECK(uint32_t(KeyboardGamepadFromSdl(axes, 0).left_trigger) == 0x80);

  // RY sends an event (overdrive pressed: 0x807F). From then on it holds the
  // event's value and is not re-seeded from the reading.
  axes.ry = -32641;
  axes_with_events =
      static_cast<uint8_t>(axes_with_events | (1u << kSdlAxisRightY));
  CHECK_FALSE(SeedKeyboardAxes(&axes, axes_with_events, readings));
  CHECK(axes.ry == -32641);
  CHECK(U16(KeyboardGamepadFromSdl(axes, 0).thumb_ry) == 0x7F80);
}

TEST_CASE("KeyboardAxesNotSeededFromSdlPlaceholder", "[hid][keyboard]") {
  // At device add SDL may not have had the first report yet: it has
  // zero-filled the joystick axes, has_initial_value is false, and
  // SDL_GameControllerGetAxis reads 0 for the sticks and 16383 for the
  // triggers. Taken at face value that is overdrive + pedal (RY 0xFFFF) and
  // 7 keys held on each trigger (0x7F).
  SdlAxes axes = {};
  std::array<SdlAxisReading, kSdlAxisCount> readings = {};
  readings[kSdlAxisTriggerLeft] = {false, 16383};
  readings[kSdlAxisTriggerRight] = {false, 16383};

  CHECK(SeedKeyboardAxes(&axes, 0, readings));
  const X_INPUT_GAMEPAD pad = KeyboardGamepadFromSdl(axes, 0);
  CHECK(U16(pad.thumb_ry) != 0xFFFF);
  CHECK(uint32_t(pad.left_trigger) != 0x7F);
  CHECK(uint32_t(pad.right_trigger) != 0x7F);
  // It starts at the idle values instead.
  CHECK(U16(pad.thumb_ry) == 0x7F00);
  CHECK(KeysDown(pad) == 0);

  // Once SDL has the axis's first report, its value is used.
  readings[kSdlAxisRightY] = {true, -32641};
  CHECK(SeedKeyboardAxes(&axes, 0, readings));
  CHECK(U16(KeyboardGamepadFromSdl(axes, 0).thumb_ry) == 0x7F80);
}

TEST_CASE("KeyboardLxIsCopiedBitExact", "[hid][keyboard]") {
  SdlAxes axes = kKeyboardIdleSdlAxes;

  // Keys 17 and 25 with velocity 127: not clamped, not sign adjusted.
  axes.lx = static_cast<int16_t>(0xFF80);
  X_INPUT_GAMEPAD pad = KeyboardGamepadFromSdl(axes, 0);
  CHECK(U16(pad.thumb_lx) == 0xFF80);
  CHECK(KeysDown(pad) == (Key(17) | Key(25)));

  // Velocity 127 does not flip neighbouring key bits.
  axes.lx = 0x7F01;
  pad = KeyboardGamepadFromSdl(axes, 0);
  CHECK(U16(pad.thumb_lx) == 0x7F01);
  CHECK(KeysDown(pad) == Key(24));

  axes.lx = static_cast<int16_t>(0x8000);
  pad = KeyboardGamepadFromSdl(axes, 0);
  CHECK(U16(pad.thumb_lx) == 0x8000);
  CHECK(KeysDown(pad) == Key(25));

  // The buttons pass through.
  pad = KeyboardGamepadFromSdl(axes, X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_START);
  CHECK(U16(pad.buttons) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_START));
}

// A driver with one device in slot 0. It implements only the InputDriver
// interface's required functions.
class FakeDriver final : public InputDriver {
 public:
  FakeDriver(uint8_t sub_type, const X_INPUT_STATE& state)
      : InputDriver(nullptr, 0), sub_type_(sub_type), state_(state) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (capabilities_error_) {
      return capabilities_error_;
    }
    *out_caps = {};
    out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
    out_caps->sub_type = sub_type_;
    // A positive maximum, so the deadzone threshold is positive and can fire.
    out_caps->gamepad.thumb_lx = 0x7FFF;
    out_caps->gamepad.thumb_ly = 0x7FFF;
    out_caps->gamepad.thumb_rx = 0x7FFF;
    out_caps->gamepad.thumb_ry = 0x7FFF;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    if (user_index != 0) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *out_state = state_;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  InputType GetInputType() const override { return InputType::Controller; }

  // Another device, or the same one under another configuration, from the
  // next call on; GetState keeps succeeding throughout.
  void set_sub_type(uint8_t sub_type) { sub_type_ = sub_type; }
  void set_capabilities_error(X_RESULT error) { capabilities_error_ = error; }

 private:
  uint8_t sub_type_;
  X_INPUT_STATE state_;
  X_RESULT capabilities_error_ = X_ERROR_SUCCESS;
};

struct LeftDeadzone {
  explicit LeftDeadzone(double value)
      : saved(cvars::left_stick_deadzone_percentage) {
    cvars::left_stick_deadzone_percentage = value;
  }
  ~LeftDeadzone() { cvars::left_stick_deadzone_percentage = saved; }
  double saved;
};

uint16_t LeftXThroughInputSystem(uint8_t sub_type, int16_t lx) {
  X_INPUT_STATE state = {};
  state.packet_number = 1;
  state.gamepad.thumb_lx = lx;
  InputSystem input_system(nullptr);
  input_system.AddDriver(std::make_unique<FakeDriver>(sub_type, state));
  X_INPUT_STATE out = {};
  REQUIRE(input_system.GetState(0, X_INPUT_FLAG_GAMEPAD, &out) ==
          X_ERROR_SUCCESS);
  return U16(out.gamepad.thumb_lx);
}

TEST_CASE("KeyboardSlotSkipsDeadzone", "[hid][keyboard]") {
  LeftDeadzone deadzone(0.2);
  // The fixture can fail: on a gamepad, 0x0A01 (key 24, velocity 10) is
  // inside a 20 % deadzone of 0x7FFF and is zeroed.
  CHECK(LeftXThroughInputSystem(XINPUT_DEVSUBTYPE_GAMEPAD, 0x0A01) == 0);
  // On a keyboard the stick is a bitfield and is left alone.
  CHECK(LeftXThroughInputSystem(XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT,
                                0x0A01) == 0x0A01);
}

// Polls slot 0 through InputSystem and returns its left stick X.
uint16_t PollLeftX(InputSystem& input_system) {
  X_INPUT_STATE out = {};
  REQUIRE(input_system.GetState(0, X_INPUT_FLAG_GAMEPAD, &out) ==
          X_ERROR_SUCCESS);
  return U16(out.gamepad.thumb_lx);
}

TEST_CASE("DeadzoneFollowsTheCurrentSubtype", "[hid][keyboard]") {
  // The slot stays connected (no failed poll) while what is in it changes:
  // a per-title controller_subtypes, or another device in the same slot.
  LeftDeadzone deadzone(0.2);
  X_INPUT_STATE state = {};
  state.gamepad.thumb_lx = 0x0A01;
  InputSystem input_system(nullptr);
  auto owned = std::make_unique<FakeDriver>(XINPUT_DEVSUBTYPE_GAMEPAD, state);
  FakeDriver& driver = *owned;
  input_system.AddDriver(std::move(owned));
  CHECK(PollLeftX(input_system) == 0);
  driver.set_sub_type(XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT);
  CHECK(PollLeftX(input_system) == 0x0A01);
  driver.set_sub_type(XINPUT_DEVSUBTYPE_GAMEPAD);
  CHECK(PollLeftX(input_system) == 0);
}

TEST_CASE("DeadzoneRetriesAFailedSubtypeQuery", "[hid][keyboard]") {
  // A capabilities query that fails counts as "not a keyboard" for that
  // poll only; the next poll asks again.
  LeftDeadzone deadzone(0.2);
  X_INPUT_STATE state = {};
  state.gamepad.thumb_lx = 0x0A01;
  InputSystem input_system(nullptr);
  auto owned = std::make_unique<FakeDriver>(
      XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT, state);
  FakeDriver& driver = *owned;
  input_system.AddDriver(std::move(owned));
  CHECK(PollLeftX(input_system) == 0x0A01);
  driver.set_capabilities_error(X_ERROR_FUNCTION_FAILED);
  CHECK(PollLeftX(input_system) == 0);
  driver.set_capabilities_error(X_ERROR_SUCCESS);
  CHECK(PollLeftX(input_system) == 0x0A01);
}

}  // namespace test
}  // namespace hid
}  // namespace xe
