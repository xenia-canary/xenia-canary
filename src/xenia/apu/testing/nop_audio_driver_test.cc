/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/nop_audio_driver.h"

#include <chrono>

#include "third_party/catch/include/catch.hpp"

TEST_CASE("NOP audio returns one slot for every discarded frame", "[apu]") {
  using namespace std::chrono_literals;
  using xe::threading::WaitResult;
  auto slots = xe::threading::Semaphore::Create(0, 2);
  REQUIRE(slots);
  xe::apu::nop::NopAudioDriver driver(slots.get());
  REQUIRE(driver.Initialize());
  float frame[xe::apu::AudioDriver::kFrameSamplesMax] = {};

  SECTION("normal playback") {}
  SECTION("paused and muted") {
    driver.Pause();
    driver.SetVolume(0.0f);
  }
  SECTION("resumed playback") {
    driver.Pause();
    driver.Resume();
  }

  driver.SubmitFrame(frame);
  driver.SubmitFrame(frame);
  REQUIRE(xe::threading::Wait(slots.get(), false, 0ms) == WaitResult::kSuccess);
  REQUIRE(xe::threading::Wait(slots.get(), false, 0ms) == WaitResult::kSuccess);
  REQUIRE(xe::threading::Wait(slots.get(), false, 0ms) == WaitResult::kTimeout);
  driver.Shutdown();
}
