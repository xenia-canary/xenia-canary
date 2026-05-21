/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/alsa/alsa_audio_driver.h"

#include <pthread.h>
#include <cerrno>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || \
    defined(_M_IX86)
#include <immintrin.h>
#define XE_ALSA_HAS_SIMD 1
#endif

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"

namespace xe {
namespace apu {
namespace alsa {

ALSAAudioDriver::ALSAAudioDriver(xe::threading::Semaphore* semaphore,
                                 uint32_t frequency, uint32_t channels,
                                 bool need_format_conversion)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  assert_not_null(semaphore);
  assert_true(frequency > 0 && frequency <= 192000);
  assert_true(channels == 2 || channels == 6);

  switch (frame_channels_) {
    case 6:
      channel_samples_ = 256;
      break;
    case 2:
      channel_samples_ = 768;
      break;
    default:
      assert_unhandled_case(frame_channels_);
  }
  frame_size_ = sizeof(float) * frame_channels_ * channel_samples_;
  assert_true(frame_size_ <= kFrameSizeMax);
  assert_true(!need_format_conversion_ || frame_channels_ == 6);
}

ALSAAudioDriver::~ALSAAudioDriver() { Shutdown(); }

bool ALSAAudioDriver::Initialize() {
  // Allocate ALSA parameter structures
  int err = snd_pcm_hw_params_malloc(&hw_params_);
  if (err < 0) {
    XELOGE("Failed to allocate hw_params: {}", snd_strerror(err));
    return false;
  }

  err = snd_pcm_sw_params_malloc(&sw_params_);
  if (err < 0) {
    XELOGE("Failed to allocate sw_params: {}", snd_strerror(err));
    return false;
  }

  // Setup the ALSA device
  if (!SetupAlsaDevice()) {
    return false;
  }

  // Allocate conversion buffer if needed
  if (need_format_conversion_ || output_channels_ != frame_channels_) {
    conversion_buffer_ =
        std::make_unique<float[]>(output_channels_ * channel_samples_);
  }

  // Allocate resample buffer for time scaling (up to 4x ratio)
  // Max size: channel_samples * channels * max_ratio
  resample_buffer_ =
      std::make_unique<float[]>(output_channels_ * channel_samples_ * 4);

  // Initialize ring buffer with allocated frames
  for (size_t i = 0; i < kRingBufferSize; i++) {
    ring_buffer_[i] =
        reinterpret_cast<float*>(std::aligned_alloc(64, frame_size_));
    if (!ring_buffer_[i]) {
      XELOGE("Failed to allocate ring buffer slot {}", i);
      return false;
    }
    std::memset(ring_buffer_[i], 0, frame_size_);
  }

  // Start the worker thread
  running_ = true;
  worker_thread_ =
      std::make_unique<std::thread>(&ALSAAudioDriver::WorkerThread, this);

  // Try to set thread priority for lower latency (non-fatal if it fails)
  pthread_t thread = worker_thread_->native_handle();
  struct sched_param param = {};
  param.sched_priority = 1;
  int res = pthread_setschedparam(thread, SCHED_FIFO, &param);
  if (res != 0) {
    if (res == EPERM) {
      XELOGI(
          "Cannot set real-time priority (insufficient permissions). "
          "Audio will work with normal priority but may have higher latency. "
          "Consider setting CAP_SYS_NICE capability for better performance.");
    } else if (res == EINVAL) {
      XELOGW("Invalid scheduling parameters (priority: {})",
             param.sched_priority);
    } else if (res == ESRCH) {
      XELOGW("Thread not found when setting priority");
    } else {
      XELOGW("Failed to set thread priority: {} ({})", res, std::strerror(res));
    }
  } else {
    XELOGI("Audio thread running with real-time priority");
  }

  // Set CPU affinity to first CPU (like XAudio2) for cache locality
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);  // Pin to CPU 0
  res = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
  if (res != 0) {
    XELOGW(
        "Failed to set CPU affinity: {} ({}). Audio will use default CPU "
        "scheduling.",
        res, std::strerror(res));
  } else {
    XELOGI("Audio thread pinned to CPU 0 for cache locality");
  }

  return true;
}

bool ALSAAudioDriver::SetupAlsaDevice() {
  // Open PCM device
  int err = snd_pcm_open(&pcm_handle_, "default", SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    XELOGE("Failed to open PCM device: {}", snd_strerror(err));
    return false;
  }

  // Initialize hardware parameters
  err = snd_pcm_hw_params_any(pcm_handle_, hw_params_);
  if (err < 0) {
    XELOGE("Failed to initialize hw_params: {}", snd_strerror(err));
    return false;
  }

  // Set access type - interleaved float samples
  err = snd_pcm_hw_params_set_access(pcm_handle_, hw_params_,
                                     SND_PCM_ACCESS_RW_INTERLEAVED);
  if (err < 0) {
    XELOGE("Failed to set access type: {}", snd_strerror(err));
    return false;
  }

  // Set sample format - 32-bit float
  err = snd_pcm_hw_params_set_format(pcm_handle_, hw_params_,
                                     SND_PCM_FORMAT_FLOAT_LE);
  if (err < 0) {
    XELOGE("Failed to set format: {}", snd_strerror(err));
    return false;
  }

  // Set sample rate
  unsigned int rate = frame_frequency_;
  err = snd_pcm_hw_params_set_rate_near(pcm_handle_, hw_params_, &rate, 0);
  if (err < 0) {
    XELOGE("Failed to set sample rate: {}", snd_strerror(err));
    return false;
  }
  if (rate != frame_frequency_) {
    XELOGW("Sample rate {} not supported, using {}", frame_frequency_, rate);
  }

  // Try to set channel count - prefer native format, fall back if needed
  output_channels_ = frame_channels_;
  err =
      snd_pcm_hw_params_set_channels(pcm_handle_, hw_params_, output_channels_);
  if (err < 0 && frame_channels_ == 6) {
    // Try stereo fallback for 5.1
    output_channels_ = 2;
    err = snd_pcm_hw_params_set_channels(pcm_handle_, hw_params_, 2);
  }
  if (err < 0) {
    XELOGE("Failed to set channels: {}", snd_strerror(err));
    return false;
  }

  // Set period size for good balance between latency and stability
  // Use power-of-2 sizes that work well with most hardware
  if (output_channels_ == 6) {
    period_size_ = kPeriodSize51;
  } else {
    period_size_ = kPeriodSizeStereo;
  }

  err = snd_pcm_hw_params_set_period_size_near(pcm_handle_, hw_params_,
                                               &period_size_, 0);
  if (err < 0) {
    XELOGE("Failed to set period size: {}", snd_strerror(err));
    return false;
  }

  // Set buffer size to multiple periods for good balance
  // This gives ~85ms latency at 48kHz which is acceptable for gaming
  buffer_size_ = period_size_ * kBufferPeriods;
  err = snd_pcm_hw_params_set_buffer_size_near(pcm_handle_, hw_params_,
                                               &buffer_size_);
  if (err < 0) {
    XELOGE("Failed to set buffer size: {}", snd_strerror(err));
    return false;
  }

  // Apply hardware parameters
  err = snd_pcm_hw_params(pcm_handle_, hw_params_);
  if (err < 0) {
    XELOGE("Failed to apply hw_params: {}", snd_strerror(err));
    return false;
  }

  // Setup software parameters
  err = snd_pcm_sw_params_current(pcm_handle_, sw_params_);
  if (err < 0) {
    XELOGE("Failed to get current sw_params: {}", snd_strerror(err));
    return false;
  }

  // Start threshold - start playback when we have 2 periods of data
  // This ensures smooth playback start without excessive delay
  err = snd_pcm_sw_params_set_start_threshold(pcm_handle_, sw_params_,
                                              period_size_ * 2);
  if (err < 0) {
    XELOGE("Failed to set start threshold: {}", snd_strerror(err));
    return false;
  }

  // Allow transfer when at least period_size frames available
  err = snd_pcm_sw_params_set_avail_min(pcm_handle_, sw_params_, period_size_);
  if (err < 0) {
    XELOGE("Failed to set avail min: {}", snd_strerror(err));
    return false;
  }

  // Apply software parameters
  err = snd_pcm_sw_params(pcm_handle_, sw_params_);
  if (err < 0) {
    XELOGE("Failed to apply sw_params: {}", snd_strerror(err));
    return false;
  }

  XELOGI(
      "ALSA initialized: {} Hz, {} channels (output: {}), period: {}, buffer: "
      "{}",
      rate, frame_channels_, output_channels_, period_size_, buffer_size_);

#if defined(XE_ALSA_HAS_SIMD)
  XELOGI("ALSA: SIMD (SSE) volume mixing enabled");
#else
  XELOGI("ALSA: Using scalar volume mixing (SIMD not available)");
#endif

  return true;
}

void ALSAAudioDriver::SubmitFrame(float* frame) {
  SCOPE_profile_cpu_f("apu");

  // Atomically claim a slot in the ring buffer
  size_t current_write, next_write;
  do {
    current_write = write_index_.load(std::memory_order_acquire);
    next_write = (current_write + 1) % kRingBufferSize;

    // Check if buffer is full
    if (next_write == read_index_.load(std::memory_order_acquire)) {
      XELOGW("ALSA ring buffer full, dropping audio frame");
      return;
    }
  } while (!write_index_.compare_exchange_weak(current_write, next_write,
                                               std::memory_order_release,
                                               std::memory_order_acquire));

  // Copy frame to the claimed slot
  std::memcpy(ring_buffer_[current_write], frame, frame_size_);
}

void ALSAAudioDriver::WorkerThread() {
  xe::threading::set_name("ALSA Audio");

  // Prepare PCM for playback
  int err = snd_pcm_prepare(pcm_handle_);
  if (err < 0) {
    XELOGE("Failed to prepare PCM: {}", snd_strerror(err));
    return;
  }

  // Prefill buffer with silence to avoid initial underrun
  // Fill 2 periods to match the start threshold
  size_t silence_frames = period_size_ * 2;
  auto silence = std::make_unique<float[]>(silence_frames * output_channels_);
  snd_pcm_writei(pcm_handle_, silence.get(), silence_frames);

  while (running_) {
    if (paused_) {
      snd_pcm_drop(pcm_handle_);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    err = snd_pcm_wait(pcm_handle_, 100);
    if (err < 0) {
      XELOGW("PCM wait returned error: {} ({})", err, snd_strerror(err));
      if (RecoverFromUnderrun(err)) {
        continue;
      }
      XELOGE("PCM wait failed: {}", snd_strerror(err));
      break;
    }

    // Check if we have data to write before calling avail_update
    size_t current_read = read_index_.load(std::memory_order_relaxed);
    size_t current_write = write_index_.load(std::memory_order_acquire);

    if (current_read == current_write) {
      // No data available, sleep and try again
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    snd_pcm_sframes_t frames_available = snd_pcm_avail_update(pcm_handle_);
    if (frames_available < 0) {
      XELOGW("avail_update returned error: {} ({})", frames_available,
             snd_strerror(frames_available));
      if (RecoverFromUnderrun(frames_available)) {
        continue;
      }
      XELOGE("Failed to get available frames: {}",
             snd_strerror(frames_available));
      break;
    }

    // Write frames from ring buffer
    current_read = read_index_.load(std::memory_order_relaxed);
    current_write = write_index_.load(std::memory_order_acquire);

    while (frames_available >= (snd_pcm_sframes_t)channel_samples_) {
      // Check if we need to refresh write_index_ (only when caught up)
      if (current_read == current_write) {
        current_write = write_index_.load(std::memory_order_acquire);
        if (current_read == current_write) {
          break;  // Still no new frames available
        }
      }

      float* frame = ring_buffer_[current_read];
      float* output = frame;

      // Convert format/channels if needed
      // ALSA expects interleaved data, so we MUST convert if data is sequential
      if (need_format_conversion_ || output_channels_ != frame_channels_) {
        ConvertChannels(frame, conversion_buffer_.get(), channel_samples_);
        output = conversion_buffer_.get();
      } else {
        // Even without format conversion, we need to handle the data layout
        // Xbox 360 audio can be in sequential format, but ALSA expects
        // interleaved For now, assume if no conversion needed, data is already
        // in correct format (This matches XAudio2 behavior where memcpy is
        // used)
        output = frame;
      }

      // Apply volume using SIMD-optimized function
      // Cache volume to ensure consistency across the entire frame
      uint32_t mv = cvars::volume > 100 ? 100 : cvars::volume;
      float vol = volume_.load(std::memory_order_relaxed) * (mv / 100.0f);
      ApplyVolume(output, output_channels_ * channel_samples_, vol);

      // Apply time scaling / resampling
      float frequency_ratio = static_cast<float>(Clock::guest_time_scalar());
      size_t frames_to_write = channel_samples_;
      float* final_output = output;

      if (frequency_ratio != 1.0f) {
        // Resample based on time scalar
        // Max output frames = channel_samples_ * 4 (supports up to 4x slowdown)
        size_t resampled_frames = ResampleFrame(
            output, resample_buffer_.get(), channel_samples_,
            channel_samples_ * 4, frequency_ratio, output_channels_);
        frames_to_write = resampled_frames;
        final_output = resample_buffer_.get();
      }

      snd_pcm_sframes_t avail = snd_pcm_avail(pcm_handle_);
      if (avail < 0) {
        XELOGW("snd_pcm_avail returned error: {} ({})", avail,
               snd_strerror(avail));
        if (!RecoverFromUnderrun(avail)) {
          XELOGE("Failed to check available space: {}", snd_strerror(avail));
          running_ = false;
          break;
        }
        continue;
      }

      // Only write if we have enough space
      snd_pcm_sframes_t written = 0;
      if (avail >= (snd_pcm_sframes_t)frames_to_write) {
        written = snd_pcm_writei(pcm_handle_, final_output, frames_to_write);
        if (written < 0) {
          if (!RecoverFromUnderrun(written)) {
            XELOGE("Failed to write audio: {}", snd_strerror(written));
            running_ = false;
            break;
          }
          continue;  // Skip frame advancement if we had to recover
        } else if (written != (snd_pcm_sframes_t)frames_to_write) {
          XELOGW("Partial write: {} of {} frames", written, frames_to_write);
        }

        // Move to next frame in ring buffer
        // Use release semantics so writer thread sees this update
        current_read = (current_read + 1) % kRingBufferSize;
        read_index_.store(current_read, std::memory_order_release);
        frames_available -= written;

        // Signal that a frame was consumed
        semaphore_->Release(1, nullptr);
      } else {
        // Not enough space, wait for next iteration
        frames_available = 0;  // Exit the inner loop to wait again
        continue;
      }
    }
  }
}

bool ALSAAudioDriver::RecoverFromUnderrun(int err) {
  if (err == -EPIPE) {
    // Underrun occurred
    XELOGW("ALSA underrun detected, recovering...");
    err = snd_pcm_prepare(pcm_handle_);
    if (err < 0) {
      XELOGE("Failed to recover from underrun: {}", snd_strerror(err));
      return false;
    }
    return true;
  } else if (err == -ESTRPIPE) {
    // Stream suspended
    XELOGW("ALSA stream suspended");
    while ((err = snd_pcm_resume(pcm_handle_)) == -EAGAIN) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (err < 0) {
      err = snd_pcm_prepare(pcm_handle_);
      if (err < 0) {
        XELOGE("Failed to resume stream: {}", snd_strerror(err));
        return false;
      }
    }
    return true;
  }
  return false;
}

void ALSAAudioDriver::ConvertChannels(const float* input, float* output,
                                      size_t channel_samples) {
  if (need_format_conversion_) {
    // Format conversion handles both endian swap and channel conversion
    // Input is sequential big-endian, output is interleaved little-endian
    if (output_channels_ == 2) {
      conversion::sequential_6_BE_to_interleaved_2_LE(output, input,
                                                      channel_samples);
    } else if (output_channels_ == 6) {
      conversion::sequential_6_BE_to_interleaved_6_LE(output, input,
                                                      channel_samples);
    }
  } else if (output_channels_ != frame_channels_) {
    // Channel count mismatch without format conversion - shouldn't happen
    // but handle it by copying what we can
    size_t samples_to_copy =
        std::min(output_channels_, frame_channels_) * channel_samples;
    std::memcpy(output, input, samples_to_copy * sizeof(float));
  } else {
    // Direct copy - input format matches output format exactly
    // For stereo: 768 samples * 2 channels * 4 bytes = 6144 bytes
    // For 5.1: 256 samples * 6 channels * 4 bytes = 6144 bytes
    std::memcpy(output, input,
                channel_samples * frame_channels_ * sizeof(float));
  }
}

size_t ALSAAudioDriver::ResampleFrame(const float* input, float* output,
                                      size_t input_frame_count,
                                      size_t output_capacity_frames,
                                      float frequency_ratio,
                                      uint32_t channels) {
  if (frequency_ratio == 1.0f) {
    size_t frames_to_copy = std::min(input_frame_count, output_capacity_frames);
    std::memcpy(output, input, frames_to_copy * channels * sizeof(float));
    return frames_to_copy;
  }

  // Linear interpolation resampling for interleaved multi-channel audio
  // frequency_ratio > 1.0 = faster playback (fewer output frames)
  // frequency_ratio < 1.0 = slower playback (more output frames)
  const double step = static_cast<double>(frequency_ratio);
  size_t output_frame_count = 0;
  double position = resample_frac_position_;

  while (output_frame_count < output_capacity_frames) {
    size_t frame_index = static_cast<size_t>(position);
    if (frame_index + 1 >= input_frame_count) {
      break;
    }

    double frac = position - frame_index;
    double inv_frac = 1.0 - frac;

    // Interpolate each channel
    for (uint32_t ch = 0; ch < channels; ch++) {
      size_t in_idx0 = frame_index * channels + ch;
      size_t in_idx1 = (frame_index + 1) * channels + ch;
      size_t out_idx = output_frame_count * channels + ch;

      output[out_idx] =
          static_cast<float>(input[in_idx0] * inv_frac + input[in_idx1] * frac);
    }

    position += step;
    output_frame_count++;
  }

  // Store fractional part for next frame
  resample_frac_position_ = position - static_cast<size_t>(position);

  return output_frame_count;
}

void ALSAAudioDriver::ApplyVolume(float* buffer, size_t sample_count,
                                  float volume) {
  if (volume == 1.0f) {
    return;
  }

#if defined(XE_ALSA_HAS_SIMD)
  // Use SSE for SIMD volume application (4 floats at a time)
  const __m128 vol_vec = _mm_set1_ps(volume);
  size_t simd_count = sample_count & ~3;  // Round down to multiple of 4

  for (size_t i = 0; i < simd_count; i += 4) {
    __m128 data = _mm_loadu_ps(&buffer[i]);
    data = _mm_mul_ps(data, vol_vec);
    _mm_storeu_ps(&buffer[i], data);
  }

  // Handle remaining samples (0-3) with scalar operations
  for (size_t i = simd_count; i < sample_count; i++) {
    buffer[i] *= volume;
  }
#else
  // Fallback scalar implementation for non-x86 platforms
  for (size_t i = 0; i < sample_count; i++) {
    buffer[i] *= volume;
  }
#endif
}

void ALSAAudioDriver::Pause() {
  paused_ = true;
  if (pcm_handle_) {
    snd_pcm_pause(pcm_handle_, 1);
  }
}

void ALSAAudioDriver::Resume() {
  paused_ = false;
  if (pcm_handle_) {
    snd_pcm_pause(pcm_handle_, 0);
  }
}

void ALSAAudioDriver::Shutdown() {
  if (running_) {
    running_ = false;
    if (worker_thread_) {
      worker_thread_->join();
      worker_thread_.reset();
    }
  }

  if (pcm_handle_) {
    snd_pcm_drop(pcm_handle_);
    snd_pcm_close(pcm_handle_);
    pcm_handle_ = nullptr;
  }

  if (hw_params_) {
    snd_pcm_hw_params_free(hw_params_);
    hw_params_ = nullptr;
  }

  if (sw_params_) {
    snd_pcm_sw_params_free(sw_params_);
    sw_params_ = nullptr;
  }

  // Free ring buffer frames
  for (size_t i = 0; i < kRingBufferSize; i++) {
    if (ring_buffer_[i]) {
      std::free(ring_buffer_[i]);
      ring_buffer_[i] = nullptr;
    }
  }
}

}  // namespace alsa
}  // namespace apu
}  // namespace xe
