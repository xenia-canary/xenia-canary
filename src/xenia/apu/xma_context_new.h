/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_XMA_CONTEXT_NEW_H_
#define XENIA_APU_XMA_CONTEXT_NEW_H_

#include <array>
#include <atomic>
#include <mutex>
#include <queue>

#include "xenia/apu/xma_context.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

// Forward declarations
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace xe {
namespace apu {

struct kPacketInfo {
  uint8_t frame_count_;
  uint8_t current_frame_;
  uint32_t current_frame_size_;

  const bool isLastFrameInPacket() const {
    return frame_count_ == 0 || current_frame_ == frame_count_ - 1;
  }
};

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

class XmaContextNew : public XmaContext {
 public:
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  explicit XmaContextNew();
  ~XmaContextNew();

  int Setup(uint32_t id, Memory* memory, uint32_t guest_ptr);
  bool Work();

  void Enable();
  void Clear();
  void Disable();
  void Release();

 private:
  void NoteNoSpaceStall(const XMA_CONTEXT_DATA& data,
                        int32_t minimum_subframe_decode_count);
  void NoteNoProgressStall(const XMA_CONTEXT_DATA& data);
  void NoteProgress(const XMA_CONTEXT_DATA& data, const RingBuffer& output_rb,
                    uint32_t previous_input_offset);

  void ClearLocked(XMA_CONTEXT_DATA* data);
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  // Convert sampling rate from ID to frequency.
  static int GetSampleRate(int id);
  // Get the containing packet number of the frame pointed to by the offset.
  static int16_t GetPacketNumber(size_t size, size_t bit_offset);

  const kPacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset);

  const uint32_t GetAmountOfBitsToRead(const uint32_t remaining_stream_bits,
                                       const uint32_t frame_size);

  const uint8_t* GetNextPacket(XMA_CONTEXT_DATA* data,
                               uint32_t next_packet_index,
                               uint32_t current_input_packet_count);

  const uint32_t GetNextPacketReadOffset(uint8_t* buffer,
                                         uint32_t next_packet_index,
                                         uint32_t current_input_packet_count);

  // Returns currently used buffer
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);

  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(RingBuffer* XE_RESTRICT output_rb,
               const XMA_CONTEXT_DATA* const XE_RESTRICT data);

  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  void PreparePacket(const uint32_t frame_size, const uint32_t frame_padding);

  RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);

  bool DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                    AVFrame* av_frame);

  // Re-reads context from guest memory and merges only decoder-owned fields,
  // preserving any game modifications made during decoding.
  void StoreContextMerged(const XMA_CONTEXT_DATA& data,
                          const XMA_CONTEXT_DATA& initial_data,
                          uint8_t* context_ptr);

  std::array<uint8_t, kBytesPerPacketData * 2> input_buffer_;
  // first byte contains bit offset information
  std::array<uint8_t, 1 + 4096> xma_frame_;
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;

  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;

  // Loop subframe precision state.
  // Maximum subframe blocks to output from the current frame (loop end
  // truncation).  0 means no limit.
  uint8_t loop_frame_output_limit_ = 0;
  // When true, the next decoded frame should skip leading subframes per
  // loop_subframe_skip (loop start adjustment).
  bool loop_start_skip_pending_ = false;

  uint32_t consecutive_no_space_stalls_ = 0;
  uint32_t consecutive_no_progress_stalls_ = 0;
  uint32_t total_no_space_stalls_ = 0;
  uint32_t total_no_progress_stalls_ = 0;
  uint32_t total_progress_events_ = 0;
  uint32_t last_progress_input_offset_ = kBitsPerPacketHeader;
  uint8_t last_progress_output_read_offset_ = 0;
  uint8_t last_progress_output_write_offset_ = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XMA_CONTEXT_H_
