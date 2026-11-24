/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/nop/nop_audio_system.h"

#include <limits>

#include "third_party/catch/include/catch.hpp"

namespace xe::kernel::test {
namespace {

class TestAudio : public apu::nop::NopAudioSystem {
 public:
  using NopAudioSystem::NopAudioSystem;
  ~TestAudio() override { Shutdown(); }
};

TEST_CASE("Audio registration reports capacity and reuses a released slot",
          "[kernel][audio]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  cpu::Processor processor(&memory, nullptr);
  TestAudio audio(&processor);
  // No worker is started. The callback and its argument are inert.
  constexpr size_t no_slot = std::numeric_limits<size_t>::max();
  for (size_t expected = 0; expected < 8; ++expected) {
    size_t index = no_slot;
    REQUIRE(audio.RegisterClient(1, 7, &index) == X_STATUS_SUCCESS);
    REQUIRE(index == expected);
  }
  size_t index = no_slot;
  CHECK(audio.RegisterClient(1, 7, &index) == X_STATUS_NO_MEMORY);
  CHECK(index == no_slot);
  CHECK(audio.RegisterClient(1, 7, nullptr) == X_STATUS_NO_MEMORY);

  audio.UnregisterClient(3);
  REQUIRE(audio.RegisterClient(1, 9, &index) == X_STATUS_SUCCESS);
  CHECK(index == 3);
}

}  // namespace
}  // namespace xe::kernel::test
