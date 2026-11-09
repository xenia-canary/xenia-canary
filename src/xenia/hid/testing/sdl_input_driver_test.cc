/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The SDL driver's own add / poll / event / remove paths, driven by SDL
// virtual joysticks (no hardware, no window). Events reach the driver the
// way they do in Xenia: from an SDL event watch, called while SDL updates
// the device, on the thread that updates it.

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "SDL.h"
#include "xenia/base/cvar.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/sdl/sdl_input_driver.h"
#include "xenia/ui/virtual_key.h"

#include "third_party/catch/include/catch.hpp"

DECLARE_string(controller_subtypes);
DECLARE_string(guitar_whammy_on_stick);

namespace xe {
namespace hid {

// Defined inside xe::hid (input_system.cc), so declared there too.
DECLARE_double(left_stick_deadzone_percentage);

namespace sdl {

// Stands in for SDLInputDriver::Setup, which needs Xenia's UI thread: the
// same event-watch filter, calling the driver's own HandleEvent.
class SDLInputDriverTestAccess {
 public:
  static void Start(SDLInputDriver& driver) {
    driver.sdl_events_initialized_ = true;
    driver.sdl_gamecontroller_initialized_ = true;
    SDL_AddEventWatch(&Watch, &driver);
  }
  static void Stop(SDLInputDriver& driver) {
    SDL_DelEventWatch(&Watch, &driver);
  }
  // What the UI thread runs for each pump GetState queues.
  static void Pump(SDLInputDriver& driver) { driver.PumpEvents(); }

 private:
  static int SDLCALL Watch(void* userdata, SDL_Event* event) {
    const auto type = event->type;
    if (type < SDL_JOYAXISMOTION || type >= SDL_FINGERDOWN) {
      return 0;
    }
    static_cast<SDLInputDriver*>(userdata)->HandleEvent(*event);
    return 0;
  }
};

namespace test {

using Access = SDLInputDriverTestAccess;

uint16_t U16(int16_t value) { return static_cast<uint16_t>(value); }

// Raw joystick values for the virtual device: SDL's game controller layer
// maps a full-range joystick axis to a trigger 0..32767, so rest is -32768.
constexpr Sint16 kTriggerRest = -32768;

class DriverFixture {
 public:
  explicit DriverFixture(const char* controller_subtypes)
      : saved_subtypes_(::cvars::controller_subtypes) {
    ::cvars::controller_subtypes = controller_subtypes;
    REQUIRE(SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) == 0);
    // Owned by an InputSystem, as in Xenia, so polls can also go through it.
    auto driver = std::make_unique<SDLInputDriver>(nullptr, 0);
    driver_ = driver.get();
    input_system_ = std::make_unique<InputSystem>(nullptr);
    input_system_->AddDriver(std::move(driver));
    Access::Start(*driver_);
  }
  ~DriverFixture() {
    for (int index = SDL_NumJoysticks() - 1; index >= 0; --index) {
      if (SDL_JoystickIsVirtual(index)) {
        SDL_JoystickDetachVirtual(index);
      }
    }
    Access::Stop(*driver_);
    // The driver closes its controllers and quits the subsystems initialized
    // above.
    input_system_.reset();
    ::cvars::controller_subtypes = saved_subtypes_;
  }

  SDLInputDriver& driver() { return *driver_; }
  InputSystem& input_system() { return *input_system_; }

  // Attaches a virtual device with SDL's game controller layout: one axis
  // per bit of `axis_mask`, in SDL_GameControllerAxis order (all six:
  // leftx, lefty, rightx, righty, lefttrigger, righttrigger = a0..a5).
  // SDL posts the add events, and the driver opens it, inside this call.
  // Returns the joystick (the driver's own handle). SDL_JoystickOpen reads
  // the device once while opening it, so SDL knows every axis's value from
  // here on (all 0 until set), without having posted an event for any.
  SDL_Joystick* Attach(SDL_JoystickType type,
                       uint32_t axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) -
                                            1) {
    SDL_VirtualJoystickDesc desc = {};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = static_cast<Uint16>(type);
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
      desc.naxes = static_cast<Uint16>(desc.naxes + ((axis_mask >> axis) & 1));
    }
    desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
    desc.vendor_id = 0x045E;
    desc.product_id = 0x02A1;
    desc.axis_mask = axis_mask;
    desc.button_mask = (1u << (SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1)) - 1;
    desc.name = "Xenia test device";
    if (type != SDL_JOYSTICK_TYPE_GAMECONTROLLER) {
      // SDL maps only a virtual "game controller" by itself; give any other
      // type the same layout by GUID (taken from a first, unopened attach).
      REQUIRE(axis_mask == (1u << SDL_CONTROLLER_AXIS_MAX) - 1);
      const int probe = SDL_JoystickAttachVirtualEx(&desc);
      REQUIRE(probe >= 0);
      char guid[33] = {};
      SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(probe), guid,
                                sizeof(guid));
      REQUIRE(SDL_JoystickDetachVirtual(probe) == 0);
      const std::string mapping =
          std::string(guid) +
          ",Xenia test device,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
          "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,"
          "dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,"
          "rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,";
      REQUIRE(SDL_GameControllerAddMapping(mapping.c_str()) >= 0);
    }
    const int index = SDL_JoystickAttachVirtualEx(&desc);
    REQUIRE(index >= 0);
    SDL_Joystick* joystick =
        SDL_JoystickFromInstanceID(SDL_JoystickGetDeviceInstanceID(index));
    REQUIRE(joystick != nullptr);  // opened by the driver
    return joystick;
  }

  // The device's report: what SDL_JoystickUpdate delivers next.
  static void SetAxis(SDL_Joystick* joystick, SDL_GameControllerAxis axis,
                      Sint16 value) {
    REQUIRE(SDL_JoystickSetVirtualAxis(joystick, axis, value) == 0);
  }

  static void DetachAll() {
    for (int index = SDL_NumJoysticks() - 1; index >= 0; --index) {
      if (SDL_JoystickIsVirtual(index)) {
        REQUIRE(SDL_JoystickDetachVirtual(index) == 0);
      }
    }
  }

  // A poll as Xenia sees it: the pump the previous poll queued has run.
  X_INPUT_STATE State(uint32_t user_index = 0) {
    Access::Pump(*driver_);
    X_INPUT_STATE state = {};
    REQUIRE(driver_->GetState(user_index, &state) == X_ERROR_SUCCESS);
    return state;
  }

 private:
  std::string saved_subtypes_;
  std::unique_ptr<InputSystem> input_system_;
  SDLInputDriver* driver_ = nullptr;
};

TEST_CASE("DriverDisconnectAndReconnect", "[hid][sdl]") {
  DriverFixture fixture("");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  CHECK(U16(fixture.State().gamepad.thumb_lx) == 0x0A01);

  // The device sleeps: SDL closes it under the driver.
  DriverFixture::DetachAll();
  X_INPUT_STATE state = {};
  CHECK(fixture.driver().GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  X_INPUT_CAPABILITIES caps = {};
  CHECK(fixture.driver().GetCapabilities(0, 0, &caps) ==
        X_ERROR_DEVICE_NOT_CONNECTED);

  // And wakes up again in the same slot, with nothing left over.
  fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  CHECK(U16(fixture.State().gamepad.thumb_lx) == 0);
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(uint32_t(caps.sub_type) == XINPUT_DEVSUBTYPE_GAMEPAD);
}

TEST_CASE("DriverGuitarWhammyRestUnchanged", "[hid][sdl]") {
  DriverFixture fixture("");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GUITAR);
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(uint32_t(caps.sub_type) == XINPUT_DEVSUBTYPE_GUITAR);

  // Untouched, the whammy (right stick X) reads as the bar at rest.
  CHECK(int(fixture.State().gamepad.thumb_rx) == -32768);
  CHECK(int(fixture.State().gamepad.thumb_ry) == 0);  // Y is not seeded

  // Once it moves, the device's value stands, also after a refresh.
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 12000);
  SDL_JoystickUpdate();
  CHECK(int(fixture.State().gamepad.thumb_rx) == 12000);
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(int(fixture.State().gamepad.thumb_rx) == 12000);
}

// Sets guitar_whammy_on_stick for one test.
class WhammyOnStick {
 public:
  explicit WhammyOnStick(const char* slots)
      : saved_(::cvars::guitar_whammy_on_stick) {
    ::cvars::guitar_whammy_on_stick = slots;
  }
  ~WhammyOnStick() { ::cvars::guitar_whammy_on_stick = saved_; }

 private:
  std::string saved_;
};

TEST_CASE("DriverForcedGuitarWhammyOnStick", "[hid][sdl]") {
  // A device SDL calls a game controller, reported as a guitar through
  // controller_subtypes, whose whammy bar is its left trigger.
  WhammyOnStick whammy("0");
  DriverFixture fixture("0:guitar");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(uint32_t(caps.sub_type) == XINPUT_DEVSUBTYPE_GUITAR);
  CHECK(int(fixture.State().gamepad.thumb_rx) == -32768);  // bar at rest

  // The trigger's range is spread over the whole right stick X, and the
  // trigger itself stays released.
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
  SDL_JoystickUpdate();
  X_INPUT_STATE state = fixture.State();
  CHECK(int(state.gamepad.thumb_rx) == 32766);
  CHECK(uint32_t(state.gamepad.left_trigger) == 0);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
                         kTriggerRest);
  SDL_JoystickUpdate();
  CHECK(int(fixture.State().gamepad.thumb_rx) == -32768);

  // The right stick X's own input (a tilt sensor on some guitars) is
  // dropped.
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 12000);
  SDL_JoystickUpdate();
  CHECK(int(fixture.State().gamepad.thumb_rx) == -32768);
}

TEST_CASE("DriverWhammyOnStickOnlyForItsSlot", "[hid][sdl]") {
  // Slot 0 asks for it, but the guitar is in slot 1: nothing changes there.
  WhammyOnStick whammy("0");
  DriverFixture fixture("0:guitar,1:guitar");
  fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 12000);
  SDL_JoystickUpdate();
  const X_INPUT_STATE state = fixture.State(1);
  CHECK(uint32_t(state.gamepad.left_trigger) == 0xFF);
  CHECK(int(state.gamepad.thumb_rx) == 12000);
}

// Waits up to two seconds for `done`; false if it never came.
bool WaitFor(const std::function<bool()>& done) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// A guitar in slot 0 and the device `controller_subtypes` names in slot 1,
// both connected. While another thread holds SDL's joystick lock, as SDL
// does through every device update and detection (HIDAPI device init sleeps
// under it), the polls still return at once, with the cached state.
void PollDoesNotWaitForSdl(const char* controller_subtypes) {
  DriverFixture fixture(controller_subtypes);
  SDL_Joystick* guitar = fixture.Attach(SDL_JOYSTICK_TYPE_GUITAR);
  SDL_Joystick* second = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(guitar, SDL_CONTROLLER_AXIS_RIGHTX, 12000);
  DriverFixture::SetAxis(second, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  const X_INPUT_STATE guitar_before = fixture.State(0);
  const X_INPUT_STATE second_before = fixture.State(1);
  REQUIRE(int(guitar_before.gamepad.thumb_rx) == 12000);

  std::promise<void> locked;
  std::promise<void> release;
  std::thread sdl_busy([&] {
    SDL_LockJoysticks();
    locked.set_value();
    release.get_future().wait();
    SDL_UnlockJoysticks();
  });
  locked.get_future().wait();

  X_INPUT_STATE guitar_during = {};
  X_INPUT_STATE second_during = {};
  X_RESULT keystroke_result = X_ERROR_SUCCESS;
  auto polls = std::async(std::launch::async, [&] {
    X_INPUT_KEYSTROKE keystroke = {};
    const bool ok =
        fixture.driver().GetState(0, &guitar_during) == X_ERROR_SUCCESS &&
        fixture.driver().GetState(1, &second_during) == X_ERROR_SUCCESS;
    keystroke_result = fixture.driver().GetKeystroke(0, 0, &keystroke);
    return ok;
  });
  const bool prompt =
      polls.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  sdl_busy.join();
  CHECK(prompt);
  CHECK(polls.get());
  CHECK(keystroke_result != X_ERROR_DEVICE_NOT_CONNECTED);
  CHECK(std::memcmp(&guitar_during, &guitar_before, sizeof(X_INPUT_STATE)) ==
        0);
  CHECK(std::memcmp(&second_during, &second_before, sizeof(X_INPUT_STATE)) ==
        0);
}

TEST_CASE("DriverPollDoesNotWaitForSdl", "[hid][sdl]") {
  PollDoesNotWaitForSdl("");
}

// A guest thread polls while SDL delivers events on this thread. Every packet
// number must stand for exactly one state.
void PacketNumberNeverRepeatsWithNewState(const char* controller_subtypes) {
  DriverFixture fixture(controller_subtypes);
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);

  std::atomic<bool> stop = false;
  std::atomic<int> mismatches = 0;
  std::atomic<int> polls = 0;
  std::map<uint32_t, X_INPUT_GAMEPAD> seen;  // the guest's; read after join
  std::thread guest([&] {
    while (!stop) {
      X_INPUT_STATE state = {};
      if (fixture.driver().GetState(0, &state) != X_ERROR_SUCCESS) {
        continue;
      }
      const auto [it, inserted] =
          seen.emplace(uint32_t(state.packet_number), state.gamepad);
      if (!inserted &&
          std::memcmp(&it->second, &state.gamepad, sizeof(state.gamepad))) {
        ++mismatches;
      }
      ++polls;
    }
  });
  constexpr int kChanges = 2000;
  bool guest_kept_up = true;
  for (int i = 0; i < kChanges && guest_kept_up; ++i) {
    DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX,
                           static_cast<Sint16>(0x0A00 | (1 << (i % 8))));
    DriverFixture::SetAxis(
        joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
        static_cast<Sint16>(((0x80 >> (i % 8)) * 257) - 32768));
    Access::Pump(fixture.driver());
    // Wait for a poll that started after this change.
    const int after = polls + 2;
    guest_kept_up = WaitFor([&] { return polls >= after; });
  }
  stop = true;
  guest.join();
  CHECK(guest_kept_up);
  CHECK(mismatches == 0);
  // Every change was seen as a state of its own.
  CHECK(seen.size() >= size_t(kChanges));
}

TEST_CASE("DriverPacketNumberNeverRepeatsWithNewState", "[hid][sdl]") {
  PacketNumberNeverRepeatsWithNewState("");
}

// The device sleeps and wakes while a guest thread keeps polling. Each phase
// waits until the guest has polled in it.
void PollingSurvivesConnectCycles(const char* controller_subtypes) {
  DriverFixture fixture(controller_subtypes);
  fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);

  std::atomic<bool> stop = false;
  std::atomic<int> connected_polls = 0;
  std::atomic<int> disconnected_polls = 0;
  std::thread guest([&] {
    while (!stop) {
      X_INPUT_STATE state = {};
      X_INPUT_CAPABILITIES caps = {};
      const X_RESULT result = fixture.driver().GetState(0, &state);
      fixture.driver().GetCapabilities(0, 0, &caps);
      if (result == X_ERROR_SUCCESS) {
        ++connected_polls;
      } else if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
        ++disconnected_polls;
      }
    }
  });
  constexpr int kCycles = 50;
  bool guest_kept_up = true;
  for (int cycle = 0; cycle < kCycles && guest_kept_up; ++cycle) {
    DriverFixture::DetachAll();
    const int seen_disconnected = disconnected_polls + 2;
    guest_kept_up =
        WaitFor([&] { return disconnected_polls >= seen_disconnected; });
    SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
    DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
    Access::Pump(fixture.driver());
    const int seen_connected = connected_polls + 2;
    guest_kept_up = guest_kept_up &&
                    WaitFor([&] { return connected_polls >= seen_connected; });
  }
  DriverFixture::DetachAll();
  stop = true;
  guest.join();
  CHECK(guest_kept_up);
  CHECK(connected_polls >= 2 * kCycles);
  CHECK(disconnected_polls >= 2 * kCycles);
  X_INPUT_STATE state = {};
  CHECK(fixture.driver().GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("DriverPollingSurvivesConnectCycles", "[hid][sdl]") {
  PollingSurvivesConnectCycles("");
}

// SDL takes the device's values without posting axis events. That is what
// happens to an axis that has not moved since the device opened, such as an
// idle keyboard instrument's RY. (A change SDL's jitter filter drops is
// different: SDL does not take its value either. Virtual devices are exempt
// from that filter, so these tests cannot show it.)
struct AxisEventsOff {
  AxisEventsOff() { SDL_EventState(SDL_JOYAXISMOTION, SDL_IGNORE); }
  ~AxisEventsOff() { SDL_EventState(SDL_JOYAXISMOTION, SDL_ENABLE); }
};

// The values SDL reads for an idle keyboard instrument.
void SetKeyboardIdle(SDL_Joystick* joystick) {
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, 0);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 0);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, -32513);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
                         kTriggerRest);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
                         kTriggerRest);
}

TEST_CASE("DriverKeyboardStartsIdleWhereSdlHasNoValue", "[hid][sdl]") {
  // A device without a right stick Y: SDL has no value for RY, and GetAxis
  // reads 0, which taken at face value is overdrive + pedal (~0 = 0xFFFF).
  // (A bound axis before SDL's first report is the other "no value" case.
  // Virtual devices cannot produce it: SDL_JoystickOpen reads the device
  // once, and the virtual driver always reports every axis, so that branch
  // is covered by KeyboardAxesNotSeededFromSdlPlaceholder only.)
  DriverFixture fixture("0:keyboard");
  constexpr uint32_t kNoRightY = ((1u << SDL_CONTROLLER_AXIS_MAX) - 1) &
                                 ~(1u << SDL_CONTROLLER_AXIS_RIGHTY);
  SDL_Joystick* joystick =
      fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER, kNoRightY);
  SDL_GameController* controller =
      SDL_GameControllerFromInstanceID(SDL_JoystickInstanceID(joystick));
  REQUIRE(controller != nullptr);
  CHECK(SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY)
            .bindType == SDL_CONTROLLER_BINDTYPE_NONE);
  CHECK(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) == 0);

  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(uint32_t(caps.type) == XINPUT_DEVTYPE_GAMEPAD);
  CHECK(uint32_t(caps.sub_type) == XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT);

  {
    AxisEventsOff no_events;
    // Five joystick axes: the triggers are a3 and a4 here.
    REQUIRE(SDL_JoystickSetVirtualAxis(joystick, 3, kTriggerRest) == 0);
    REQUIRE(SDL_JoystickSetVirtualAxis(joystick, 4, kTriggerRest) == 0);
    SDL_JoystickUpdate();
  }
  const X_INPUT_STATE state = fixture.State();
  CHECK(U16(state.gamepad.thumb_ry) == 0x7F00);  // idle, not 0xFFFF
  CHECK(uint32_t(state.gamepad.left_trigger) == 0);
  CHECK(uint32_t(state.gamepad.right_trigger) == 0);
}

TEST_CASE("DriverKeyboardFollowsSdlWithoutEvents", "[hid][sdl]") {
  DriverFixture fixture("0:keyboard");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  // At add SDL already has the device's values (all 0) and has posted no
  // event; the keyboard shows SDL's values (RY 0 -> ~0).
  const X_INPUT_STATE at_add = fixture.State();
  CHECK(U16(at_add.gamepad.thumb_ry) == 0xFFFF);
  const uint32_t first_packet = at_add.packet_number;
  // Nothing changed, so the packet number stays.
  CHECK(uint32_t(fixture.State().packet_number) == first_packet);

  {
    AxisEventsOff no_events;
    SetKeyboardIdle(joystick);
    // Overdrive held (RY 0x807F) and key 24 (LX 0x0A01): SDL has the values
    // but posts no event for them.
    DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, -32641);
    DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
    SDL_JoystickUpdate();
  }
  Sint16 unused = 0;
  REQUIRE(SDL_JoystickGetAxisInitialState(joystick, SDL_CONTROLLER_AXIS_RIGHTY,
                                          &unused) == SDL_TRUE);

  const X_INPUT_STATE state = fixture.State();
  CHECK(U16(state.gamepad.thumb_ry) == 0x7F80);
  CHECK(U16(state.gamepad.thumb_lx) == 0x0A01);
  CHECK(uint32_t(state.gamepad.left_trigger) == 0);
  // A title may only reread a pad whose packet number moved.
  CHECK(uint32_t(state.packet_number) != first_packet);
  CHECK(uint32_t(fixture.State().packet_number) ==
        uint32_t(state.packet_number));
}

TEST_CASE("DriverKeyboardKeepsAnEventValue", "[hid][sdl]") {
  DriverFixture fixture("0:keyboard");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  {
    AxisEventsOff no_events;
    SetKeyboardIdle(joystick);
    SDL_JoystickUpdate();
  }
  CHECK(U16(fixture.State().gamepad.thumb_ry) == 0x7F00);

  // Overdrive pressed, with events: the driver takes the event.
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, -32641);
  SDL_JoystickUpdate();
  const X_INPUT_STATE pressed = fixture.State();
  CHECK(U16(pressed.gamepad.thumb_ry) == 0x7F80);

  // A key on the left trigger (key 1 = byte 0x80, raw 0x80 * 257 - 32768).
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 128);
  SDL_JoystickUpdate();
  CHECK(uint32_t(fixture.State().gamepad.left_trigger) == 0x80);

  // SDL's value moves without an event: an axis that has had an event keeps
  // the event's value; it is not re-seeded from SDL.
  {
    AxisEventsOff no_events;
    DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, -32513);
    SDL_JoystickUpdate();
  }
  CHECK(U16(fixture.State().gamepad.thumb_ry) == 0x7F80);
}

TEST_CASE("DriverKeyboardSendsNoAnalogKeystrokes", "[hid][sdl]") {
  // The idle RY (0x7F00) is past the thumb threshold. On a gamepad that is
  // the right stick held up, which XInputGetKeystroke reports.
  {
    DriverFixture fixture("");
    SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
    SetKeyboardIdle(joystick);
    SDL_JoystickUpdate();
    REQUIRE(U16(fixture.State().gamepad.thumb_ry) == 0x7F00);
    X_INPUT_KEYSTROKE keystroke = {};
    REQUIRE(fixture.driver().GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
    CHECK(uint32_t(keystroke.virtual_key) ==
          uint32_t(ui::VirtualKey::kXInputPadRThumbUp));
  }

  // A keyboard instrument's axes are bitfields: no keystrokes from them, not
  // even with a key held. Its buttons still send theirs.
  DriverFixture fixture("0:keyboard");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  SetKeyboardIdle(joystick);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 128);
  SDL_JoystickUpdate();
  const X_INPUT_STATE state = fixture.State();
  REQUIRE(U16(state.gamepad.thumb_ry) == 0x7F00);
  REQUIRE(uint32_t(state.gamepad.left_trigger) == 0x80);  // key 1
  X_INPUT_KEYSTROKE keystroke = {};
  CHECK(fixture.driver().GetKeystroke(0, 0, &keystroke) == X_ERROR_EMPTY);

  REQUIRE(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A,
                                       SDL_PRESSED) == 0);
  SDL_JoystickUpdate();
  fixture.State();
  REQUIRE(fixture.driver().GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
  CHECK(uint32_t(keystroke.virtual_key) ==
        uint32_t(ui::VirtualKey::kXInputPadA));
}

TEST_CASE("DriverKeyboardIgnoresWhammyOnStick", "[hid][sdl]") {
  // guitar_whammy_on_stick only applies to a guitar: listed for a keyboard
  // slot, its left trigger and right stick X still carry keys and velocities.
  WhammyOnStick whammy("0");
  DriverFixture fixture("0:keyboard");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  SetKeyboardIdle(joystick);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 128);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 0x0A00);
  SDL_JoystickUpdate();
  const X_INPUT_STATE state = fixture.State();
  CHECK(uint32_t(state.gamepad.left_trigger) == 0x80);  // key 1
  CHECK(U16(state.gamepad.thumb_rx) == 0x0A00);
}

TEST_CASE("DriverKeyboardDisconnectAndReconnect", "[hid][sdl]") {
  DriverFixture fixture("0:keyboard");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  SetKeyboardIdle(joystick);
  SDL_JoystickUpdate();
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  CHECK(U16(fixture.State().gamepad.thumb_lx) == 0x0A01);  // key 24 down

  DriverFixture::DetachAll();
  X_INPUT_STATE state = {};
  CHECK(fixture.driver().GetState(0, &state) == X_ERROR_DEVICE_NOT_CONNECTED);

  // Back in the same slot: still a keyboard, idle, no key left held.
  joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  {
    AxisEventsOff no_events;
    SetKeyboardIdle(joystick);
    SDL_JoystickUpdate();
  }
  state = fixture.State();
  CHECK(U16(state.gamepad.thumb_lx) == 0);
  CHECK(U16(state.gamepad.thumb_ry) == 0x7F00);
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  CHECK(uint32_t(caps.sub_type) == XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT);
}

// Sets left_stick_deadzone_percentage for one test.
class LeftDeadzone {
 public:
  explicit LeftDeadzone(double value)
      : saved_(cvars::left_stick_deadzone_percentage) {
    cvars::left_stick_deadzone_percentage = value;
  }
  ~LeftDeadzone() { cvars::left_stick_deadzone_percentage = saved_; }

 private:
  double saved_;
};

// Slot 0's left stick X through InputSystem, as XamInputGetState reads it.
uint16_t InputSystemLeftX(DriverFixture& fixture) {
  Access::Pump(fixture.driver());
  X_INPUT_STATE state = {};
  REQUIRE(fixture.input_system().GetState(0, X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  return U16(state.gamepad.thumb_lx);
}

TEST_CASE("DriverKeyboardDeadzoneFollowsPerTitleSubtypes", "[hid][sdl]") {
  // A title's own config asks for "keyboard" while the slot is connected
  // (no failed poll in between). Once the title asks for the capabilities,
  // which reapplies controller_subtypes, the deadzone no longer applies.
  LeftDeadzone deadzone(0.2);
  DriverFixture fixture("");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  CHECK(InputSystemLeftX(fixture) == 0);  // a gamepad: inside the deadzone

  ::cvars::controller_subtypes = "0:keyboard";
  X_INPUT_CAPABILITIES caps = {};
  REQUIRE(fixture.driver().GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS);
  REQUIRE(uint32_t(caps.sub_type) == XINPUT_DEVSUBTYPE_KEYBOARD_INSTRUMENT);
  CHECK(InputSystemLeftX(fixture) == 0x0A01);  // key 24 kept
}

TEST_CASE("DriverKeyboardDeadzoneFollowsSlotReuse", "[hid][sdl]") {
  // A gamepad leaves and a keyboard instrument takes its slot between two
  // polls, so InputSystem never sees the slot empty.
  LeftDeadzone deadzone(0.2);
  DriverFixture fixture("");
  SDL_Joystick* joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  CHECK(InputSystemLeftX(fixture) == 0);

  DriverFixture::DetachAll();
  ::cvars::controller_subtypes = "0:keyboard";
  joystick = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  CHECK(InputSystemLeftX(fixture) == 0x0A01);
}

TEST_CASE("DriverKeyboardDeadzonePollDoesNotWaitForSdl", "[hid][sdl]") {
  // With a deadzone set, polls through InputSystem must not ask the driver
  // whether the slot is a keyboard instrument: SDL's GetCapabilities waits
  // for SDL's joystick lock.
  LeftDeadzone deadzone(0.2);
  DriverFixture fixture("1:keyboard");
  SDL_Joystick* gamepad = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  SDL_Joystick* keyboard = fixture.Attach(SDL_JOYSTICK_TYPE_GAMECONTROLLER);
  DriverFixture::SetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  DriverFixture::SetAxis(keyboard, SDL_CONTROLLER_AXIS_LEFTX, 0x0A01);
  SDL_JoystickUpdate();
  Access::Pump(fixture.driver());
  X_INPUT_STATE state = {};
  // The first polls connect the slots (one capabilities query each). The
  // deadzone zeroes the gamepad's small LX, not the keyboard's key 24.
  REQUIRE(fixture.input_system().GetState(0, X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  CHECK(U16(state.gamepad.thumb_lx) == 0);
  REQUIRE(fixture.input_system().GetState(1, X_INPUT_FLAG_GAMEPAD, &state) ==
          X_ERROR_SUCCESS);
  CHECK(U16(state.gamepad.thumb_lx) == 0x0A01);

  std::promise<void> locked;
  std::promise<void> release;
  std::thread sdl_busy([&] {
    SDL_LockJoysticks();
    locked.set_value();
    release.get_future().wait();
    SDL_UnlockJoysticks();
  });
  locked.get_future().wait();
  auto polls = std::async(std::launch::async, [&] {
    X_INPUT_STATE gamepad_state = {};
    X_INPUT_STATE keyboard_state = {};
    return fixture.input_system().GetState(0, X_INPUT_FLAG_GAMEPAD,
                                           &gamepad_state) == X_ERROR_SUCCESS &&
           fixture.input_system().GetState(
               1, X_INPUT_FLAG_GAMEPAD, &keyboard_state) == X_ERROR_SUCCESS &&
           U16(keyboard_state.gamepad.thumb_lx) == 0x0A01;
  });
  const bool prompt =
      polls.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release.set_value();
  sdl_busy.join();
  CHECK(prompt);
  CHECK(polls.get());
}

TEST_CASE("DriverKeyboardPollDoesNotWaitForSdl", "[hid][sdl]") {
  PollDoesNotWaitForSdl("1:keyboard");
}

TEST_CASE("DriverKeyboardPacketNumberNeverRepeatsWithNewState", "[hid][sdl]") {
  PacketNumberNeverRepeatsWithNewState("0:keyboard");
}

TEST_CASE("DriverKeyboardPollingSurvivesConnectCycles", "[hid][sdl]") {
  PollingSurvivesConnectCycles("0:keyboard");
}

}  // namespace test
}  // namespace sdl
}  // namespace hid
}  // namespace xe
