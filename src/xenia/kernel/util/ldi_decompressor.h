/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_LDI_DECOMPRESSOR_H_
#define XENIA_KERNEL_UTIL_LDI_DECOMPRESSOR_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace xe {
namespace kernel {
namespace util {

// Block based LZX decompression on top of libmspack's lzxd, as used by the LDI
// exports: one frame per call, sharing the window between calls.
class LdiDecompressor {
 public:
  static constexpr uint32_t kFrameSize = 0x8000;
  // Maximum growth of a compressed frame, as in libmspack (CAB_INPUTMAX).
  static constexpr uint32_t kMaxFrameGrowth = 6144;
  static constexpr uint32_t kMaxCompressedFrameSize =
      kFrameSize + kMaxFrameGrowth;

  // Returns nullptr if window_size isn't a supported LZX window size.
  static std::unique_ptr<LdiDecompressor> Create(uint32_t window_size);
  ~LdiDecompressor();

  LdiDecompressor(const LdiDecompressor&) = delete;
  LdiDecompressor& operator=(const LdiDecompressor&) = delete;

  // Starts a new stream. Returns false if lzxd can't be set up again.
  bool Reset();

  // destination_size is the expected frame size on input and the number of
  // bytes written on output.
  bool Decompress(uint8_t* source, size_t source_size, uint8_t* destination,
                  uint32_t& destination_size);

 private:
  // libmspack state, kept out of this header.
  struct State;

  explicit LdiDecompressor(uint32_t window_bits);
  void Free();

  std::unique_ptr<State> state_;
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_LDI_DECOMPRESSOR_H_
