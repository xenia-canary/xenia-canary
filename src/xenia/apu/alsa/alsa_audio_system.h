/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_ALSA_ALSA_AUDIO_SYSTEM_H_
#define XENIA_APU_ALSA_ALSA_AUDIO_SYSTEM_H_

#include "xenia/apu/audio_system.h"

namespace xe {
namespace apu {
namespace alsa {

class ALSAAudioSystem : public AudioSystem {
 public:
  explicit ALSAAudioSystem(cpu::Processor* processor);
  ~ALSAAudioSystem() override;

  static bool IsAvailable();

  static std::unique_ptr<AudioSystem> Create(cpu::Processor* processor);

  std::string name() const override { return "ALSA"; }

  X_STATUS CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                            uint32_t frequency, uint32_t channels,
                            bool need_format_conversion) override;

 protected:
  void Initialize() override;
};

}  // namespace alsa
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_ALSA_ALSA_AUDIO_SYSTEM_H_
