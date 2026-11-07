/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_audio_driver.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/helper/sdl/sdl_helper.h"

namespace xe {
namespace apu {
namespace sdl {

SDLAudioDriver::SDLAudioDriver(xe::threading::Semaphore* semaphore,
                               uint32_t frequency, uint32_t channels,
                               bool need_format_conversion)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
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

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
};

bool SDLAudioDriver::Initialize() {
  if (!xe::helper::sdl::SDLHelper::Prepare()) {
    return false;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    XELOGE("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  sdl_stream_channels_ = frame_channels_;
  if (need_format_conversion_) {
    // 5.1 input is converted here to stereo when that's what the device has.
    // Any other layout (4 channels, 7.1) gets 5.1, which SDL converts.
    SDL_AudioSpec device_spec = {};
    sdl_stream_channels_ =
        (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                  &device_spec, nullptr) &&
         device_spec.channels == 2)
            ? 2
            : 6;
  }
  assert_true(sdl_stream_channels_ * channel_samples_ <= kFrameSamplesMax);

  // Ask for a device buffer of one frame, as SDL2's `samples` did. This is
  // only a request, and SDL may pick another size.
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
              std::to_string(channel_samples_).c_str());

  SDL_AudioSpec spec = {};
  spec.format = SDL_AUDIO_F32;
  spec.channels = static_cast<int>(sdl_stream_channels_);
  spec.freq = static_cast<int>(frame_frequency_);
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, SDLCallback, this);
  if (!sdl_stream_) {
    XELOGE("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }
  SDL_SetAudioStreamGain(sdl_stream_, volume_);

  SDL_ResumeAudioStreamDevice(sdl_stream_);

  return true;
}

void SDLAudioDriver::SubmitFrame(float* frame) {
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_channels_ * channel_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, frame, frame_size_);

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
  }
}

void SDLAudioDriver::Pause() {
  if (sdl_stream_) {
    SDL_PauseAudioStreamDevice(sdl_stream_);
  }
}

void SDLAudioDriver::Resume() {
  if (sdl_stream_) {
    SDL_ResumeAudioStreamDevice(sdl_stream_);
  }
}

void SDLAudioDriver::SetVolume(float volume) {
  volume_ = volume;
  if (sdl_stream_) {
    SDL_SetAudioStreamGain(sdl_stream_, volume);
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  };
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  };
}

void SDLCALL SDLAudioDriver::SDLCallback(void* userdata,
                                         SDL_AudioStream* stream,
                                         int additional_amount,
                                         int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    XELOGE("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  float* const output = driver->output_frame_.data();
  const int output_size = static_cast<int>(
      sizeof(float) * driver->channel_samples_ * driver->sdl_stream_channels_);

  // SDL asks for a number of bytes, not whole frames: give it frames until it
  // has at least that much.
  while (additional_amount > 0) {
    float* buffer = nullptr;
    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      if (!driver->frames_queued_.empty()) {
        buffer = driver->frames_queued_.front();
        driver->frames_queued_.pop();
      }
    }

    if (!buffer) {
      // Nothing to play: give only the silence SDL needs, so that the next
      // frame isn't delayed.
      std::memset(output, 0, output_size);
      while (additional_amount > 0) {
        const int size = std::min(additional_amount, output_size);
        SDL_PutAudioStreamData(stream, output, size);
        additional_amount -= size;
      }
      break;
    }

    if (cvars::mute) {
      std::memset(output, 0, output_size);
    } else if (driver->need_format_conversion_) {
      switch (driver->sdl_stream_channels_) {
        case 2:
          conversion::sequential_6_BE_to_interleaved_2_LE(
              output, buffer, driver->channel_samples_);
          break;
        case 6:
          conversion::sequential_6_BE_to_interleaved_6_LE(
              output, buffer, driver->channel_samples_);
          break;
        default:
          assert_unhandled_case(driver->sdl_stream_channels_);
          break;
      }
    } else {
      assert_true(driver->sdl_stream_channels_ == driver->frame_channels_);
      std::memcpy(output, buffer, output_size);
    }
    SDL_PutAudioStreamData(stream, output, output_size);
    additional_amount -= output_size;

    {
      std::unique_lock<std::mutex> guard(driver->frames_mutex_);
      driver->frames_unused_.push(buffer);
    }

    auto ret = driver->semaphore_->Release(1, nullptr);
    assert_true(ret);
  }
}

}  // namespace sdl
}  // namespace apu
}  // namespace xe
