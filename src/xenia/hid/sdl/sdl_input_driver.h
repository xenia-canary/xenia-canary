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
#include <unordered_map>
#include <unordered_set>

#include "SDL.h"
#include "third_party/rapidcsv/src/rapidcsv.h"
#include "xenia/hid/input_driver.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window_listener.h"

#define HID_SDL_USER_COUNT 4
#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace xe {
namespace hid {
namespace sdl {

class SDLInputDriver final : public InputDriver,
                             public xe::ui::WindowInputListener {
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
  InputType GetInputType() const override;

  // WindowInputListener overrides:
  void OnKeyDown(xe::ui::KeyEvent& e) override;
  void OnKeyUp(xe::ui::KeyEvent& e) override;
  void OnMouseDown(xe::ui::MouseEvent& e) override;
  void OnMouseUp(xe::ui::MouseEvent& e) override;
  void OnMouseMove(xe::ui::MouseEvent& e) override;

 private:
  struct ControllerState {
    SDL_GameController* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
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
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();
  void GetKeyboardState(X_INPUT_GAMEPAD& pad);

  bool sdl_events_initialized_;
  bool sdl_gamecontroller_initialized_;
  int sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  std::array<ControllerState, HID_SDL_USER_COUNT> controllers_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;

  std::mutex input_mutex_;
  std::unordered_set<xe::ui::VirtualKey> pressed_keys_;
  std::unordered_map<xe::ui::VirtualKey, uint64_t> key_press_times_;
  bool mouse_left_down_ = false;
  bool mouse_right_down_ = false;
  bool mouse_middle_down_ = false;
  int32_t last_mouse_x_ = 0;
  int32_t last_mouse_y_ = 0;
  int32_t mouse_accum_dx_ = 0;
  int32_t mouse_accum_dy_ = 0;
  uint32_t kb_packet_number_ = 1;
};

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_SDL_INPUT_DRIVER_H_
