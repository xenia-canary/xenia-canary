/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_PACING_AUDIO_DRIVER_H_
#define XENIA_APU_PACING_AUDIO_DRIVER_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {

// Discards audio and releases frame slots at the playback rate for direct media
// playback without a host output driver.
class PacingAudioDriver final : public AudioDriver {
 public:
  explicit PacingAudioDriver(xe::threading::Semaphore* semaphore,
                             uint32_t frequency = kFrameFrequencyDefault,
                             uint32_t channel_samples = kChannelSamplesDefault);
  ~PacingAudioDriver() override;

  // Starts the pacing thread if the frame duration is valid.
  bool Initialize() override;
  // Stops the pacing thread and drops pending frames without releasing them.
  void Shutdown() override;

  // Discards the samples and schedules a frame slot release.
  void SubmitFrame(float* samples) override;
  // Holds releases until Resume shifts pending deadlines by the pause duration.
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override {}

 private:
  using Clock = std::chrono::steady_clock;

  void ThreadMain();

  xe::threading::Semaphore* semaphore_ = nullptr;
  const uint32_t frequency_;
  const uint32_t channel_samples_;
  const Clock::duration frame_duration_;

  std::mutex mutex_;
  bool running_ = false;
  bool paused_ = false;
  Clock::time_point paused_at_;
  Clock::time_point next_deadline_;
  // Pending frame deadlines in submission order.
  std::deque<Clock::time_point> deadlines_;

  std::unique_ptr<xe::threading::Event> wake_event_;
  std::unique_ptr<xe::threading::Thread> thread_;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_PACING_AUDIO_DRIVER_H_
