/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_media_player.h"

#include <utility>

#include "xenia/base/logging.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

AudioMediaPlayer::AudioMediaPlayer(apu::AudioSystem* audio_system,
                                   kernel::KernelState* kernel_state)
    : audio_system_(audio_system), kernel_state_(kernel_state) {}

AudioMediaPlayer::~AudioMediaPlayer() { Stop(true, true); }

void AudioMediaPlayer::WorkerThreadMain() {}

void AudioMediaPlayer::Setup() {
  XELOGW("AudioMediaPlayer disabled on macOS (FFmpeg libavformat unavailable)");
  state_ = XmpApp::State::kIdle;
}

X_STATUS AudioMediaPlayer::Play(uint32_t playlist_handle, uint32_t song_handle,
                                bool force) {
  return X_STATUS_NOT_SUPPORTED;
}

void AudioMediaPlayer::Play() {}

void AudioMediaPlayer::Pause() { state_ = XmpApp::State::kPaused; }

void AudioMediaPlayer::Stop(bool change_state, bool force) {
  if (change_state) {
    state_ = XmpApp::State::kIdle;
  }
  active_playlist_ = nullptr;
  active_song_ = nullptr;
}

void AudioMediaPlayer::Continue() {}

X_STATUS AudioMediaPlayer::Next() { return X_STATUS_NOT_SUPPORTED; }

X_STATUS AudioMediaPlayer::Previous() { return X_STATUS_NOT_SUPPORTED; }

bool AudioMediaPlayer::LoadSongToMemory(std::vector<uint8_t>* buffer) {
  return false;
}

void AudioMediaPlayer::AddPlaylist(uint32_t handle,
                                   std::unique_ptr<XmpApp::Playlist> playlist) {
  playlists_[handle] = std::move(playlist);
}

void AudioMediaPlayer::RemovePlaylist(uint32_t handle) {
  playlists_.erase(handle);
}

X_STATUS AudioMediaPlayer::SetVolume(float volume) {
  volume_.store(volume);
  return X_STATUS_SUCCESS;
}

bool AudioMediaPlayer::IsLastSongInPlaylist() const { return true; }

void AudioMediaPlayer::SetCaptureCallback(uint32_t callback, uint32_t context,
                                          bool title_render) {
  callback_ = callback;
  callback_context_ = context;
  is_title_rendering_enabled_ = title_render;
}

void AudioMediaPlayer::OnStateChanged() {}

void AudioMediaPlayer::ProcessAudioBuffer(std::vector<float>* buffer) {}

bool AudioMediaPlayer::SetupDriver(uint32_t sample_rate, uint32_t channels) {
  return false;
}

void AudioMediaPlayer::DeleteDriver() {}

}  // namespace apu
}  // namespace xe
