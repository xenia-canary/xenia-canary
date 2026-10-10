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
#include "xenia/kernel/util/ldi_decompressor.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

// TODO(knuckleslee): Find the error codes the console returns.
constexpr X_STATUS kLdiError = X_STATUS_UNSUCCESSFUL;

std::mutex g_ldi_mutex;
std::unordered_map<uint32_t, std::unique_ptr<util::LdiDecompressor>>
    g_ldi_contexts;
uint32_t g_ldi_next_handle = 1;

// Checks that the first and last byte of a guest range are mapped.
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

dword_result_t LDICreateDecompression_entry(
    lpdword_t max_block_size_ptr, lpdword_t config_ptr, dword_t alloc_callback,
    dword_t free_callback, lpvoid_t unknown_ptr, lpdword_t max_source_size_ptr,
    lpdword_t handle_ptr) {
  if (!max_block_size_ptr || !config_ptr || !handle_ptr) {
    XELOGE(
        "LDICreateDecompression: null pointer (max_block_size_ptr {:08X}, "
        "config_ptr {:08X}, handle_ptr {:08X})",
        max_block_size_ptr.guest_address(), config_ptr.guest_address(),
        handle_ptr.guest_address());
    return kLdiError;
  }

  const uint32_t max_block_size = *max_block_size_ptr;
  if (max_block_size > util::LdiDecompressor::kFrameSize) {
    XELOGE("LDICreateDecompression: unsupported max block size {:08X}",
           max_block_size);
    return kLdiError;
  }

  const uint32_t window_size = config_ptr[0];
  auto context = util::LdiDecompressor::Create(window_size);
  if (!context) {
    XELOGE("LDICreateDecompression: unsupported window size {:08X}",
           window_size);
    return kLdiError;
  }

  if (max_source_size_ptr) {
    *max_source_size_ptr = util::LdiDecompressor::kMaxCompressedFrameSize;
  }

  std::lock_guard<std::mutex> lock(g_ldi_mutex);
  const uint32_t handle = g_ldi_next_handle++;
  g_ldi_contexts.emplace(handle, std::move(context));
  *handle_ptr = handle;
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDICreateDecompression, kNone, kSketchy);

dword_result_t LDIDecompress_entry(dword_t handle, lpvoid_t source_ptr,
                                   dword_t source_size,
                                   lpvoid_t destination_ptr,
                                   lpdword_t destination_size_ptr) {
  if (!source_ptr || !destination_ptr || !destination_size_ptr) {
    XELOGE(
        "LDIDecompress: null pointer (source_ptr {:08X}, destination_ptr "
        "{:08X}, destination_size_ptr {:08X})",
        source_ptr.guest_address(), destination_ptr.guest_address(),
        destination_size_ptr.guest_address());
    return kLdiError;
  }
  if (!source_size) {
    XELOGE("LDIDecompress: source_size is 0");
    return kLdiError;
  }

  std::lock_guard<std::mutex> lock(g_ldi_mutex);
  auto it = g_ldi_contexts.find(handle);
  if (it == g_ldi_contexts.end()) {
    XELOGE("LDIDecompress: invalid handle {:08X}", handle.value());
    return kLdiError;
  }

  uint32_t destination_size = *destination_size_ptr;
  if (!IsGuestRangeValid(source_ptr.guest_address(), source_size) ||
      !IsGuestRangeValid(destination_ptr.guest_address(), destination_size)) {
    XELOGE(
        "LDIDecompress: invalid range (source {:08X} size {:08X}, destination "
        "{:08X} size {:08X})",
        source_ptr.guest_address(), source_size.value(),
        destination_ptr.guest_address(), destination_size);
    return kLdiError;
  }

  const bool succeeded =
      it->second->Decompress(source_ptr.as<uint8_t*>(), source_size,
                             destination_ptr.as<uint8_t*>(), destination_size);
  *destination_size_ptr = destination_size;
  if (!succeeded) {
    XELOGE("LDIDecompress: failed (source size {}, output size {})",
           source_size.value(), destination_size);
    return kLdiError;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDecompress, kNone, kSketchy);

dword_result_t LDIResetDecompression_entry(dword_t handle) {
  std::lock_guard<std::mutex> lock(g_ldi_mutex);
  auto it = g_ldi_contexts.find(handle);
  if (it == g_ldi_contexts.end()) {
    XELOGE("LDIResetDecompression: invalid handle {:08X}", handle.value());
    return kLdiError;
  }

  if (!it->second->Reset()) {
    XELOGE("LDIResetDecompression: lzxd_init failed for handle {:08X}",
           handle.value());
    return kLdiError;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDIResetDecompression, kNone, kSketchy);

dword_result_t LDIDestroyDecompression_entry(dword_t handle) {
  std::lock_guard<std::mutex> lock(g_ldi_mutex);
  if (!g_ldi_contexts.erase(handle)) {
    XELOGE("LDIDestroyDecompression: invalid handle {:08X}", handle.value());
    return kLdiError;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(LDIDestroyDecompression, kNone, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ldi);
