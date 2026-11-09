/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_SDL_INPUT_DRIVER_H_
#define XENIA_HID_SDL_SDL_INPUT_DRIVER_H_

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include "SDL.h"
#include "third_party/rapidcsv/src/rapidcsv.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/sdl/sdl_keyboard_report.h"

#define HID_SDL_USER_COUNT 4
#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace xe {
namespace hid {
namespace sdl {

class SDLInputDriver final : public InputDriver {
 public:
  explicit SDLInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void LoadGameControllerDB();

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  // From the cached slot state: never calls SDL or waits for its lock.
  bool IsKeyboardInstrument(uint32_t user_index) override;
  virtual InputType GetInputType() const override;

 private:
  struct ControllerState {
    SDL_GameController* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    // Reported as a guitar kind.
    bool guitar;
    // The whammy bar arrives as the left trigger.
    bool whammy_on_trigger;
    // The whammy has moved, so the stick holds the device's value.
    bool whammy_seen;
    // Reported as a keyboard instrument (subtype 15): its axes are
    // bitfields, kept bit-exact in keyboard_axes.
    bool keyboard;
    // The keyboard's axes as SDL reports them; the gamepad is built from them.
    sdl::SdlAxes keyboard_axes;
    // Bit (1 << SDL axis) set once that axis has sent an event. Until then
    // the axis follows SDL_GameControllerGetAxis (SDL sends no event for an
    // axis that never moves, such as an idle keyboard instrument's RY).
    uint8_t keyboard_axes_with_events;
  };

  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  void HandleEvent(const SDL_Event& event);
  void OnControllerDeviceAdded(const SDL_Event& event);
  void OnControllerDeviceRemoved(const SDL_Event& event);
  void OnControllerDeviceAxisMotion(const SDL_Event& event);
  void OnControllerDeviceButtonChanged(const SDL_Event& event);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(
      SDL_JoystickID instance_id);
  ControllerState* GetControllerState(uint32_t user_index);
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state, size_t user_index);
  // Brings a keyboard's axes that have not sent an event up to date from SDL
  // (or to the idle values while SDL has had no report for them) and
  // rebuilds its gamepad; marks the state changed when the gamepad changed.
  void RefreshKeyboardAxes(ControllerState& state);
  void QueueControllerUpdate();
  // What the UI thread runs for each pump QueueControllerUpdate queues:
  // SDL_PumpEvents, then the keyboards' refresh from SDL.
  void PumpEvents();
  // The kind controller_subtypes asks for in this slot, if it asks for one.
  static std::optional<uint8_t> ForcedSubtypeForSlot(size_t user_index);
  static bool IsGuitarSubtype(uint8_t sub_type);
  // True when guitar_whammy_on_stick routes this slot's trigger to the stick.
  bool WhammyOnStick(size_t user_index) const;

  bool sdl_events_initialized_;
  bool sdl_gamecontroller_initialized_;
  int sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  std::array<ControllerState, HID_SDL_USER_COUNT> controllers_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;
  // Guards the slots' state (see the lock notes in sdl_input_driver.cc).
  // A leaf lock, always taken after SDL's joystick lock, never before.
  std::mutex state_mutex_;

  // Drives the driver's own event handling without a window (tests only).
  friend class SDLInputDriverTestAccess;
};

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_SDL_INPUT_DRIVER_H_
