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

// Decodes one LZX chunk per call into one frame, keeping the window.
class LdiDecompressor {
 public:
  static constexpr uint32_t kFrameSize = 0x8000;

  // Returns nullptr outside lzxd's 32 KiB to 2 MiB windows.
  static std::unique_ptr<LdiDecompressor> Create(uint32_t window_size);
  ~LdiDecompressor();

  LdiDecompressor(const LdiDecompressor&) = delete;
  LdiDecompressor& operator=(const LdiDecompressor&) = delete;

  bool Reset();

  // destination_size is the frame size in and the bytes written out.
  bool Decompress(const uint8_t* source, size_t source_size,
                  uint8_t* destination, uint32_t& destination_size);

 private:
  struct State;

  explicit LdiDecompressor(uint32_t window_bits);
  void Free();

  std::unique_ptr<State> state_;
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_LDI_DECOMPRESSOR_H_
