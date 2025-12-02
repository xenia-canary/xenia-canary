/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_XMA_CONTEXT_FAKE_H_
#define XENIA_APU_XMA_CONTEXT_FAKE_H_

#include <array>
#include <atomic>
#include <mutex>
#include <queue>

#include "xenia/apu/xma_context.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

class XmaContextFake : public XmaContext {
 public:
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  explicit XmaContextFake();
  ~XmaContextFake() override;

  int Setup(uint32_t id, Memory* memory, uint32_t guest_ptr) override;
  bool Work() override;

  void Enable() override;
  bool Block(bool poll) override;
  void Clear() override;
  void Disable() override;
  void Release() override;

 private:
  void ProcessPacket(XMA_CONTEXT_DATA* data);
  void Consume(RingBuffer* output_rb, XMA_CONTEXT_DATA* data);
  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);

  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);
  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  // Fake frame data - generate silence
  std::array<uint8_t, kBytesPerFrameChannel * 2> fake_frame_;

  // Minimal state tracking
  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_XMA_CONTEXT_FAKE_H_
