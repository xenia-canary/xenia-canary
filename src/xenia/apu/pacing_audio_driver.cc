/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/pacing_audio_driver.h"

#include <algorithm>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"

namespace xe {
namespace apu {

namespace {

std::chrono::steady_clock::duration FrameDuration(uint32_t frequency,
                                                  uint32_t channel_samples) {
  if (!frequency || !channel_samples) {
    return std::chrono::steady_clock::duration::zero();
  }
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::nanoseconds(uint64_t(channel_samples) * 1000000000u /
                               frequency));
}

}  // namespace

PacingAudioDriver::PacingAudioDriver(xe::threading::Semaphore* semaphore,
                                     uint32_t frequency,
                                     uint32_t channel_samples)
    : semaphore_(semaphore),
      frequency_(frequency),
      channel_samples_(channel_samples),
      frame_duration_(FrameDuration(frequency, channel_samples)),
      wake_event_(xe::threading::Event::CreateAutoResetEvent(false)) {
  assert_not_null(semaphore_);
}

PacingAudioDriver::~PacingAudioDriver() { Shutdown(); }

bool PacingAudioDriver::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_) {
    return true;
  }
  if (!wake_event_ || frame_duration_ <= Clock::duration::zero()) {
    return false;
  }
  running_ = true;
  // The thread takes mutex_ first thing, so it waits for this to return.
  thread_ = xe::threading::Thread::Create({}, [this]() { ThreadMain(); });
  if (!thread_) {
    running_ = false;
    return false;
  }
  thread_->set_name("APU Pacing Driver");
  XELOGI(
      "APU: pacing silent media at {} Hz, {} samples per "
      "frame",
      frequency_, channel_samples_);
  return true;
}

void PacingAudioDriver::Shutdown() {
  std::unique_ptr<xe::threading::Thread> thread;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    deadlines_.clear();
    thread = std::move(thread_);
  }
  if (thread) {
    wake_event_->Set();
    xe::threading::Wait(thread.get(), false);
  }
}

void PacingAudioDriver::SubmitFrame(float* samples) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_) {
    return;
  }
  // Start from now if the previous frame has already finished.
  next_deadline_ =
      std::max(paused_ ? paused_at_ : Clock::now(), next_deadline_) +
      frame_duration_;
  deadlines_.push_back(next_deadline_);
  if (deadlines_.size() == 1) {
    wake_event_->Set();
  }
}

void PacingAudioDriver::Pause() {
  // The worker checks paused_ before releasing pending frames.
  std::lock_guard<std::mutex> lock(mutex_);
  if (paused_) {
    return;
  }
  paused_ = true;
  paused_at_ = Clock::now();
}

void PacingAudioDriver::Resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!paused_) {
    return;
  }
  const Clock::duration paused_for = Clock::now() - paused_at_;
  for (Clock::time_point& deadline : deadlines_) {
    deadline += paused_for;
  }
  next_deadline_ += paused_for;
  paused_ = false;
  if (running_) {
    wake_event_->Set();
  }
}

void PacingAudioDriver::ThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_) {
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max();
    if (!paused_) {
      const Clock::time_point now = Clock::now();
      while (!deadlines_.empty() && deadlines_.front() <= now) {
        deadlines_.pop_front();
        // Hold the mutex so Pause and Shutdown wait for pending releases.
        auto released = semaphore_->Release(1, nullptr);
        assert_true(released);
      }
      if (!deadlines_.empty()) {
        timeout = std::chrono::ceil<std::chrono::milliseconds>(
            deadlines_.front() - now);
      }
    }
    lock.unlock();
    xe::threading::Wait(wake_event_.get(), false, timeout);
    lock.lock();
  }
}

}  // namespace apu
}  // namespace xe
