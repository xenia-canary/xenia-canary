/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/ldi_decompressor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "xenia/base/math.h"

#include "third_party/mspack/mspack.h"
// lzx.h needs off_t, which mspack.h includes.
#include "third_party/mspack/lzx.h"

namespace xe {
namespace kernel {
namespace util {

namespace {

constexpr uint32_t kMinWindowBits = 15;
constexpr uint32_t kMaxWindowBits = 21;

// One direction of lzxd I/O: a span of memory that is replaced before every
// call. mspack_system hands these back to the callbacks as mspack_file*.
struct LdiBuffer {
  uint8_t* data;
  size_t size;
  size_t offset;
};

int LdiRead(mspack_file* file, void* buffer, int bytes) {
  auto ldi_buffer = reinterpret_cast<LdiBuffer*>(file);
  const size_t count = std::min(static_cast<size_t>(bytes),
                                ldi_buffer->size - ldi_buffer->offset);
  std::memcpy(buffer, ldi_buffer->data + ldi_buffer->offset, count);
  ldi_buffer->offset += count;
  return static_cast<int>(count);
}

int LdiWrite(mspack_file* file, void* buffer, int bytes) {
  auto ldi_buffer = reinterpret_cast<LdiBuffer*>(file);
  const size_t count = std::min(static_cast<size_t>(bytes),
                                ldi_buffer->size - ldi_buffer->offset);
  std::memcpy(ldi_buffer->data + ldi_buffer->offset, buffer, count);
  ldi_buffer->offset += count;
  return static_cast<int>(count);
}

void* LdiAlloc(mspack_system* system, size_t bytes) {
  return std::calloc(bytes, 1);
}

void LdiFree(void* pointer) { std::free(pointer); }

void LdiCopy(void* source, void* destination, size_t bytes) {
  std::memcpy(destination, source, bytes);
}

void LdiMessage(mspack_file* file, const char* format, ...) {}

}  // namespace

struct LdiDecompressor::State {
  uint32_t window_bits;
  mspack_system system = {};
  LdiBuffer input = {};
  LdiBuffer output = {};
  lzxd_stream* stream = nullptr;
};

std::unique_ptr<LdiDecompressor> LdiDecompressor::Create(uint32_t window_size) {
  uint32_t window_bits;
  if (!xe::is_pow2(window_size) ||
      !xe::bit_scan_forward(window_size, &window_bits) ||
      window_bits < kMinWindowBits || window_bits > kMaxWindowBits) {
    return nullptr;
  }
  std::unique_ptr<LdiDecompressor> decompressor(
      new LdiDecompressor(window_bits));
  if (!decompressor->Reset()) {
    return nullptr;
  }
  return decompressor;
}

LdiDecompressor::LdiDecompressor(uint32_t window_bits)
    : state_(std::make_unique<State>()) {
  state_->window_bits = window_bits;
  mspack_system& system = state_->system;
  system.read = LdiRead;
  system.write = LdiWrite;
  system.alloc = LdiAlloc;
  system.free = LdiFree;
  system.copy = LdiCopy;
  system.message = LdiMessage;
}

LdiDecompressor::~LdiDecompressor() { Free(); }

bool LdiDecompressor::Reset() {
  Free();
  // No reset interval, a frame sized input buffer and the output length set
  // per frame.
  state_->stream =
      lzxd_init(&state_->system, reinterpret_cast<mspack_file*>(&state_->input),
                reinterpret_cast<mspack_file*>(&state_->output),
                state_->window_bits, 0, kFrameSize, 0, 0);
  return state_->stream != nullptr;
}

bool LdiDecompressor::Decompress(uint8_t* source, size_t source_size,
                                 uint8_t* destination,
                                 uint32_t& destination_size) {
  lzxd_stream* stream = state_->stream;
  if (!stream || !destination_size || destination_size > kFrameSize) {
    return false;
  }

  state_->input = {source, source_size, 0};
  state_->output = {destination, destination_size, 0};

  // lzxd reads padding past the end of a frame; drop its buffered input so
  // the next block starts with a clean bit reader.
  stream->i_ptr = stream->i_end = stream->inbuf;
  stream->bit_buffer = 0;
  stream->bits_left = 0;
  stream->input_end = 0;
  lzxd_set_output_length(stream, stream->offset + destination_size);

  // Ending exactly on a frame boundary makes lzxd decode the next frame, so
  // decode one byte less first, then the last byte.
  int result = lzxd_decompress(stream, destination_size - 1);
  if (result == MSPACK_ERR_OK) {
    result = lzxd_decompress(stream, 1);
  }

  destination_size = static_cast<uint32_t>(state_->output.offset);
  return result == MSPACK_ERR_OK;
}

void LdiDecompressor::Free() {
  if (state_->stream) {
    lzxd_free(state_->stream);
    state_->stream = nullptr;
  }
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
