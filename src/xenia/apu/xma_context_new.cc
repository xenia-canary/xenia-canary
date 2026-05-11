/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2024 Xenia Canary. All rights reserved.                          *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "xenia/apu/xma_context_new.h"
#include "xenia/apu/xma_helpers.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#include "third_party/FFmpeg/libavutil/channel_layout.h"
#include "third_party/FFmpeg/libavutil/error.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

namespace xe {
namespace apu {

XmaContextNew::XmaContextNew() = default;

XmaContextNew::~XmaContextNew() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContextNew::Setup(uint32_t id, Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    XELOGE("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    XELOGE("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->ch_layout = AVChannelLayout{};
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    XELOGE("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

RingBuffer XmaContextNew::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity =
      data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset =
      data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset =
      data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    XELOGW(
        "XmaContext {}: Output buffer uses more space than expected! "
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

bool XmaContextNew::Work() {
  if (!is_enabled() || !is_allocated()) {
    return false;
  }

  std::lock_guard<xe_mutex> lock(lock_);
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  RingBuffer output_rb = PrepareOutputRingBuffer(&data);

  if (data.IsConsumeOnlyContext()) {
    // Nothing to drain — don't touch the context or we'll reset the
    // game's output buffer offsets, causing stale PCM to be re-read.
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    XELOGAPU("XmaContext {}: Consume-only context, draining subframes", id());
    Consume(&output_rb, &data);
    data.output_buffer_write_offset =
        output_rb.write_offset() / kOutputBytesPerBlock;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use the number of subframes Consume() will actually write per iteration
  // (= subframe_decode_count, clamped to 1) plus any headroom requested by
  // output_buffer_padding.  Using a full-frame worth of space (the old
  // formula) was far too restrictive: games like TGM Ace use
  // subframe_decode_count=2 on a small ring buffer and never had 8 free
  // blocks available, causing the decoder to permanently stall.
  const uint32_t effective_sdc =
      std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + data.output_buffer_padding;

  // We don't have enough space to even make one pass
  // Waiting for decoder to return more space.
  if (minimum_subframe_decode_count >
      remaining_subframe_blocks_in_output_buffer_) {
    XELOGD("XmaContext {}: No space for subframe decoding {}/{}!", id(),
           minimum_subframe_decode_count,
           remaining_subframe_blocks_in_output_buffer_);
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >=
         minimum_subframe_decode_count) {
    XELOGAPU(
        "XmaContext {}: Write Count: {}, Capacity: {} - {} {} Subframes: {} "
        "Padding: {}",
        id(), (uint32_t)output_rb.write_count(),
        remaining_subframe_blocks_in_output_buffer_,
        data.input_buffer_0_valid + (data.input_buffer_1_valid << 1),
        data.output_buffer_valid, data.subframe_decode_count,
        data.output_buffer_padding);

    const uint32_t pre_decode_offset = data.input_buffer_read_offset;
    const uint8_t pre_remaining_subframes = current_frame_remaining_subframes_;

    Decode(&data);
    Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      XELOGAPU(
          "XmaContext {}: Work loop exit - buffers_valid={} error_status={}",
          id(), data.IsAnyInputBufferValid(), data.error_status);
      break;
    }

    // If Decode didn't advance the read offset and produced no new frame,
    // we can't make progress. Break to avoid spinning.
    // Only check when there were no pending subframes — if we entered this
    // iteration with subframes remaining, Decode() intentionally skipped
    // (offset unchanged) while Consume() drained the frame.
    if (pre_remaining_subframes == 0 &&
        data.input_buffer_read_offset == pre_decode_offset &&
        current_frame_remaining_subframes_ == 0) {
      XELOGAPU(
          "XmaContext {}: Decode stalled at offset {} (no progress), "
          "waiting for next buffer",
          id(), pre_decode_offset);
      break;
    }
  }

  data.output_buffer_write_offset =
      output_rb.write_offset() / kOutputBytesPerBlock;

  XELOGAPU("XmaContext {}: Read Output: {} Write Output: {}", id(),
           data.output_buffer_read_offset, data.output_buffer_write_offset);

  // That's a bit misleading due to nature of ringbuffer
  // when write and read offset matches it might mean that we wrote nothing
  // or we fully saturated allocated space.
  if (output_rb.empty()) {
    XELOGAPU("XmaContext {}: Output ring buffer empty, invalidating output",
             id());
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XmaContextNew::Enable() { set_is_enabled(true); }

void XmaContextNew::Clear() {
  std::lock_guard<xe_mutex> lock(lock_);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContextNew::ClearLocked(XMA_CONTEXT_DATA* data) {
  XELOGAPU("XmaContext: reset context {}", id());

  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
}

void XmaContextNew::Disable() { set_is_enabled(false); }

void XmaContextNew::Release() {
  // Lock it in case the decoder thread is working on it now.
  std::lock_guard<xe_mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));  // Zero it.
}

int XmaContextNew::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

void XmaContextNew::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  // No more frames.
  XELOGAPU("XmaContext: SwapInputBuffer from buffer {} to {}",
           data->current_buffer, data->current_buffer ^ 1);
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContextNew::Consume(RingBuffer* XE_RESTRICT output_rb,
                            const XMA_CONTEXT_DATA* const XE_RESTRICT data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  // Check if the loop end truncation limit has been reached.
  // Total subframes for this frame minus remaining gives how many have already
  // been consumed.  If that reaches the limit, discard the rest.
  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes =
        (kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo;
    const uint8_t consumed =
        total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      // Charge headroom as if the frame completed normally so the Work()
      // loop doesn't overestimate available output space.
      XELOGAPU(
          "XmaContext {}: Loop end truncation: discarding {} remaining "
          "subframes (limit {})",
          id(), current_frame_remaining_subframes_, loop_frame_output_limit_);
      remaining_subframe_blocks_in_output_buffer_ -=
          data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return;
    }
  }

  // Guard against subframe_decode_count == 0 which would cause zero progress
  // and an infinite loop in Work().  Treat 0 as 1 (minimum progress).
  const uint8_t effective_sdc =
      std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(
      (int8_t)current_frame_remaining_subframes_, (int8_t)effective_sdc);

  // Clamp to loop end limit if active.
  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes =
        (kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo;
    const uint8_t consumed =
        total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit =
        (int8_t)(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  output_rb->Write(
      raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
      subframes_to_write * kOutputBytesPerBlock);

  // Reserve extra blocks as headroom when unk_skip_decode is set.
  // Only apply when the frame is fully consumed to avoid double-counting.
  const int8_t headroom =
      (current_frame_remaining_subframes_ - subframes_to_write == 0)
          ? data->output_buffer_padding
          : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;

  XELOGAPU("XmaContext {}: Consume: {} - {} - {} - {} - {}", id(),
           remaining_subframe_blocks_in_output_buffer_,
           data->output_buffer_write_offset, data->output_buffer_read_offset,
           output_rb->write_offset(), current_frame_remaining_subframes_);
}

void XmaContextNew::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  // No available data.
  if (!data->IsAnyInputBufferValid()) {
    XELOGAPU("XmaContext {}: Decode skipped - no valid input buffers", id());
    // data->error_status = 4;
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    XELOGAPU("XmaContext {}: Current buffer {} invalid, swapping to other",
             id(), data->current_buffer);
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      XELOGAPU("XmaContext {}: Both buffers invalid after swap, aborting",
               id());
      return;
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  // Detect if we're about to decode the loop end frame (before
  // UpdateLoopStatus may reset the offset).
  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  UpdateLoopStatus(data);

  if (!data->output_buffer_block_count) {
    XELOGE("XmaContext {}: Error - Received 0 for output_buffer_block_count!",
           id());
    return;
  }

  XELOGAPU(
      "Processing context {} (offset {}, buffer {}, ptr {:p}, output buffer "
      "{:08X}, output buffer count {})",
      id(), data->input_buffer_read_offset, data->current_buffer,
      static_cast<void*>(current_input_buffer), data->output_buffer_ptr,
      data->output_buffer_block_count);

  // Games like Dirt 2 can kick the decoder with read offset 0 (pointing into
  // the packet header) before filling in a valid offset. Clamp to the first
  // valid data position to avoid rejecting the packet entirely.
  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    XELOGW(
        "XmaContext {}: Read offset {} is inside packet header, clamping to {}",
        id(), data->input_buffer_read_offset, kBitsPerPacketHeader);
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count =
      current_input_size / kBytesPerPacket;

  const int16_t packet_index =
      GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    XELOGE("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
           data->input_buffer_read_offset);
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  // If the read offset is before the first frame in this packet we're in the
  // tail of a split frame from the previous packet.  We don't have the
  // beginning of that frame so skip ahead to the first complete frame.
  // This also guards against games that kick the decoder with an offset
  // pointing into the packet header (e.g. Dirt 2).
  if (relative_offset < packet_first_frame_offset) {
    XELOGAPU(
        "XmaContext {}: Skipping split frame tail in packet {} "
        "(offset {} -> first frame {})",
        id(), packet_index, relative_offset, packet_first_frame_offset);
    data->input_buffer_read_offset =
        (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }
  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip — no new frames begin in this packet (XMA2: 0xFF,
  // XMA1: lower 8 bits of 0x7FF also reads as 0xFF).  Advance to the
  // next sequential packet instead of trying to parse frames.
  if (skip_count == 0xFF) {
    XELOGAPU("XmaContext {}: Full packet skip (0xFF) at packet {}/{}", id(),
             packet_index, current_input_packet_count);
    const uint32_t next_packet_index_skip = packet_index + 1;
    uint32_t next_input_offset = GetNextPacketReadOffset(
        data, next_packet_index_skip, current_input_packet_count);
    if (next_packet_index_skip >= current_input_packet_count ||
        next_input_offset == kBitsPerPacketHeader) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary — combine packets to read
  // the full 15-bit header and resolve the real frame size.
  // Only detected for XMA2 packets where the header provides an authoritative
  // frame count. XMA1 packets lack a frame count field so split headers
  // cannot be detected — if XMA1 encoders can produce them, those frames
  // will still be silently lost.
  if (packet_info.current_frame_size_ == 0) {
    XELOGAPU(
        "XmaContext {}: Split frame header at packet {} boundary, "
        "combining with next packet {}",
        id(), packet_index, next_packet_index);
    const uint8_t* next_packet =
        GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // Next buffer not available yet.  We can't resolve the split header
      // without it, so consume (swap) the current buffer and move on.
      XELOGAPU(
          "XmaContext {}: Split frame header at packet {}, next buffer "
          "unavailable — swapping input buffer",
          id(), packet_index);
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader,
                kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData,
                next_packet + kBytesPerPacketHeader, kBytesPerPacketData);

    BitStream combined(input_buffer_.data(),
                       (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      XELOGW(
          "XmaContext {}: Split header resolved to kMaxFrameLength (0x7FFF), "
          "setting error_status=4",
          id());
      // Matching split-body error handling below; correct error code unknown.
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = (uint32_t)frame_size;
  }

  BitStream stream =
      BitStream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(
      (uint32_t)stream.BitsRemaining(), packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    XELOGE("XmaContext {}: There is no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    // Frame is a splitted frame
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);

      if (!next_packet) {
        // Error path
        // Decoder probably should return error here
        // Not sure what error code should be returned
        data->error_status = 4;
        return;
      }
      // Copy next packet to buffer
      std::memcpy(input_buffer_.data() + kBytesPerPacketData,
                  next_packet + kBytesPerPacketHeader, kBytesPerPacketData);
    }
  }

  // Copy current packet to buffer
  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader,
              kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(),
                     (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  XELOGAPU(
      "XmaContext {}: Reading Frame {}/{} (size: {}) From Packet "
      "{}/{}",
      id(), (int32_t)packet_info.current_frame_, packet_info.frame_count_,
      packet_info.current_frame_size_, packet_index,
      current_input_packet_count);

  const uint32_t padding_start = static_cast<uint8_t>(
      stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  PreparePacket(packet_info.current_frame_size_, padding_start);
  if (DecodePacket(av_context_, av_packet_, av_frame_)) {
    // dump_raw(av_frame_, id());
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data),
                 bool(data->is_stereo), raw_frame_.data());
    current_frame_remaining_subframes_ = 4 << data->is_stereo;

    // Loop end: limit output to subframes 0..loop_subframe_end.
    if (is_loop_end_frame) {
      loop_frame_output_limit_ = (data->loop_subframe_end + 1)
                                 << data->is_stereo;
      XELOGAPU(
          "XmaContext {}: Loop end frame - limiting output to {} subframes "
          "(loop_subframe_end={})",
          id(), loop_frame_output_limit_, data->loop_subframe_end);
    } else {
      loop_frame_output_limit_ = 0;
    }

    // Loop start: skip leading subframes per loop_subframe_skip.
    // Reducing remaining shifts the read offset forward in Consume().
    if (loop_start_skip_pending_) {
      const uint8_t skip = data->loop_subframe_skip << data->is_stereo;
      if (skip < current_frame_remaining_subframes_) {
        XELOGAPU(
            "XmaContext {}: Loop start - skipping {} leading subframes "
            "(loop_subframe_skip={})",
            id(), skip, data->loop_subframe_skip);
        current_frame_remaining_subframes_ -= skip;
      }
      loop_start_skip_pending_ = false;
    }
  }

  // Compute where to go next.
  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;

    XELOGAPU("XmaContext {}: Index: {}/{} - Next frame offset: {}", id(),
             (int32_t)packet_info.current_frame_, packet_info.frame_count_,
             next_frame_offset);

    data->input_buffer_read_offset =
        (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  const uint8_t* next_packet =
      GetNextPacket(data, next_packet_index, current_input_packet_count);

  if (!next_packet) {
    XELOGAPU(
        "XmaContext {}: Last frame in packet {}, next packet {} unavailable "
        "(end of buffer)",
        id(), packet_index, next_packet_index);
  }

  uint32_t next_input_offset = GetNextPacketReadOffset(
      data, next_packet_index, current_input_packet_count);

  if (next_packet_index >= current_input_packet_count ||
      next_input_offset == kBitsPerPacketHeader) {
    SwapInputBuffer(data);
  }

  if (next_input_offset == kBitsPerPacketHeader) {
    // We're at start of next buffer
    // If it have any frame in this packet decoder should go to first frame in
    // packet If it doesn't have any frame then it should immediatelly go to
    // next packet
    if (data->IsAnyInputBufferValid()) {
      next_input_offset = xma::GetPacketFrameOffset(
          memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()));

      if (next_input_offset > kMaxFrameSizeinBits) {
        XELOGAPU(
            "XmaContext {}: Next buffer contains no frames in packet! Frame "
            "offset: {}",
            id(), next_input_offset);
        SwapInputBuffer(data);
        return;
      }
      XELOGAPU("XmaContext {}: Next buffer first frame starts at: {}", id(),
               next_input_offset);
    }
  }
  data->input_buffer_read_offset = next_input_offset;
  return;
}

//  Frame & Packet searching methods

void XmaContextNew::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  XELOGAPU("XmaContext {}: Looped Data: {} < {} (Start: {}) Remaining: {}",
           id(), data->input_buffer_read_offset, data->loop_end,
           data->loop_start, data->loop_count);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count != 255) {
    data->loop_count--;
  }
}

kPacketHandle XmaContextNew::GetPacketHandle(
    XMA_CONTEXT_DATA* data, uint32_t buffer_index, uint32_t packet_index,
    uint32_t current_input_packet_count) {
  kPacketHandle result{};
  bool is_packet_in_next_buffer = packet_index >= current_input_packet_count;
  if (is_packet_in_next_buffer) {
    buffer_index = buffer_index ^ 1;
    packet_index = packet_index - current_input_packet_count;
  }

  if (!data->IsInputBufferValid(buffer_index)) {
    return result;
  }

  const uint32_t next_buffer_address =
      data->GetInputBufferAddress(buffer_index);
  if (!next_buffer_address) {
    // This should never occur but there is always a chance
    XELOGE(
        "XmaContext {}: the packet is expected to be in the {} buffer, "
        "but the buffer marked valid is null pointer!",
        id(), is_packet_in_next_buffer ? "next" : "current");
    return result;
  }

  const uint32_t next_buffer_packet_count =
      data->GetInputBufferPacketCount(buffer_index);
  if (packet_index >= next_buffer_packet_count) {
    XELOGE(
        "XmaContext {}: the packet is expected to be in the {} buffer, "
        "but the buffer is too short to contain this packet!",
        id(), is_packet_in_next_buffer ? "next" : "current");
    return result;
  }

  result.buffer_index_ = buffer_index;
  result.packet_index_ = packet_index;
  result.is_valid_ = true;
  return result;
}

const uint8_t* XmaContextNew::GetNextPacket(
    XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
    uint32_t current_input_packet_count) {
  kPacketHandle packet_handle =
      GetPacketHandle(data, data->current_buffer, next_packet_index,
                      current_input_packet_count);
  if (!packet_handle.is_valid_) {
    return nullptr;
  }

  const uint32_t next_buffer_address =
      data->GetInputBufferAddress(packet_handle.buffer_index_);
  return memory()->TranslatePhysical(next_buffer_address) +
         packet_handle.packet_index_ * kBytesPerPacket;
}

const uint32_t XmaContextNew::GetNextPacketReadOffset(
    uint8_t* buffer, uint32_t next_packet_index,
    uint32_t current_input_packet_count) {
  // Scan forward for the next packet that contains a new frame.
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeinBits) {
      const uint32_t new_input_buffer_offset =
          (next_packet_index * kBitsPerPacket) + packet_frame_offset;

      XELOGAPU("XmaContext {}: new offset: {} packet_offset: {} packet: {}/{}",
               id(), new_input_buffer_offset, packet_frame_offset,
               next_packet_index, current_input_packet_count);
      return new_input_buffer_offset;
    }
    next_packet_index++;
  }

  return kBitsPerPacketHeader;
}

const uint32_t XmaContextNew::GetNextPacketReadOffset(
    XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
    uint32_t current_input_packet_count) {
  kPacketHandle packet_handle =
      GetPacketHandle(data, data->current_buffer, next_packet_index,
                      current_input_packet_count);
  if (!packet_handle.is_valid_) {
    return kBitsPerPacketHeader;
  }

  const uint32_t next_buffer_address =
      data->GetInputBufferAddress(packet_handle.buffer_index_);
  return GetNextPacketReadOffset(
      memory()->TranslatePhysical(next_buffer_address),
      packet_handle.packet_index_,
      data->GetInputBufferPacketCount(packet_handle.buffer_index_));
}

const uint32_t XmaContextNew::GetAmountOfBitsToRead(
    const uint32_t remaining_stream_bits, const uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

uint32_t XmaContextNew::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContextNew::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

const kPacketInfo XmaContextNew::GetPacketInfo(uint8_t* packet,
                                               uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  // Handling of splitted frame
  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = (uint32_t)frame_size;
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      // Last frame.
      break;
    }

    stream.Advance(frame_size - 1);

    // Read the trailing bit to see if frames follow
    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      // Frame header split across packet boundary — scanner couldn't
      // peek the full 15-bit header. Trust the XMA2 header count.
      if (packet_info.current_frame_size_ == 0) {
        // Current frame is the split-header frame
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    } else if (xma2_frame_count != packet_info.frame_count_) {
      XELOGE(
          "XmaContext {}: XMA2 packet header defines different amount of "
          "frames than internally found! (Header: {} Found: {})",
          id(), xma2_frame_count, packet_info.frame_count_);
    }
  }
  return packet_info;
}

int16_t XmaContextNew::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }

  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }

  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;

  return (int16_t)packet_number;
}

int XmaContextNew::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  // Re-initialize the context with new sample rate and channels.
  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->ch_layout.nb_channels != (int)channels) {
    XELOGAPU("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}",
             id(), av_context_->sample_rate, sample_rate,
             av_context_->ch_layout.nb_channels, channels);
    // We have to recreate the codec context so it'll realloc whatever data it
    // needs.
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_channel_layout_default(&av_context_->ch_layout, channels);
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      XELOGE("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContextNew::PreparePacket(const uint32_t frame_size,
                                  const uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size =
      static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                       (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContextNew::DecodePacket(AVCodecContext* av_context,
                                 const AVPacket* av_packet, AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    XELOGE("XmaContext {}: Error sending packet for decoding: {} ({})", id(),
           errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    // Codec needs more input before producing output (e.g. first frame warmup).
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    XELOGE("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContextNew::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                       const XMA_CONTEXT_DATA& initial_data,
                                       uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  // DWORD 0: decoder owns loop_count, output_buffer_write_offset.
  // Only clear valid flags the decoder actually consumed (was 1, now 0).
  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  // DWORD 1: decoder conditionally clears output_buffer_valid
  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  // DWORD 2: decoder owns input_buffer_read_offset, error_status
  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;

  // DWORD 4: decoder owns current_buffer
  fresh.current_buffer = data.current_buffer;

  // DWORD 9: decoder owns output_buffer_read_offset (reset by ClearLocked)
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

}  // namespace apu
}  // namespace xe
