/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_INPUT_FILTER_H_
#define XENIA_KERNEL_XAM_UI_INPUT_FILTER_H_

#include <array>
#include <cstdint>

#include "xenia/hid/input.h"
#include "xenia/xbox.h"

namespace xe {
namespace hid {
class InputSystem;
}  // namespace hid
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

// Hides the input that closed XAM UI from the game, so it doesn't act on the
// same press, e.g. by reopening the dialog.
class UIInputFilter {
 public:
  // Hides the buttons held now until they're released, and drops the
  // keystrokes queued while the UI was open.
  void OnUIClosed(hid::InputSystem* input_system);

  // These must be called with the input system locked.
  void FilterState(uint32_t user_index, hid::X_INPUT_STATE* state);
  bool ShouldDropKeystroke(hid::InputSystem* input_system,
                           const hid::X_INPUT_KEYSTROKE& keystroke);

 private:
  static constexpr int kMaxQueuedKeystrokes = 64;

  std::array<uint16_t, XUserMaxUserCount> held_buttons_ = {};
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
