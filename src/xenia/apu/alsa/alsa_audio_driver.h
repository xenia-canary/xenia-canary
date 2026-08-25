/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_ALSA_ALSA_AUDIO_DRIVER_H_
#define XENIA_APU_ALSA_ALSA_AUDIO_DRIVER_H_

#include <alsa/asoundlib.h>
#include <atomic>
#include <memory>
#include <thread>

#include "xenia/apu/audio_driver.h"
#include "xenia/base/threading.h"

namespace xe {
namespace apu {
namespace alsa {

class ALSAAudioDriver : public AudioDriver {
 public:
  ALSAAudioDriver(xe::threading::Semaphore* semaphore,
                  uint32_t frequency = kFrameFrequencyDefault,
                  uint32_t channels = kFrameChannelsDefault,
                  bool need_format_conversion = true);
  ~ALSAAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override { volume_.store(volume); }
  void Shutdown() override;

 private:
  void WorkerThread();
  bool SetupAlsaDevice();
  bool RecoverFromUnderrun(int err);
  void ConvertChannels(const float* input, float* output,
                       size_t channel_samples);
  size_t ResampleFrame(const float* input, float* output,
                       size_t input_frame_count, size_t output_capacity_frames,
                       float frequency_ratio, uint32_t channels);
  void ApplyVolume(float* buffer, size_t sample_count, float volume);
  void ApplyChannelSwizzle(float* buffer, size_t channel_samples);

  xe::threading::Semaphore* semaphore_ = nullptr;

  // ALSA handles
  snd_pcm_t* pcm_handle_ = nullptr;
  snd_pcm_hw_params_t* hw_params_ = nullptr;
  snd_pcm_sw_params_t* sw_params_ = nullptr;

  // Device configuration
  uint32_t frame_frequency_;
  uint32_t frame_channels_;
  uint32_t channel_samples_;
  uint32_t frame_size_;
  bool need_format_conversion_;

  // Output configuration (may differ from input)
  uint32_t output_channels_ = 0;
  snd_pcm_uframes_t period_size_ = 0;
  snd_pcm_uframes_t buffer_size_ = 0;

  // True if the opened device's channel map doesn't already match Xenia's
  // Xbox 360/XAudio2 channel order (FL, FR, FC, LFE, BL, BR), and channels
  // 3/5 need to be swapped with 5/6 (indices 2/3 with 4/5) before writing.
  bool swizzle_channels_ = false;

  // Threading
  std::unique_ptr<std::thread> worker_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> paused_{false};
  std::atomic<float> volume_{1.0f};

  // Ring buffer for frames (larger size to reduce underruns)
  static constexpr size_t kRingBufferSize = 32;
  float* ring_buffer_[kRingBufferSize] = {};
  std::atomic<size_t> read_index_{0};
  std::atomic<size_t> write_index_{0};

  // ALSA period and buffer configuration
  static constexpr snd_pcm_uframes_t kPeriodSize51 = 512;
  static constexpr snd_pcm_uframes_t kPeriodSizeStereo = 1024;
  static constexpr size_t kBufferPeriods = 4;

  // Conversion buffer
  std::unique_ptr<float[]> conversion_buffer_;

  // Time scaling / resampling state
  std::unique_ptr<float[]> resample_buffer_;
  double resample_frac_position_ = 0.0;
};

}  // namespace alsa
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_ALSA_ALSA_AUDIO_DRIVER_H_
