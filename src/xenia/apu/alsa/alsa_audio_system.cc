/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/alsa/alsa_audio_system.h"

#include <alsa/asoundlib.h>

#include "xenia/apu/alsa/alsa_audio_driver.h"
#include "xenia/apu/apu_flags.h"
#include "xenia/base/logging.h"

namespace xe {
namespace apu {
namespace alsa {

ALSAAudioSystem::ALSAAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

ALSAAudioSystem::~ALSAAudioSystem() {}

bool ALSAAudioSystem::IsAvailable() {
  // Check if ALSA is available by trying to open a dummy PCM device
  snd_pcm_t* pcm = nullptr;
  int err =
      snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err >= 0 && pcm) {
    snd_pcm_close(pcm);
    return true;
  }
  return false;
}

void ALSAAudioSystem::Initialize() {}

std::unique_ptr<AudioSystem> ALSAAudioSystem::Create(
    cpu::Processor* processor) {
  if (!IsAvailable()) {
    XELOGE("ALSA is not available on this system");
    return nullptr;
  }
  return std::make_unique<ALSAAudioSystem>(processor);
}

X_STATUS ALSAAudioSystem::CreateDriver(size_t index,
                                       xe::threading::Semaphore* semaphore,
                                       AudioDriver** out_driver) {
  assert_not_null(out_driver);

  XELOGI("ALSAAudioSystem::CreateDriver for client index {}", index);

  // Create a new driver for each client
  // ALSA's dmix plugin should handle mixing multiple streams
  auto driver = new ALSAAudioDriver(semaphore);
  if (!driver->Initialize()) {
    XELOGE("Failed to initialize ALSA driver for client index {}", index);
    delete driver;
    *out_driver = nullptr;
    return X_STATUS_UNSUCCESSFUL;
  }

  *out_driver = driver;
  XELOGI("Successfully created ALSA driver for client index {}", index);
  return X_STATUS_SUCCESS;
}

AudioDriver* ALSAAudioSystem::CreateDriver(xe::threading::Semaphore* semaphore,
                                           uint32_t frequency,
                                           uint32_t channels,
                                           bool need_format_conversion) {
  return new ALSAAudioDriver(semaphore, frequency, channels,
                             need_format_conversion);
}

}  // namespace alsa
}  // namespace apu
}  // namespace xe
