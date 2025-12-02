/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/xma_context_fake.h"
#include "xenia/apu/xma_helpers.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"

namespace xe {
namespace apu {

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

XmaContextFake::XmaContextFake() = default;

XmaContextFake::~XmaContextFake() = default;

int XmaContextFake::Setup(uint32_t id, Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Initialize fake frame to silence
  fake_frame_.fill(0);

  XELOGI("XmaContextFake {}: Setup complete", id);
  return 0;
}

RingBuffer XmaContextFake::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity =
      data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset =
      data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset =
      data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    XELOGW(
        "XmaContextFake {}: Output buffer uses more space than expected! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  // Output buffers are in raw PCM samples, 256 bytes per block.
  // Output buffer is a ring buffer. We need to write from the write offset
  // to the read offset.
  RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      (int32_t)output_rb.write_count() / kOutputBytesPerBlock;

  return output_rb;
}

bool XmaContextFake::Work() {
  if (!is_enabled() || !is_allocated()) {
    return false;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  if (!data.output_buffer_valid) {
    return true;
  }

  RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  const int32_t minimum_subframe_decode_count =
      (data.subframe_decode_count * 2) - 1;

  // We don't have enough space to even make one pass
  if (minimum_subframe_decode_count >
      remaining_subframe_blocks_in_output_buffer_) {
    XELOGD("XmaContextFake {}: No space for subframe decoding {}/{}!", id(),
           minimum_subframe_decode_count,
           remaining_subframe_blocks_in_output_buffer_);
    data.Store(context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >=
         minimum_subframe_decode_count) {
    XELOGAPU("XmaContextFake {}: Processing context (buffer {} {}/{} bits)",
             id(), data.current_buffer, data.input_buffer_read_offset,
             data.GetCurrentInputBufferPacketCount() * kBitsPerPacket);

    ProcessPacket(&data);
    Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      break;
    }
  }

  data.output_buffer_write_offset =
      output_rb.write_offset() / kOutputBytesPerBlock;

  XELOGAPU("XmaContextFake {}: Read Output: {} Write Output: {}", id(),
           data.output_buffer_read_offset, data.output_buffer_write_offset);

  // That's a bit misleading due to nature of ringbuffer
  // when write and read offset matches it might mean that we wrote nothing
  // or we fully saturated allocated space.
  if (output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  data.Store(context_ptr);
  return true;
}

void XmaContextFake::Enable() {
  std::lock_guard<xe_mutex> lock(lock_);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  XELOGAPU("XmaContextFake: kicking context {} (buffer {} {}/{} bits)", id(),
           data.current_buffer, data.input_buffer_read_offset,
           data.GetCurrentInputBufferPacketCount() * kBitsPerPacket);

  data.Store(context_ptr);
  set_is_enabled(true);
}

bool XmaContextFake::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContextFake::Clear() {
  std::lock_guard<xe_mutex> lock(lock_);
  XELOGAPU("XmaContextFake: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  data.input_buffer_0_valid = 0;
  data.input_buffer_1_valid = 0;
  data.output_buffer_valid = 0;

  data.input_buffer_read_offset = 0;
  data.output_buffer_read_offset = 0;
  data.output_buffer_write_offset = 0;
  data.input_buffer_read_offset = kBitsPerPacketHeader;

  current_frame_remaining_subframes_ = 0;
  data.Store(context_ptr);
}

void XmaContextFake::Disable() {
  std::lock_guard<xe_mutex> lock(lock_);
  XELOGAPU("XmaContextFake: disabling context {}", id());
  set_is_enabled(false);
}

void XmaContextFake::Release() {
  // Lock it in case the decoder thread is working on it now.
  std::lock_guard<xe_mutex> lock(lock_);
  assert_true(is_allocated_ == true);

  set_is_allocated(false);
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));  // Zero it.
}

int XmaContextFake::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

void XmaContextFake::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  // No more frames.
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContextFake::Consume(RingBuffer* output_rb, XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  const int8_t subframes_to_write =
      std::min((int8_t)current_frame_remaining_subframes_,
               (int8_t)data->subframe_decode_count);

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  output_rb->Write(
      fake_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
      subframes_to_write * kOutputBytesPerBlock);
  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write;
  current_frame_remaining_subframes_ -= subframes_to_write;

  XELOGAPU("XmaContextFake {}: Consume: {} - {} - {} - {} - {}", id(),
           remaining_subframe_blocks_in_output_buffer_,
           data->output_buffer_write_offset, data->output_buffer_read_offset,
           output_rb->write_offset(), current_frame_remaining_subframes_);
}

void XmaContextFake::ProcessPacket(XMA_CONTEXT_DATA* data) {
  // No available data.
  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  UpdateLoopStatus(data);

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_packet_index =
      data->input_buffer_read_offset / kBitsPerPacket;

  // if we've processed all packets in current buffer, switch to next
  if (current_packet_index >= current_input_size / kBytesPerPacket) {
    SwapInputBuffer(data);
    return;
  }

  // just move to next packet
  const uint32_t next_packet_index = current_packet_index + 1;
  if (next_packet_index >= current_input_size / kBytesPerPacket) {
    SwapInputBuffer(data);
  } else {
    // Advance to next packet in current buffer
    data->input_buffer_read_offset =
        next_packet_index * kBitsPerPacket + kBitsPerPacketHeader;
  }

  // fixed subframe count
  current_frame_remaining_subframes_ = 4 << data->is_stereo;

  XELOGAPU(
      "XmaContextFake {}: Processed packet {}, set up {} subframes for "
      "consumption",
      id(), current_packet_index, current_frame_remaining_subframes_);
}

void XmaContextFake::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  XELOGAPU("XmaContextFake {}: Looped Data: {} < {} (Start: {}) Remaining: {}",
           id(), data->input_buffer_read_offset, data->loop_end,
           data->loop_start, data->loop_count);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;

  if (data->loop_count != 255) {
    data->loop_count--;
  }
}

uint32_t XmaContextFake::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContextFake::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

}  // namespace apu
}  // namespace xe
