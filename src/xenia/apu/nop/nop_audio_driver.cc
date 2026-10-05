/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/nop/nop_audio_driver.h"

namespace xe {
namespace apu {
namespace nop {

NopAudioDriver::NopAudioDriver(xe::threading::Semaphore* semaphore)
    : semaphore_(semaphore) {}

NopAudioDriver::~NopAudioDriver() = default;

bool NopAudioDriver::Initialize() { return true; }

void NopAudioDriver::SubmitFrame(float* frame) {
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
}

}  // namespace nop
}  // namespace apu
}  // namespace xe
