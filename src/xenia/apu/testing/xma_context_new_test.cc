/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/xma_context_new.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::apu::test {
namespace {
using Packet = std::array<uint8_t, XmaContext::kBytesPerPacket>;

// Synthetic mono XMA silence: one 512-sample frame, no coefficients.
// The long variant uses legal extended-header fill bits to cross a packet
// boundary. No encoded game audio or private decoder fields are needed.
std::vector<bool> SilenceFrame(bool long_frame) {
  std::vector<bool> bits;
  auto write = [&bits](uint32_t value, uint32_t count) {
    for (uint32_t i = count; i; --i) {
      bits.push_back((value >> (i - 1)) & 1);
    }
  };
  write(long_frame ? 16360 : 31, 15);  // Frame length, including trailer.
  write(1, 1);                         // Fixed channel layout.
  write(0, 1);                         // Full-length subframe.
  write(0, 8);                         // DRC gain.
  write(0, 1);                         // No sample trimming.
  write(long_frame ? 1 : 0, 1);        // Extended header.
  if (long_frame) {
    write(0, 2);       // Variable-width fill length follows.
    write(14, 4);      // Width of the fill length.
    write(16308, 14);  // 16309 fill bits (encoded as count - 1).
    bits.insert(bits.end(), 16309, false);
  }
  write(0, 1);  // Reserved.
  write(0, 1);  // No transmitted coefficients.
  write(0, 1);  // Frame padding.
  write(0, 1);  // No subsequent frame in this packet.
  return bits;
}

Packet MakePacket(const std::vector<bool>& frame, uint32_t first_bit) {
  Packet packet{};
  uint32_t offset = first_bit - 32;
  packet[0] = uint8_t(4 | (offset >> 13));  // One frame starts here.
  packet[1] = uint8_t(offset >> 5);
  packet[2] = uint8_t((offset << 3) | 1);  // XMA2 metadata.
  for (size_t i = 0; i < frame.size() && first_bit + i < packet.size() * 8;
       ++i) {
    if (frame[i]) {
      packet[(first_bit + i) / 8] |= uint8_t(1 << (7 - (first_bit + i) % 8));
    }
  }
  return packet;
}

Packet ContinuationPacket(const std::vector<bool>& frame, uint32_t head_bits) {
  const uint32_t tail_bits = uint32_t(frame.size()) - head_bits;
  Packet packet = MakePacket(SilenceFrame(false), 32 + tail_bits);
  for (uint32_t i = 0; i < tail_bits; ++i) {
    if (frame[head_bits + i]) {
      const uint32_t bit = 32 + i;
      packet[bit / 8] |= uint8_t(1 << (7 - bit % 8));
    }
  }
  return packet;
}

struct Result {
  uint32_t error;
  std::vector<uint8_t> pcm;
};

class Fixture {
 public:
  Fixture() {
    if (!memory.Initialize()) {
      throw std::runtime_error("Unable to initialize test memory");
    }
    context_address = memory.SystemHeapAlloc(64, 64);
    input_address = memory.SystemHeapAlloc(2048, 2048, kSystemHeapPhysical);
    second_address = memory.SystemHeapAlloc(2048, 2048, kSystemHeapPhysical);
    output_address = memory.SystemHeapAlloc(4096, 4096, kSystemHeapPhysical);
    if (!context_address || !input_address || !second_address ||
        !output_address) {
      throw std::runtime_error("Unable to allocate test buffers");
    }
  }

  void Setup(XmaContextNew& context) {
    if (context.Setup(0, &memory, context_address) != 0) {
      throw std::runtime_error("Unable to initialize test decoder");
    }
    context.set_is_allocated(true);
    context.Clear();
  }

  Result Decode(XmaContextNew& context, const Packet& packet,
                uint32_t first_bit, uint32_t rate_id,
                const Packet* next_packet = nullptr,
                bool null_next_address = false) {
    std::memcpy(memory.TranslateVirtual(input_address), packet.data(),
                packet.size());
    if (next_packet) {
      std::memcpy(memory.TranslateVirtual(second_address), next_packet->data(),
                  next_packet->size());
    }
    auto output = memory.TranslateVirtual(output_address);
    std::memset(output, 0xA5, 4096);
    std::array<uint8_t, 64> zero{};
    XMA_CONTEXT_DATA data(zero.data());
    data.input_buffer_0_ptr = memory.GetPhysicalAddress(input_address);
    data.input_buffer_0_packet_count = 1;
    data.input_buffer_0_valid = 1;
    if (next_packet) {
      data.input_buffer_1_ptr =
          null_next_address ? 0 : memory.GetPhysicalAddress(second_address);
      data.input_buffer_1_packet_count = 1;
      data.input_buffer_1_valid = 1;
    }
    data.input_buffer_read_offset = first_bit;
    data.output_buffer_ptr = memory.GetPhysicalAddress(output_address);
    data.output_buffer_block_count = 15;
    data.output_buffer_valid = 1;
    data.sample_rate = rate_id;
    data.subframe_decode_count = 4;
    data.Store(memory.TranslateVirtual(context_address));
    context.Enable();
    context.Work();
    XMA_CONTEXT_DATA result(memory.TranslateVirtual(context_address));
    size_t bytes = result.output_buffer_write_offset * 256;
    return {result.error_status, std::vector<uint8_t>(output, output + bytes)};
  }

  Result Refill(XmaContextNew& context, const Packet& packet,
                const Packet* following = nullptr, bool null_address = false) {
    auto guest = memory.TranslateVirtual(context_address);
    XMA_CONTEXT_DATA data(guest);
    const uint32_t address =
        data.current_buffer ? second_address : input_address;
    std::memcpy(memory.TranslateVirtual(address), packet.data(), packet.size());
    if (data.current_buffer) {
      data.input_buffer_1_ptr = memory.GetPhysicalAddress(address);
      data.input_buffer_1_packet_count = 1;
      data.input_buffer_1_valid = 1;
    } else {
      data.input_buffer_0_valid = 1;
    }
    if (following) {
      const uint32_t other =
          data.current_buffer ? input_address : second_address;
      std::memcpy(memory.TranslateVirtual(other), following->data(),
                  following->size());
      if (data.current_buffer) {
        data.input_buffer_0_ptr = memory.GetPhysicalAddress(other);
        data.input_buffer_0_packet_count = 1;
        data.input_buffer_0_valid = 1;
      } else {
        data.input_buffer_1_ptr = memory.GetPhysicalAddress(other);
        data.input_buffer_1_packet_count = 1;
        data.input_buffer_1_valid = 1;
      }
    }
    data.output_buffer_valid = 1;
    if (null_address) {
      if (data.current_buffer) {
        data.input_buffer_1_ptr = 0;
      } else {
        data.input_buffer_0_ptr = 0;
      }
    }
    data.Store(guest);
    context.Enable();
    context.Work();
    XMA_CONTEXT_DATA result(guest);
    auto output = memory.TranslateVirtual(output_address);
    return {result.error_status,
            std::vector<uint8_t>(
                output, output + result.output_buffer_write_offset * 256)};
  }

 private:
  Memory memory;
  uint32_t context_address = 0;
  uint32_t input_address = 0;
  uint32_t output_address = 0;
  uint32_t second_address = 0;
};
}  // namespace

TEST_CASE("XMA new preserves split frames across delayed refill",
          "[apu][xma]") {
  bool long_frame = false;
  bool skip_packet = false;
  uint32_t first_bit = 16373;
  SECTION("split header") {}
  SECTION("split body") {
    long_frame = true;
    first_bit = 32;
  }
  SECTION("continuation after a skipped packet") { skip_packet = true; }
  const auto frame = SilenceFrame(long_frame);
  auto first = MakePacket(frame, first_bit);
  auto next = ContinuationPacket(frame, 16384 - first_bit);
  first[2] &= ~7;  // XMA1, matching the captured stream.
  next[2] &= ~7;
  if (skip_packet) {
    first[3] = 1;
  }

  Fixture fixture;
  XmaContextNew delayed;
  fixture.Setup(delayed);
  fixture.Decode(delayed, first, first_bit, 0);
  const auto skipped = MakePacket(SilenceFrame(false), 32);
  const auto actual = skip_packet ? fixture.Refill(delayed, skipped, &next)
                                  : fixture.Refill(delayed, next);
  // Two encoded 512-sample mono silence frames must yield 2048 zero bytes.
  // This checks the public output, independently of decoder internals.
  REQUIRE(actual.pcm == std::vector<uint8_t>(2048, 0));
}

TEST_CASE("XMA new rejects undersized delayed split frame headers",
          "[apu][xma]") {
  Fixture fixture;
  XmaContextNew context;
  fixture.Setup(context);
  std::vector<bool> frame(31, false);
  frame[14] = true;  // Malformed length 1: shorter than the header itself.
  const auto first = MakePacket(frame, 16373);
  const auto next = ContinuationPacket(frame, 11);
  fixture.Decode(context, first, 16373, 0);
  const auto actual = fixture.Refill(context, next);
  REQUIRE(actual.error == 4);
}

TEST_CASE("XMA new rejects null continuation buffer addresses", "[apu][xma]") {
  Fixture fixture;
  XmaContextNew context;
  fixture.Setup(context);
  const auto frame = SilenceFrame(false);
  const auto first = MakePacket(frame, 16373);
  const auto next = ContinuationPacket(frame, 11);
  Result actual;
  SECTION("already valid continuation") {
    actual = fixture.Decode(context, first, 16373, 0, &next, true);
  }
  SECTION("delayed continuation") {
    fixture.Decode(context, first, 16373, 0);
    actual = fixture.Refill(context, next, nullptr, true);
  }
  REQUIRE(actual.error == 4);
}
}  // namespace xe::apu::test
