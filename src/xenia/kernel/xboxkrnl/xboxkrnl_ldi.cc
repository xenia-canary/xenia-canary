/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>
#include <mutex>
#include <unordered_map>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/ldi_decompressor.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

// The handle is the guest address of a context starting with kLdiMagic, in
// the title's scratch memory or from its allocator.
namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

enum : uint32_t {
  kMdiErrorNoError = 0,
  kMdiErrorNotEnoughMemory = 1,
  kMdiErrorBadParameters = 2,
  kMdiErrorBufferOverflow = 3,
  kMdiErrorFailed = 4,
};

constexpr uint32_t kLdiCompressedFrameGrowth = 0x1800;
constexpr uint32_t kLdiContextSize = 0x2FF8;
// Seen with a 32 KiB window
constexpr uint32_t kLdiWindowExtra = 0x105;
constexpr fourcc_t kLdiMagic = make_fourcc("CIDL");
// Accepted window sizes. lzxd only decodes 32 KiB to 2 MiB, so LDIDecompress
// fails outside that.
constexpr uint32_t kLdiMinWindowBits = 12;
constexpr uint32_t kLdiMaxWindowBits = 22;

struct X_LZXDECOMPRESS {
  xe::be<int32_t> window_size;
  xe::be<int32_t> cpu_type;
};
static_assert_size(X_LZXDECOMPRESS, 0x8);

struct LdiContext {
  uint32_t window_size;
  uint32_t data_block_max;
  // Null for windows lzxd can't decode
  std::unique_ptr<util::LdiDecompressor> decompressor;
  // Zero for scratch memory
  uint32_t free_fn = 0;
  uint32_t context_alloc = 0;
  uint32_t window_alloc = 0;
};

std::mutex g_ldi_mutex;
std::unordered_map<uint32_t, std::unique_ptr<LdiContext>> g_ldi_contexts;

uint32_t CallLdiCallback(uint32_t function, uint32_t arg) {
  auto thread = XThread::GetCurrentThread();

  if (!thread) {
    XELOGE("LDI: callback {:08X} called from a host thread", function);
    return 0;
  }

  uint64_t args[] = {arg};
  return static_cast<uint32_t>(kernel_state()->processor()->Execute(
      thread->thread_state(), function, args, xe::countof(args)));
}

void FreeLdiAllocations(const LdiContext& context) {
  if (!context.free_fn) {
    return;
  }

  // The window first, then the context
  if (context.window_alloc) {
    CallLdiCallback(context.free_fn, context.window_alloc);
  }

  if (context.context_alloc) {
    CallLdiCallback(context.free_fn, context.context_alloc);
  }
}

xe::be<uint32_t>* LdiMagic(uint32_t handle) {
  return kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(handle);
}

// Call with g_ldi_mutex held. Checks the registry before reading guest memory.
LdiContext* LookupLdiContext(uint32_t handle) {
  auto it = g_ldi_contexts.find(handle);

  if (it == g_ldi_contexts.end() || *LdiMagic(handle) != kLdiMagic) {
    return nullptr;
  }

  return it->second.get();
}

bool IsGuestRangeValid(uint32_t address, uint32_t size) {
  if (!size) {
    return true;
  }

  const uint32_t last_address = address + size - 1;
  return last_address >= address &&
         kernel_memory()->LookupHeap(address) != nullptr &&
         kernel_memory()->LookupHeap(last_address) != nullptr;
}

}  // namespace

dword_result_t LDICreateDecompression_entry(lpdword_t data_block_max_ptr,
                                            pointer_t<X_LZXDECOMPRESS> config,
                                            lpvoid_t alloc_fn, lpvoid_t free_fn,
                                            lpvoid_t scratch,
                                            lpdword_t src_buffer_min_ptr,
                                            lpdword_t handle_ptr) {
  if (!data_block_max_ptr || !config) {
    return kMdiErrorBadParameters;
  }

  // Written even without a handle pointer, which succeeds
  const uint32_t data_block_max = *data_block_max_ptr;

  if (src_buffer_min_ptr) {
    *src_buffer_min_ptr = data_block_max + kLdiCompressedFrameGrowth;
  }

  if (!handle_ptr) {
    return kMdiErrorNoError;
  }

  *handle_ptr = 0;

  const uint32_t window_size = static_cast<uint32_t>(config->window_size);
  uint32_t window_bits;

  if (!xe::is_pow2(window_size) ||
      !xe::bit_scan_forward(window_size, &window_bits) ||
      window_bits < kLdiMinWindowBits || window_bits > kLdiMaxWindowBits) {
    XELOGE("LDICreateDecompression: bad window size {:08X}", window_size);
    return kMdiErrorNotEnoughMemory;
  }

  auto context = std::make_unique<LdiContext>();
  context->window_size = window_size;
  context->data_block_max = data_block_max;
  context->decompressor = util::LdiDecompressor::Create(window_size);

  uint32_t context_address = scratch.guest_address();

  if (alloc_fn) {
    context->free_fn = free_fn.guest_address();
    context->context_alloc =
        CallLdiCallback(alloc_fn.guest_address(), kLdiContextSize);

    if (context->context_alloc) {
      context->window_alloc = CallLdiCallback(alloc_fn.guest_address(),
                                              window_size + kLdiWindowExtra);
    }

    if (!context->window_alloc) {
      FreeLdiAllocations(*context);
      return kMdiErrorNotEnoughMemory;
    }

    context_address = context->context_alloc;
  }

  if (!context_address) {
    return kMdiErrorNotEnoughMemory;
  }

  *LdiMagic(context_address) = kLdiMagic;

  {
    std::lock_guard<std::mutex> lock(g_ldi_mutex);
    g_ldi_contexts[context_address] = std::move(context);
  }

  *handle_ptr = context_address;
  return kMdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kImplemented);

dword_result_t LDIDecompress_entry(dword_t handle, lpvoid_t src,
                                   dword_t src_size, lpvoid_t dst,
                                   lpdword_t dst_size_ptr) {
  std::lock_guard<std::mutex> lock(g_ldi_mutex);
  LdiContext* context = LookupLdiContext(handle);

  if (!context || !dst_size_ptr) {
    return kMdiErrorBadParameters;
  }

  uint32_t dst_size = *dst_size_ptr;

  if (!dst_size || dst_size > context->data_block_max) {
    return kMdiErrorBufferOverflow;
  }

  *dst_size_ptr = 0;

  if (!context->decompressor || dst_size > util::LdiDecompressor::kFrameSize) {
    XELOGE("LDIDecompress: can't decode 0x{:X} bytes with a {:08X} window",
           dst_size, context->window_size);
    return kMdiErrorFailed;
  }

  if (!IsGuestRangeValid(src.guest_address(), src_size) ||
      !IsGuestRangeValid(dst.guest_address(), dst_size)) {
    XELOGE(
        "LDIDecompress: invalid range (source {:08X} size {:08X}, destination "
        "{:08X} size {:08X})",
        src.guest_address(), src_size.value(), dst.guest_address(), dst_size);
    return kMdiErrorFailed;
  }

  if (!context->decompressor->Decompress(src.as<const uint8_t*>(), src_size,
                                         dst.as<uint8_t*>(), dst_size)) {
    XELOGE("LDIDecompress: LZX error (source size {}, output size {})",
           src_size.value(), dst_size);
    return kMdiErrorFailed;
  }

  *dst_size_ptr = dst_size;
  return kMdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kImplemented);

dword_result_t LDIResetDecompression_entry(dword_t handle) {
  std::lock_guard<std::mutex> lock(g_ldi_mutex);
  LdiContext* context = LookupLdiContext(handle);

  if (!context) {
    return kMdiErrorBadParameters;
  }

  if (context->decompressor && !context->decompressor->Reset()) {
    return kMdiErrorNotEnoughMemory;
  }

  return kMdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kSketchy);

dword_result_t LDIDestroyDecompression_entry(dword_t handle) {
  std::unique_ptr<LdiContext> context;

  {
    std::lock_guard<std::mutex> lock(g_ldi_mutex);

    if (!LookupLdiContext(handle)) {
      return kMdiErrorBadParameters;
    }

    auto it = g_ldi_contexts.find(handle);
    context = std::move(it->second);
    g_ldi_contexts.erase(it);
    *LdiMagic(handle) = 0;
  }

  // free_fn is title code, so outside the lock
  FreeLdiAllocations(*context);
  return kMdiErrorNoError;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ldi);
