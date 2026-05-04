/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/xex_module_mac.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"

#include "xenia/base/byte_order_mac.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math_mac.h"
#include "xenia/base/memory_mac.h"
#include "xenia/base/pe_image.h"

#include "xenia/cpu/cpu_flags_mac.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/lzx.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator_mac.h"
#include "xenia/kernel/kernel_state_mac.h"
#include "xenia/kernel/user_module_mac.h"
#include "xenia/kernel/xmodule.h"

#include "third_party/crypto/TinySHA1.hpp"
#include "third_party/crypto/rijndael-alg-fst.c"
#include "third_party/crypto/rijndael-alg-fst.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_instr.h"
DEFINE_bool(disable_instruction_infocache, false,
            "Disables caching records of called instructions/mmio accesses.",
            "CPU");

DEFINE_bool(writable_code_segments, false,
            "Enables a program to write to its own code segments in memory.",
            "CPU");

DEFINE_bool(
    enable_early_precompilation, false,
    "Enable pre-compiling guest functions that we know we've called/that "
    "we've recognized as being functions via simple heuristics, good for error "
    "finding/stress testing with the JIT",
    "CPU");

DECLARE_bool(allow_plugins);
DECLARE_bool(trace_gpu_bootstrap);
DEFINE_bool(xex_log_imports_gpu, false,
            "Log GPU-related XEX imports (Vd* ordinals) at module load.",
            "CPU");
DEFINE_bool(xex_log_basic_decode_blocks, false,
            "Log basic-compressed XEX decode progress for each block.",
            "CPU");
DEFINE_uint32(
    xex_stage_watchdog_log_interval_ms, 1000,
    "Interval in milliseconds for XEX module-load stage watchdog logs.",
    "CPU");
DEFINE_uint32(
    xex_basic_decode_heartbeat_bytes, 262144,
    "Byte interval for module-load progress heartbeats while decoding "
    "basic-compressed encrypted XEX blocks.",
    "CPU");
DEFINE_uint32(
    xex_basic_decode_heartbeat_ms, 250,
    "Maximum heartbeat silence interval in milliseconds while decoding "
    "basic-compressed encrypted XEX blocks.",
    "CPU");
DEFINE_uint32(
    xex_basic_decode_progress_log_bytes, 1048576,
    "Progress-log byte interval while decoding encrypted basic-compressed "
    "XEX blocks.",
    "CPU");
DEFINE_uint32(
    xex_basic_decode_progress_log_interval_ms, 500,
    "Maximum silence interval in milliseconds between progress logs while "
    "decoding encrypted basic-compressed XEX blocks.",
    "CPU");
#if XE_PLATFORM_MAC
constexpr bool kDefaultXexBasicDecodeZeroFill = false;
#else
constexpr bool kDefaultXexBasicDecodeZeroFill = true;
#endif
DEFINE_bool(
  xex_basic_decode_zero_fill,
  kDefaultXexBasicDecodeZeroFill,
  "Explicitly memset zero-fill regions for basic-compressed XEX blocks. "
  "Can be disabled to avoid host stalls on large zeroed tails.",
  "CPU");
constexpr bool kDefaultStageBasicEncryptedSourceCopy = false;
DEFINE_bool(
    xex_basic_decode_stage_encrypted_source_copy,
    kDefaultStageBasicEncryptedSourceCopy,
  "Stage encrypted basic-compressed XEX source data into owned memory with "
  "padding before block decrypt (opt-in diagnostic workaround for host mmap "
  "tail access faults).",
    "CPU");

static constexpr uint8_t xe_xex1_retail_key[16] = {
    0xA2, 0x6C, 0x10, 0xF7, 0x1F, 0xD9, 0x35, 0xE9,
    0x8B, 0x99, 0x92, 0x2C, 0xE9, 0x32, 0x15, 0x72};
static constexpr uint8_t xe_xex2_retail_key[16] = {
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};
static constexpr uint8_t xe_xex2_devkit_key[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void aes_decrypt_buffer(const uint8_t* session_key, const uint8_t* input_buffer,
                        const size_t input_size, uint8_t* output_buffer,
                        const size_t output_size) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16] = {0};
  int32_t Nr = rijndaelKeySetupDec(rk, session_key, 128);
  const uint8_t* ct = input_buffer;
  uint8_t* pt = output_buffer;
  for (size_t n = 0; n < input_size; n += 16, ct += 16, pt += 16) {
    // Decrypt 16 uint8_ts from input -> output.
    rijndaelDecrypt(rk, Nr, ct, pt);
    for (size_t i = 0; i < 16; i++) {
      // XOR with previous.
      pt[i] ^= ivec[i];
      // Set previous.
      ivec[i] = ct[i];
    }
  }
}

void aes_decrypt_buffer_with_ivec(const uint8_t* session_key,
                                  const uint8_t* input_buffer,
                                  const size_t input_size,
                                  const uint8_t initial_ivec[16],
                                  uint8_t* output_buffer,
                                  uint8_t out_final_ivec[16]) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16];
  std::memcpy(ivec, initial_ivec, sizeof(ivec));
  int32_t Nr = rijndaelKeySetupDec(rk, session_key, 128);
  const uint8_t* ct = input_buffer;
  uint8_t* pt = output_buffer;
  for (size_t n = 0; n < input_size; n += 16, ct += 16, pt += 16) {
    rijndaelDecrypt(rk, Nr, ct, pt);
    for (size_t i = 0; i < 16; i++) {
      pt[i] ^= ivec[i];
      ivec[i] = ct[i];
    }
  }
  if (out_final_ivec) {
    std::memcpy(out_final_ivec, ivec, sizeof(ivec));
  }
}

namespace {

void SecureZeroBuffer(void* buffer, size_t buffer_size) {
  auto* bytes = reinterpret_cast<volatile uint8_t*>(buffer);
  while (buffer_size--) {
    *bytes++ = 0;
  }
}

class ScopedStageWatchdog final {
 public:
  ScopedStageWatchdog(std::string stage_name, std::string path,
                      std::function<void()> heartbeat_fn = {})
      : stage_name_(std::move(stage_name)),
        path_(std::move(path)),
        heartbeat_fn_(std::move(heartbeat_fn)),
        begin_ms_(xe::Clock::QueryHostUptimeMillis()) {
    watchdog_thread_ = std::thread([this]() { Run(); });
  }

  ScopedStageWatchdog(const ScopedStageWatchdog&) = delete;
  ScopedStageWatchdog& operator=(const ScopedStageWatchdog&) = delete;

  ~ScopedStageWatchdog() { Stop(); }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) {
        return;
      }
      stopped_ = true;
    }
    condition_.notify_all();
    if (watchdog_thread_.joinable()) {
      watchdog_thread_.join();
    }
  }

 private:
  void Run() {
    uint64_t interval_ms =
        std::max<uint64_t>(1, cvars::xex_stage_watchdog_log_interval_ms);
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
      if (condition_.wait_for(lock, std::chrono::milliseconds(interval_ms),
                              [this]() { return stopped_; })) {
        break;
      }
      lock.unlock();
      uint64_t elapsed_ms = xe::Clock::QueryHostUptimeMillis() - begin_ms_;
      XELOGW("[DEBUG] {} still in progress ({} ms, path='{}')", stage_name_,
             static_cast<unsigned long long>(elapsed_ms), path_);
      if (heartbeat_fn_) {
        heartbeat_fn_();
      }
      lock.lock();
    }
  }

  std::string stage_name_;
  std::string path_;
  std::function<void()> heartbeat_fn_;
  uint64_t begin_ms_ = 0;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread watchdog_thread_;
  bool stopped_ = false;
};

}  // namespace

namespace xe {
namespace cpu {

using xe::kernel::KernelState;

XexModule::XexModule(Processor* processor, KernelState* kernel_state)
    : Module(processor), processor_(processor), kernel_state_(kernel_state) {}

XexModule::~XexModule() {}

bool XexModule::GetOptHeader(const xex2_header* header, xex2_header_keys key,
                             void** out_ptr) {
  assert_not_null(header);
  assert_not_null(out_ptr);

  for (uint32_t i = 0; i < header->header_count; i++) {
    const xex2_opt_header& opt_header = header->headers[i];
    if (opt_header.key == key) {
      // Match!
      switch (key & 0xFF) {
        case 0x00: {
          // We just return the value of the optional header.
          // Assume that the output pointer points to a uint32_t.
          *reinterpret_cast<uint32_t*>(out_ptr) =
              static_cast<uint32_t>(opt_header.value);
        } break;
        case 0x01: {
          // Pointer to the value on the optional header.
          *out_ptr = const_cast<void*>(
              reinterpret_cast<const void*>(&opt_header.value));
        } break;
        default: {
          // Pointer to the header.
          *out_ptr =
              reinterpret_cast<void*>(uintptr_t(header) + opt_header.offset);
        } break;
      }

      return true;
    }
  }

  return false;
}

bool XexModule::GetOptHeader(xex2_header_keys key, void** out_ptr) const {
  return XexModule::GetOptHeader(xex_header(), key, out_ptr);
}

const void* XexModule::GetSecurityInfo(const xex2_header* header) {
  return reinterpret_cast<const void*>(uintptr_t(header) +
                                       header->security_offset);
}

const PESection* XexModule::GetPESection(const char* name) {
  for (std::vector<PESection>::iterator it = pe_sections_.begin();
       it != pe_sections_.end(); ++it) {
    if (!strcmp(it->name, name)) {
      return &(*it);
    }
  }
  return nullptr;
}

uint32_t XexModule::GetProcAddress(uint16_t ordinal) const {
  // First: Check the xex2 export table.
  if (xex_security_info()->export_table) {
    auto export_table = memory()->TranslateVirtual<const xex2_export_table*>(
        xex_security_info()->export_table);

    ordinal -= export_table->base;
    if (ordinal >= export_table->count) {
      XELOGE("GetProcAddress({:03X}): ordinal out of bounds", ordinal);
      return 0;
    }

    uint32_t num = ordinal;
    uint32_t ordinal_offset = export_table->ordOffset[num];
    ordinal_offset += export_table->imagebaseaddr << 16;
    return ordinal_offset;
  }

  // Second: Check the PE exports.
  assert_not_zero(base_address_);

  xex2_opt_data_directory* pe_export_directory = 0;
  if (GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &pe_export_directory)) {
    auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
        base_address_ + pe_export_directory->offset);
    assert_not_null(e);

    uint32_t* function_table =
        reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfFunctions);

    if (ordinal < e->NumberOfFunctions) {
      return base_address_ + function_table[ordinal];
    }
  }

  return 0;
}

uint32_t XexModule::GetProcAddress(const std::string_view name) const {
  assert_not_zero(base_address_);

  xex2_opt_data_directory* pe_export_directory = 0;
  if (!GetOptHeader(XEX_HEADER_EXPORTS_BY_NAME, &pe_export_directory)) {
    // No exports by name.
    return 0;
  }

  auto e = memory()->TranslateVirtual<const X_IMAGE_EXPORT_DIRECTORY*>(
      base_address_ + pe_export_directory->offset);
  assert_not_null(e);

  // e->AddressOfX RVAs are relative to the IMAGE_EXPORT_DIRECTORY!
  uint32_t* function_table =
      reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfFunctions);

  // Names relative to directory
  uint32_t* name_table =
      reinterpret_cast<uint32_t*>(uintptr_t(e) + e->AddressOfNames);

  // Table of ordinals (by name)
  uint16_t* ordinal_table =
      reinterpret_cast<uint16_t*>(uintptr_t(e) + e->AddressOfNameOrdinals);

  for (uint32_t i = 0; i < e->NumberOfNames; i++) {
    auto fn_name = reinterpret_cast<const char*>(uintptr_t(e) + name_table[i]);
    uint16_t ordinal = ordinal_table[i];
    uint32_t addr = base_address_ + function_table[ordinal];
    if (name == std::string_view(fn_name)) {
      // We have a match!
      return addr;
    }
  }

  // No match
  return 0;
}

int XexModule::ApplyPatch(XexModule* module) {
  if (!is_patch()) {
    // This isn't a XEX2 patch.
    return 1;
  }

  const uint32_t original_base_address = module->base_address();

  // Grab the delta descriptor and get to work.
  xex2_opt_delta_patch_descriptor* patch_header = nullptr;
  GetOptHeader(XEX_HEADER_DELTA_PATCH_DESCRIPTOR,
               reinterpret_cast<void**>(&patch_header));
  assert_not_null(patch_header);

  uint32_t size = module->xex_header()->header_size;
  if (patch_header->delta_headers_source_offset > size) {
    XELOGE("XEX header patch source is outside base XEX header area");
    return 2;
  }

  uint32_t header_size_available =
      size - patch_header->delta_headers_source_offset;
  if (patch_header->delta_headers_source_size > header_size_available) {
    XELOGE("XEX header patch source is too large");
    return 3;
  }

  if (patch_header->delta_headers_target_offset >
      patch_header->size_of_target_headers) {
    XELOGE("XEX header patch target is outside base XEX header area");
    return 4;
  }

  uint32_t delta_target_size = patch_header->size_of_target_headers -
                               patch_header->delta_headers_target_offset;
  if (patch_header->delta_headers_source_size > delta_target_size) {
    return 5;  // ? unsure what the point of this test is, kernel checks for it
               // though
  }

  // Patch base XEX header
  uint32_t original_image_size = module->image_size();
  uint32_t header_target_size = patch_header->size_of_target_headers;

  if (!header_target_size) {
    header_target_size = patch_header->delta_headers_target_offset +
                         patch_header->delta_headers_source_size;
  }

  //size_t mem_size = module->xex_header_mem_.size();
#ifdef __APPLE__
  (void)mem_size;
#endif

  // Increase xex header buffer length if needed
  if (header_target_size > module->xex_header_mem_.size()) {
    module->xex_header_mem_.resize(header_target_size);
  }

  auto* header_ptr = module->xex_header_mem_.data();

  // If headers_source_offset is set, copy [source_offset:source_size] to
  // target_offset
  if (patch_header->delta_headers_source_offset) {
    memcpy(header_ptr + patch_header->delta_headers_target_offset,
           header_ptr + patch_header->delta_headers_source_offset,
           patch_header->delta_headers_source_size);
  }

  // If new size is smaller than original, null out the difference
  if (header_target_size < module->xex_header_mem_.size()) {
    SecureZeroBuffer(header_ptr + header_target_size,
                     module->xex_header_mem_.size() - header_target_size);
  }

  auto file_format_header = opt_file_format_info();
  assert_not_null(file_format_header);

  // Apply header patch...
  uint32_t headerpatch_size = patch_header->size;

  int result_code = lzxdelta_apply_patch(
      &patch_header->info, headerpatch_size,
      file_format_header->compression_info.normal.window_size, header_ptr);
  if (result_code) {
    XELOGE("XEX header patch application failed, error code {}", result_code);
    return result_code;
  }

  // Decrease xex header buffer length if needed (but only after patching)
  if (module->xex_header_mem_.size() > header_target_size) {
    module->xex_header_mem_.resize(header_target_size);
  }

  // Update security info context with latest security info data
  module->ReadSecurityInfo();

  uint32_t new_image_size = module->image_size();

  // Check if we need to alloc new memory for the patched xex
  if (new_image_size > original_image_size) {
    uint32_t size_delta = new_image_size - original_image_size;
    uint32_t addr_new_mem = module->base_address_ + original_image_size;

    // Before we allocate new range we must check if patch haven't modified
    // base_address.
    uint32_t new_base_address = module->base_address();
    xe::be<uint32_t>* base_addr_opt = nullptr;
    if (module->GetOptHeader(XEX_HEADER_IMAGE_BASE_ADDRESS, &base_addr_opt)) {
      new_base_address = *base_addr_opt;
    }

    if (original_base_address != new_base_address) {
      XELOGW(
          "Patch for module: {} changed base_address from {:08X} to {:08X}, "
          "need to reallocate xex "
          "data!",
          module->name(), module->base_address_, new_base_address);
      module->base_address_ = new_base_address;
      addr_new_mem = new_base_address;
      size_delta = new_image_size;
    }

    bool alloc_result =
        memory()
            ->LookupHeap(addr_new_mem)
            ->AllocFixed(
                addr_new_mem, size_delta, 4096,
                xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                xe::kMemoryProtectRead | xe::kMemoryProtectWrite);

    if (!alloc_result) {
      XELOGE("Unable to allocate XEX memory at {:08X}-{:08X}.", addr_new_mem,
             size_delta);
      assert_always();
      return 6;
    }

    // For base_address change we need to copy data from previous allocation to
    // new one
    if (original_base_address != new_base_address) {
      kernel_state_->memory()->Copy(new_base_address, original_base_address,
                                    original_image_size);
    }
  }

  uint8_t orig_session_key[0x10];
  memcpy(orig_session_key, module->session_key_, 0x10);

  // Header patch updated the base XEX key, need to redecrypt it
  aes_decrypt_buffer(
      module->is_dev_kit_ ? xe_xex2_devkit_key : xe_xex2_retail_key,
      reinterpret_cast<const uint8_t*>(module->xex_security_info()->aes_key),
      16, module->session_key_, 16);

  // Decrypt the patch XEX's key using base XEX key
  aes_decrypt_buffer(
      module->session_key_,
      reinterpret_cast<const uint8_t*>(xex_security_info()->aes_key), 16,
      session_key_, 16);

  // Test delta key against our decrypted keys
  // (kernel doesn't seem to check this, but it's the one use for the
  // image_key_source field I can think of...)
  uint8_t test_delta_key[0x10];
  aes_decrypt_buffer(module->session_key_, patch_header->image_key_source, 0x10,
                     test_delta_key, 0x10);

  if (memcmp(test_delta_key, orig_session_key, 0x10) != 0) {
    XELOGE("XEX patch image key doesn't match original XEX!");
    return 7;
  }

  // Decrypt (if needed).
  uint8_t* allocated_input_buffer = nullptr;
  uint8_t* patch_buffer = xexp_data_mem_.data();
  const size_t patch_length = xexp_data_mem_.size();

  uint8_t* input_buffer = patch_buffer;

  switch (file_format_header->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      // No-op.
      break;
    case XEX_ENCRYPTION_NORMAL:
      // TODO: a way to do without a copy/alloc?
      allocated_input_buffer =
          static_cast<uint8_t*>(std::calloc(1, patch_length));
      input_buffer = allocated_input_buffer;
      aes_decrypt_buffer(session_key_, patch_buffer, patch_length, input_buffer,
                         patch_length);
      break;
    default:
      assert_always();
      return 8;
  }

  const xex2_compressed_block_info* cur_block =
      &file_format_header->compression_info.normal.first_block;

  uint8_t* p = input_buffer;
  uint8_t* base_exe = memory()->TranslateVirtual(module->base_address_);

  // If image_source_offset is set, copy [source_offset:source_size] to
  // target_offset
  if (patch_header->delta_image_source_offset) {
    memcpy(base_exe + patch_header->delta_image_target_offset,
           base_exe + patch_header->delta_image_source_offset,
           patch_header->delta_image_source_size);
  }

  // TODO: should we use new_image_size here instead?
  uint32_t image_target_size = patch_header->delta_image_target_offset +
                               patch_header->delta_image_source_size;

  // If new size is smaller than original, null out the difference
  if (image_target_size < original_image_size) {
    SecureZeroBuffer(base_exe + image_target_size,
                     original_image_size - image_target_size);
  }

  uint8_t digest[0x14];
  sha1::SHA1 s;
  // Now loop through each block and apply the delta patches inside
  while (cur_block->block_size) {
    const auto* next_block =
        reinterpret_cast<const xex2_compressed_block_info*>(p);

    // Compare block hash, if no match we probably used wrong decrypt key
    s.reset();
    s.processBytes(p, cur_block->block_size);
    s.finalize(digest);

    if (memcmp(digest, cur_block->block_hash, 0x14) != 0) {
      result_code = 9;
      XELOGE("XEX patch block hash doesn't match hash inside block info!");
      break;
    }

    // skip block info
    p += 20;
    p += 4;

    uint32_t block_data_size = cur_block->block_size - 20 - 4;

    // Apply delta patch
    result_code = lzxdelta_apply_patch(
        reinterpret_cast<xex2_delta_patch*>(p), block_data_size,
        file_format_header->compression_info.normal.window_size, base_exe);
    if (result_code) {
      break;
    }

    p += block_data_size;
    cur_block = next_block;
  }

  if (!result_code) {
    // Decommit unused pages if new image size is smaller than original
    if (original_image_size > new_image_size) {
      uint32_t size_delta = original_image_size - new_image_size;
      uint32_t addr_free_mem = module->base_address_ + new_image_size;

      bool free_result = memory()
                             ->LookupHeap(addr_free_mem)
                             ->Decommit(addr_free_mem, size_delta);

      if (!free_result) {
        XELOGE("Unable to decommit XEX memory at {:08X}-{:08X}.", addr_free_mem,
               size_delta);
        assert_always();
      }
    }

    xex2_version source_ver, target_ver;
    source_ver = patch_header->source_version();
    target_ver = patch_header->target_version();
    XELOGI(
        "XEX patch applied successfully: base version: {}.{}.{}.{}, new "
        "version: {}.{}.{}.{}",
        source_ver.major, source_ver.minor, source_ver.build, source_ver.qfe,
        target_ver.major, target_ver.minor, target_ver.build, target_ver.qfe);
  } else {
    XELOGE("XEX patch application failed, error code {}", result_code);
  }

  if (allocated_input_buffer) {
    std::free(allocated_input_buffer);
  }
  return result_code;
}

int XexModule::ReadImage(const void* xex_addr, size_t xex_length,
                         const uint8_t* key) {
  uint64_t t0 = Clock::QueryHostUptimeMillis();
  if (!opt_file_format_info()) {
    return 1;
  }

  XELOGI(
      "[DEBUG] XexModule::ReadImage begin path='{}' len={} compression={} "
      "encryption={} is_patch={}",
      path_, static_cast<unsigned long long>(xex_length),
      static_cast<unsigned int>(opt_file_format_info()->compression_type),
      static_cast<unsigned int>(opt_file_format_info()->encryption_type),
      is_patch() ? "YES" : "NO");

  is_dev_kit_ = key[0] == 0x00;

  if (is_patch()) {
    // Make a copy of patch data for other XEX's to use with ApplyPatch()
    const uint32_t data_len =
        static_cast<uint32_t>(xex_length - xex_header()->header_size);
    const auto* xex_bytes = static_cast<const uint8_t*>(xex_addr);
    xexp_data_mem_.resize(data_len);
    std::memcpy(xexp_data_mem_.data(),
                xex_bytes + xex_header()->header_size, data_len);
    XELOGI("[DEBUG] XexModule::ReadImage patch short path completed in {} ms",
           static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                           t0));
    return 0;
  }

  memory()->LookupHeap(base_address_)->Reset();

  aes_decrypt_buffer(
      key, reinterpret_cast<const uint8_t*>(xex_security_info()->aes_key), 16,
      session_key_, 16);

  XELOGI(
      "[DEBUG] XexModule::ReadImage decoder dispatch begin "
      "compression_type={} path='{}'",
      static_cast<unsigned int>(opt_file_format_info()->compression_type),
      path_);

  int result_code = 0;
  switch (opt_file_format_info()->compression_type) {
    case XEX_COMPRESSION_NONE:
      result_code = ReadImageUncompressed(xex_addr, xex_length);
      break;
    case XEX_COMPRESSION_BASIC:
      result_code = ReadImageBasicCompressed(xex_addr, xex_length);
      XELOGI(
          "[DEBUG] XexModule::ReadImage checkpoint after ReadImageBasicCompressed "
          "result={} path='{}'",
          result_code, path_);
      break;
    case XEX_COMPRESSION_NORMAL:
      result_code = ReadImageCompressed(xex_addr, xex_length);
      break;
    default:
      assert_always();
      return 2;
  }

  XELOGI(
      "[DEBUG] XexModule::ReadImage decoder dispatch returned "
      "compression_type={} result={} elapsed={} ms",
      static_cast<unsigned int>(opt_file_format_info()->compression_type),
      result_code,
      static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0));

  if (result_code) {
    XELOGW("[DEBUG] XexModule::ReadImage failed in {} ms result={}",
           static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                           t0),
           result_code);
    return result_code;
  }

  bool patch_image = is_patch();
  XELOGI(
      "[DEBUG] XexModule::ReadImage post-decode transition "
      "path='{}' patch={} base={:08X} image_size={:08X}",
      path_, patch_image ? "YES" : "NO", base_address_, image_size());
    XELOGI(
      "[DEBUG] XexModule::ReadImage validation begin path='{}' patch={} "
      "base={:08X} image_size={:08X}",
      path_, patch_image ? "YES" : "NO", base_address_, image_size());
  bool valid_executable = patch_image || is_valid_executable();
    XELOGI(
      "[DEBUG] XexModule::ReadImage validation end valid_executable={} "
      "elapsed={} ms",
      valid_executable ? "YES" : "NO",
      static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0));
  XELOGI(
      "[DEBUG] XexModule::ReadImage executable validation result={} "
      "(patch={})",
      valid_executable ? "YES" : "NO", patch_image ? "YES" : "NO");
  if (valid_executable) {
    XELOGI("[DEBUG] XexModule::ReadImage success in {} ms",
           static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                           t0));
    return 0;
  }

  // Not a patch and image doesn't have proper PE header, return 3
  XELOGW("[DEBUG] XexModule::ReadImage invalid executable after {} ms",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0));
  return 3;
}

int XexModule::ReadImageUncompressed(const void* xex_addr, size_t xex_length) {
  uint64_t t0 = Clock::QueryHostUptimeMillis();
  XELOGI("[DEBUG] XexModule::ReadImageUncompressed begin path='{}'", path_);

  // Allocate in-place the XEX memory.
  const uint32_t exe_length =
      static_cast<uint32_t>(xex_length - xex_header()->header_size);

  uint32_t uncompressed_size = exe_length;
  bool alloc_result =
      memory()
          ->LookupHeap(base_address_)
          ->AllocFixed(
              base_address_, uncompressed_size, 4096,
              xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
              xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
  if (!alloc_result) {
    XELOGE("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
           uncompressed_size);
    return 2;
  }
  uint8_t* buffer = memory()->TranslateVirtual(base_address_);
  std::memset(buffer, 0, uncompressed_size);

  const auto* xex_bytes = static_cast<const uint8_t*>(xex_addr);
  const uint8_t* p = xex_bytes + xex_header()->header_size;

  switch (opt_file_format_info()->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      if (exe_length > uncompressed_size) {
        return 1;
      }
      memcpy(buffer, p, exe_length);
      XELOGI("[DEBUG] XexModule::ReadImageUncompressed end {} ms (unencrypted)",
             static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                             t0));
      return 0;
    case XEX_ENCRYPTION_NORMAL:
      aes_decrypt_buffer(session_key_, p, exe_length, buffer,
                         uncompressed_size);
      XELOGI("[DEBUG] XexModule::ReadImageUncompressed end {} ms (decrypted)",
             static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                             t0));
      return 0;
    default:
      assert_always();
      return 1;
  }

  return 0;
}

int XexModule::ReadImageBasicCompressed(const void* xex_addr,
                                        size_t xex_length) {
  uint64_t t0 = Clock::QueryHostUptimeMillis();
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed begin path='{}'", path_);

  auto* file_info = opt_file_format_info();
  auto& comp_info = file_info->compression_info.basic;

  const uint32_t exe_length =
      static_cast<uint32_t>(xex_length - xex_header()->header_size);
#ifdef __APPLE__
  (void)exe_length;
#endif
  const auto* xex_bytes = static_cast<const uint8_t*>(xex_addr);
  const uint8_t* source_buffer = xex_bytes + xex_header()->header_size;
  static thread_local std::vector<uint8_t> staged_source_buffer;
  static thread_local std::vector<uint8_t> decrypted_source_buffer;
  staged_source_buffer.clear();
  decrypted_source_buffer.clear();
  const uint8_t* p = source_buffer;

  auto heap = memory()->LookupHeap(base_address_);

  // Calculate uncompressed length.
  uint32_t uncompressed_size = 0;
  uint64_t compressed_data_size_from_headers = 0;

  uint32_t block_count = (file_info->info_size - 8) / 8;
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed block_count={}",
         static_cast<unsigned long long>(block_count));
  for (uint32_t n = 0; n < block_count; n++) {
    const uint32_t data_size = comp_info.blocks[n].data_size;
    const uint32_t zero_size = comp_info.blocks[n].zero_size;
    XELOGI(
        "[DEBUG] XexModule::ReadImageBasicCompressed header block={} data={} "
        "zero={}",
        static_cast<unsigned long long>(n),
        static_cast<unsigned long long>(data_size),
        static_cast<unsigned long long>(zero_size));
    compressed_data_size_from_headers += data_size;
    uncompressed_size += data_size + zero_size;
  }

  if (compressed_data_size_from_headers >
      static_cast<uint64_t>(exe_length)) {
    XELOGE(
        "XEX basic-compressed header data exceeds payload: header_data={} "
        "payload={}",
        static_cast<unsigned long long>(compressed_data_size_from_headers),
        static_cast<unsigned long long>(exe_length));
    return 1;
  }
  if (opt_file_format_info()->encryption_type == XEX_ENCRYPTION_NORMAL) {
    staged_source_buffer.resize(
        static_cast<size_t>(compressed_data_size_from_headers) + 16, 0);
    if (compressed_data_size_from_headers != 0) {
      std::memcpy(staged_source_buffer.data(), source_buffer,
                  static_cast<size_t>(compressed_data_size_from_headers));
    }
    const size_t encrypted_size =
        static_cast<size_t>(compressed_data_size_from_headers);
    // Keep a small tail guard for host memcpy implementations that may
    // over-read source bytes in vectorized paths near buffer end.
    decrypted_source_buffer.resize(encrypted_size + 16, 0);
    if (encrypted_size != 0) {
      aes_decrypt_buffer(session_key_, staged_source_buffer.data(), encrypted_size,
                         decrypted_source_buffer.data(), encrypted_size);
    }
    source_buffer = decrypted_source_buffer.data();
    p = source_buffer;
    XELOGI(
        "[DEBUG] XexModule::ReadImageBasicCompressed staged encrypted source copy "
        "bytes={} (tail_padding=16)",
        static_cast<unsigned long long>(compressed_data_size_from_headers));
  }
  const uint8_t* source_end =
      source_buffer + static_cast<size_t>(compressed_data_size_from_headers);

  // Calculate the total size of the XEX image from its headers.
  uint32_t total_size = 0;
  for (uint32_t i = 0; i < xex_security_info()->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(xex_security_info()->page_descriptors[i].value);

    total_size += desc.page_count * heap->page_size();
  }
  XELOGI(
      "[DEBUG] XexModule::ReadImageBasicCompressed stage=image_size_ready "
      "total_size={} uncompressed_size={} page_desc_count={}",
      static_cast<unsigned long long>(total_size),
      static_cast<unsigned long long>(uncompressed_size),
      static_cast<unsigned long long>(xex_security_info()->page_descriptor_count));

  // Allocate in-place the XEX memory.
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed stage=alloc.begin "
         "base={:08X} size={}",
         base_address_, static_cast<unsigned long long>(total_size));
  bool alloc_result = heap->AllocFixed(
      base_address_, total_size, 4096,
      xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
      xe::kMemoryProtectRead | xe::kMemoryProtectWrite);
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed stage=alloc.end "
         "result={} base={:08X}",
         alloc_result ? "SUCCESS" : "FAIL", base_address_);
  if (!alloc_result) {
    XELOGE("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
           uncompressed_size);
    return 1;
  }

  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed stage=translate.begin "
         "base={:08X}",
         base_address_);
  uint8_t* buffer = memory()->TranslateVirtual(base_address_);
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed stage=translate.end "
         "buffer_ptr={}",
         static_cast<void*>(buffer));
  if (!buffer) {
    XELOGE("XEX basic-compressed output translation failed at {:08X}.",
           base_address_);
    return 1;
  }
  std::memset(buffer, 0, total_size);
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed output buffer ready "
         "base={:08X} total_size={}",
         base_address_, static_cast<unsigned long long>(total_size));
  uint8_t* d = buffer;
  uint8_t* d_end = buffer + total_size;
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed stage=decode_loop.begin "
         "blocks={} source_ptr={} source_end={} dest_ptr={} dest_end={}",
         static_cast<unsigned long long>(block_count),
         static_cast<const void*>(source_buffer),
         static_cast<const void*>(source_end), static_cast<void*>(d),
         static_cast<void*>(d_end));

  for (size_t n = 0; n < block_count; n++) {
#if XE_PLATFORM_MAC
    constexpr bool log_basic_decode_progress = false;
#else
    const bool log_basic_decode_progress = cvars::xex_log_basic_decode_blocks;
#endif
    const uint32_t data_size = comp_info.blocks[n].data_size;
    const uint32_t zero_size = comp_info.blocks[n].zero_size;
    uint64_t block_begin_ms = Clock::QueryHostUptimeMillis();
    size_t source_offset_begin = static_cast<size_t>(p - source_buffer);
    size_t dest_offset_begin = static_cast<size_t>(d - buffer);
    XELOGI(
        "[DEBUG] XexModule::ReadImageBasicCompressed decode block={}/{} begin "
        "data={} zero={} src_off={} dst_off={} encryption={}",
        static_cast<unsigned long long>(n + 1),
        static_cast<unsigned long long>(block_count),
        static_cast<unsigned long long>(data_size),
        static_cast<unsigned long long>(zero_size),
        static_cast<unsigned long long>(source_offset_begin),
        static_cast<unsigned long long>(dest_offset_begin),
        static_cast<unsigned int>(opt_file_format_info()->encryption_type));

    if (p + data_size > source_end) {
      XELOGE(
          "XEX basic-compressed input out of bounds: block={} data={} "
          "remaining={}",
          static_cast<unsigned long long>(n),
          static_cast<unsigned long long>(data_size),
          static_cast<unsigned long long>(source_end - p));
      return 1;
    }
    if (d + data_size + zero_size > d_end) {
      XELOGE(
          "XEX basic-compressed output overflow: block={} data={} zero={} "
          "remaining={}",
          static_cast<unsigned long long>(n),
          static_cast<unsigned long long>(data_size),
          static_cast<unsigned long long>(zero_size),
          static_cast<unsigned long long>(d_end - d));
      return 1;
    }

    switch (opt_file_format_info()->encryption_type) {
      case XEX_ENCRYPTION_NONE:
        if (data_size > uncompressed_size - (d - buffer)) {
          // Overflow.
          return 1;
        }
        std::memcpy(d, p, data_size);
        if (log_basic_decode_progress) {
          uint64_t elapsed = Clock::QueryHostUptimeMillis() - block_begin_ms;
          XELOGI(
              "[DEBUG] XexModule::ReadImageBasicCompressed block={}/{} progress={} "
              "/ {} bytes (100%) elapsed={} ms src_off={} dst_off={}",
              static_cast<unsigned long long>(n + 1),
              static_cast<unsigned long long>(block_count),
              static_cast<unsigned long long>(data_size),
              static_cast<unsigned long long>(data_size),
              static_cast<unsigned long long>(elapsed),
              static_cast<unsigned long long>(source_offset_begin + data_size),
              static_cast<unsigned long long>(dest_offset_begin + data_size));
        }
        break;
      case XEX_ENCRYPTION_NORMAL: {
        if (data_size & 0xF) {
          XELOGE(
              "XEX basic-compressed encrypted block has non-16-byte size: "
              "block={} data={}",
              static_cast<unsigned long long>(n),
              static_cast<unsigned long long>(data_size));
          return 1;
        }
        // Copy encrypted-block output in exact AES block strides to avoid any
        // undefined over-read/write behavior at final block boundaries.
        for (uint32_t i = 0; i < data_size; i += 16) {
          uint8_t block_copy[16];
          std::memcpy(block_copy, p + i, sizeof(block_copy));
          std::memcpy(d + i, block_copy, sizeof(block_copy));
        }
        if (log_basic_decode_progress) {
          uint64_t elapsed = Clock::QueryHostUptimeMillis() - block_begin_ms;
          XELOGI(
              "[DEBUG] XexModule::ReadImageBasicCompressed block={}/{} progress={} "
              "/ {} bytes (100%) elapsed={} ms src_off={} dst_off={}",
              static_cast<unsigned long long>(n + 1),
              static_cast<unsigned long long>(block_count),
              static_cast<unsigned long long>(data_size),
              static_cast<unsigned long long>(data_size),
              static_cast<unsigned long long>(elapsed),
              static_cast<unsigned long long>(source_offset_begin + data_size),
              static_cast<unsigned long long>(dest_offset_begin + data_size));
        }
      } break;
      default:
        assert_always();
        return 1;
    }

    p += data_size;
    d += data_size + zero_size;
    uint64_t block_elapsed_ms = Clock::QueryHostUptimeMillis() - block_begin_ms;
    if (block_elapsed_ms >= 1000) {
      XELOGW(
          "[DEBUG] XexModule::ReadImageBasicCompressed slow block={}/{} "
          "elapsed={} ms data={} zero={}",
          static_cast<unsigned long long>(n + 1),
          static_cast<unsigned long long>(block_count),
          static_cast<unsigned long long>(block_elapsed_ms),
          static_cast<unsigned long long>(data_size),
          static_cast<unsigned long long>(zero_size));
    }
    if (kernel_state_ && (n + 1) < block_count) {
      kernel_state_->NoteUserModuleLoadProgress();
    }
    XELOGI(
        "[DEBUG] XexModule::ReadImageBasicCompressed decoded block={}/{} "
        "block_elapsed={} ms total_elapsed={} ms src_off={} dst_off={}",
        static_cast<unsigned long long>(n + 1),
        static_cast<unsigned long long>(block_count),
        static_cast<unsigned long long>(block_elapsed_ms),
        static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0),
        static_cast<unsigned long long>(p - source_buffer),
        static_cast<unsigned long long>(d - buffer));
  }
  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed stage=decode_loop.end "
         "final_src_off={} final_dst_off={}",
         static_cast<unsigned long long>(p - source_buffer),
         static_cast<unsigned long long>(d - buffer));

  uint64_t decoded_source_bytes =
      static_cast<uint64_t>(p - source_buffer);
  uint64_t decoded_dest_bytes = static_cast<uint64_t>(d - buffer);
  if (decoded_source_bytes != compressed_data_size_from_headers) {
    XELOGE(
        "XEX basic-compressed decode consumed unexpected source bytes: "
        "consumed={} header_data={}",
        static_cast<unsigned long long>(decoded_source_bytes),
        static_cast<unsigned long long>(compressed_data_size_from_headers));
    return 1;
  }
  if (decoded_dest_bytes != static_cast<uint64_t>(uncompressed_size)) {
    XELOGE(
        "XEX basic-compressed decode produced unexpected output bytes: "
        "produced={} expected={}",
        static_cast<unsigned long long>(decoded_dest_bytes),
        static_cast<unsigned long long>(uncompressed_size));
    return 1;
  }

  XELOGI("[DEBUG] XexModule::ReadImageBasicCompressed end {} ms",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0));

  XELOGI(
      "[DEBUG] XexModule::ReadImageBasicCompressed returning success "
      "src_bytes={} dst_bytes={} expected_dst={}",
      static_cast<unsigned long long>(decoded_source_bytes),
      static_cast<unsigned long long>(decoded_dest_bytes),
      static_cast<unsigned long long>(uncompressed_size));

  return 0;
}

int XexModule::ReadImageCompressed(const void* xex_addr, size_t xex_length) {
  uint64_t t0 = Clock::QueryHostUptimeMillis();
  XELOGI("[DEBUG] XexModule::ReadImageCompressed begin path='{}'", path_);

  const uint32_t exe_length =
      static_cast<uint32_t>(xex_length - xex_header()->header_size);
  const auto* xex_bytes = static_cast<const uint8_t*>(xex_addr);
  const uint8_t* exe_buffer = xex_bytes + xex_header()->header_size;

  // src -> dest:
  // - decrypt (if encrypted)
  // - de-block:
  //    4b total size of next block in uint8_ts
  //   20b hash of entire next block (including size/hash)
  //    Nb block uint8_ts
  // - decompress block contents

  uint8_t* compress_buffer = nullptr;
  const uint8_t* p = nullptr;
  uint8_t* d = nullptr;
  sha1::SHA1 s;

  // Decrypt (if needed).
  uint8_t* allocated_input_buffer = nullptr;
  const uint8_t* input_buffer = exe_buffer;
  size_t input_size = exe_length;
#ifdef __APPLE__
  (void)input_size;
#endif

  switch (opt_file_format_info()->encryption_type) {
    case XEX_ENCRYPTION_NONE:
      // No-op.
      break;
    case XEX_ENCRYPTION_NORMAL:
      // TODO: a way to do without a copy/alloc?
      allocated_input_buffer =
          static_cast<uint8_t*>(std::calloc(1, exe_length));
      input_buffer = allocated_input_buffer;
      aes_decrypt_buffer(session_key_, exe_buffer, exe_length,
                         allocated_input_buffer, exe_length);
      break;
    default:
      assert_always();
      return 1;
  }

  const auto* compression_info = &opt_file_format_info()->compression_info;
  const xex2_compressed_block_info* cur_block =
      &compression_info->normal.first_block;

  compress_buffer = static_cast<uint8_t*>(std::calloc(1, exe_length));

  p = input_buffer;
  d = compress_buffer;

  // De-block.
  int result_code = 0;
  size_t block_index = 0;
  const uint8_t* input_end = input_buffer + input_size;
  uint8_t* compress_end = compress_buffer + exe_length;

  uint8_t block_calced_digest[0x14];
  while (cur_block->block_size) {
    if (p + cur_block->block_size > input_end) {
      XELOGE(
          "XEX compressed block out of bounds: index={} block_size={} "
          "remaining={}",
          static_cast<unsigned long long>(block_index),
          static_cast<unsigned long long>(cur_block->block_size),
          static_cast<unsigned long long>(input_end - p));
      result_code = 2;
      break;
    }

    const uint8_t* pnext = p + cur_block->block_size;
    const auto* next_block =
        reinterpret_cast<const xex2_compressed_block_info*>(p);

    // Compare block hash, if no match we probably used wrong decrypt key
    s.reset();
    s.processBytes(p, cur_block->block_size);
    s.finalize(block_calced_digest);
    if (memcmp(block_calced_digest, cur_block->block_hash, 0x14) != 0) {
      result_code = 2;
      break;
    }

    // skip block info
    p += 4;
    p += 20;

    while (true) {
      if (p + 2 > pnext) {
        XELOGE("XEX compressed chunk header out of bounds (block={})",
               static_cast<unsigned long long>(block_index));
        result_code = 2;
        break;
      }
      const size_t chunk_size = (p[0] << 8) | p[1];
      p += 2;
      if (!chunk_size) {
        break;
      }

      if (p + chunk_size > pnext) {
        XELOGE("XEX compressed chunk out of bounds (block={}, chunk={})",
               static_cast<unsigned long long>(block_index),
               static_cast<unsigned long long>(chunk_size));
        result_code = 2;
        break;
      }
      if (d + chunk_size > compress_end) {
        XELOGE(
            "XEX compressed output overflow (block={}, chunk={}, "
            "capacity_remaining={})",
            static_cast<unsigned long long>(block_index),
            static_cast<unsigned long long>(chunk_size),
            static_cast<unsigned long long>(compress_end - d));
        result_code = 2;
        break;
      }

      memcpy(d, p, chunk_size);
      p += chunk_size;
      d += chunk_size;
    }

    if (result_code) {
      break;
    }

    p = pnext;
    cur_block = next_block;
    ++block_index;
  }

  if (!result_code) {
    uint32_t uncompressed_size = image_size();

    // Allocate in-place the XEX memory.
    bool alloc_result =
        memory()
            ->LookupHeap(base_address_)
            ->AllocFixed(
                base_address_, uncompressed_size, 4096,
                xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                xe::kMemoryProtectRead | xe::kMemoryProtectWrite);

    if (alloc_result) {
      uint8_t* buffer = memory()->TranslateVirtual(base_address_);
      std::memset(buffer, 0, uncompressed_size);

      // Decompress into XEX base
      result_code = lzx_decompress(
          compress_buffer, d - compress_buffer, buffer, uncompressed_size,
          compression_info->normal.window_size, nullptr, 0);
    } else {
      XELOGE("Unable to allocate XEX memory at {:08X}-{:08X}.", base_address_,
             uncompressed_size);
      result_code = 3;
    }
  }

  if (compress_buffer) {
    std::free(compress_buffer);
  }
  if (allocated_input_buffer) {
    std::free(allocated_input_buffer);
  }
  XELOGI("[DEBUG] XexModule::ReadImageCompressed end {} ms result={}",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0),
         result_code);
  return result_code;
}

int XexModule::ReadPEHeaders() {
  const uint8_t* p = memory()->TranslateVirtual(base_address_);

  // Verify DOS signature (MZ).
  auto doshdr = reinterpret_cast<const XIMAGE_DOS_HEADER*>(p);
  if (doshdr->e_magic != XIMAGE_DOS_SIGNATURE) {
    XELOGE("PE signature mismatch; likely bad decryption/decompression");
    return 1;
  }

  // Move to the NT header offset from the DOS header.
  p += doshdr->e_lfanew;

  // Verify NT signature (PE\0\0).
  auto nthdr = reinterpret_cast<const XIMAGE_NT_HEADERS32*>(p);
  if (nthdr->Signature != XIMAGE_NT_SIGNATURE) {
    return 1;
  }

  // Verify matches an Xbox PE.
  const XIMAGE_FILE_HEADER* filehdr = &nthdr->FileHeader;
  if ((filehdr->Machine != XIMAGE_FILE_MACHINE_POWERPCBE) ||
      !(filehdr->Characteristics & XIMAGE_FILE_32BIT_MACHINE)) {
    return 1;
  }
  // Verify the expected size.
  if (filehdr->SizeOfOptionalHeader != XIMAGE_SIZEOF_NT_OPTIONAL_HEADER) {
    return 1;
  }

  // Verify optional header is 32bit.
  const XIMAGE_OPTIONAL_HEADER32* opthdr = &nthdr->OptionalHeader;
  if (opthdr->Magic != XIMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    return 1;
  }
  // Verify subsystem.
  if (opthdr->Subsystem != XIMAGE_SUBSYSTEM_XBOX) {
    return 1;
  }

  // Quick scan to determine bounds of sections.
  size_t upper_address = 0;
  const XIMAGE_SECTION_HEADER* sechdr = XIMAGE_FIRST_SECTION(nthdr);
  for (size_t n = 0; n < filehdr->NumberOfSections; n++, sechdr++) {
    const size_t physical_address = opthdr->ImageBase + sechdr->VirtualAddress;
    upper_address =
        std::max(upper_address, physical_address + sechdr->Misc.VirtualSize);
  }

  // Setup/load sections.
  sechdr = XIMAGE_FIRST_SECTION(nthdr);
  for (size_t n = 0; n < filehdr->NumberOfSections; n++, sechdr++) {
    PESection section;
    memcpy(section.name, sechdr->Name, sizeof(sechdr->Name));
    section.name[8] = 0;
    section.raw_address = sechdr->PointerToRawData;
    section.raw_size = sechdr->SizeOfRawData;
    section.address = base_address_ + sechdr->VirtualAddress;
    section.size = sechdr->Misc.VirtualSize;
    section.flags = sechdr->Characteristics;
    pe_sections_.push_back(section);
  }

  // DumpTLSDirectory(pImageBase, pNTHeader, (PIMAGE_TLS_DIRECTORY32)0);
  // DumpExportsSection(pImageBase, pNTHeader);
  return 0;
}

void XexModule::ReadSecurityInfo() {
  if (xex_format_ == kFormatXex1) {
    const xex1_security_info* xex1_sec_info =
        reinterpret_cast<const xex1_security_info*>(
            GetSecurityInfo(xex_header()));

    security_info_.rsa_signature = xex1_sec_info->rsa_signature;
    security_info_.aes_key = xex1_sec_info->aes_key;
    security_info_.image_size = xex1_sec_info->image_size;
    security_info_.image_flags = xex1_sec_info->image_flags;
    security_info_.export_table = xex1_sec_info->export_table;
    security_info_.load_address = xex1_sec_info->load_address;
    security_info_.page_descriptor_count = xex1_sec_info->page_descriptor_count;
    security_info_.page_descriptors = xex1_sec_info->page_descriptors;
  } else if (xex_format_ == kFormatXex2) {
    const xex2_security_info* xex2_sec_info =
        reinterpret_cast<const xex2_security_info*>(
            GetSecurityInfo(xex_header()));

    security_info_.rsa_signature = xex2_sec_info->rsa_signature;
    security_info_.aes_key = xex2_sec_info->aes_key;
    security_info_.image_size = xex2_sec_info->image_size;
    security_info_.image_flags = xex2_sec_info->image_flags;
    security_info_.export_table = xex2_sec_info->export_table;
    security_info_.load_address = xex2_sec_info->load_address;
    security_info_.page_descriptor_count = xex2_sec_info->page_descriptor_count;
    security_info_.page_descriptors = xex2_sec_info->page_descriptors;
  }
}

bool XexModule::Load(const std::string_view name, const std::string_view path,
                     const void* xex_addr, size_t xex_length) {
  uint64_t t0 = Clock::QueryHostUptimeMillis();
  XELOGI("[DEBUG] XexModule::Load begin name='{}' path='{}' length={}", name, path,
         static_cast<unsigned long long>(xex_length));

  auto src_header = reinterpret_cast<const xex2_header*>(xex_addr);

  if (src_header->magic == kXEX1Signature) {
    xex_format_ = kFormatXex1;
  } else if (src_header->magic == kXEX2Signature) {
    xex_format_ = kFormatXex2;
  } else {
    return false;
  }

  assert_false(loaded_);
  loaded_ = true;

  // Read in XEX headers
  xex_header_mem_.resize(src_header->header_size);
  std::memcpy(xex_header_mem_.data(), src_header, src_header->header_size);

  // Read/convert XEX1/XEX2 security info to a common format
  ReadSecurityInfo();

  auto sec_header = xex_security_info();
#ifdef __APPLE__
  (void)sec_header;
#endif

  // Try setting our base_address based on XEX_HEADER_IMAGE_BASE_ADDRESS, fall
  // back to xex_security_info otherwise
  base_address_ = xex_security_info()->load_address;
  xe::be<uint32_t>* base_addr_opt = nullptr;
  if (GetOptHeader(XEX_HEADER_IMAGE_BASE_ADDRESS, &base_addr_opt))
    base_address_ = *base_addr_opt;

  // Setup debug info.
  name_ = name;
  path_ = path;

  // Load in the XEX basefile
  // We'll try using both XEX2 keys to see if any give a valid PE
  uint64_t t_read_image_begin = Clock::QueryHostUptimeMillis();
  ScopedStageWatchdog read_image_watchdog(
      "XexModule::Load waiting in ReadImage", path_, [this]() {
        if (kernel_state_) {
          kernel_state_->NoteUserModuleLoadProgress();
        }
      });

  int result_code = ReadImage(xex_addr, xex_length, xe_xex2_retail_key);
  if (result_code) {
    XELOGW("XEX load failed with code {}, trying with devkit encryption key...",
           result_code);

    result_code = ReadImage(xex_addr, xex_length, xe_xex2_devkit_key);
    if (result_code) {
      XELOGE("XEX load failed with code {}, trying with xex1 encryption key...",
             result_code);

      result_code = ReadImage(xex_addr, xex_length, xe_xex1_retail_key);
      if (result_code) {
        XELOGE("XEX load failed with code {}", result_code);
        return false;
      }
    }
  }

  XELOGI(
      "[DEBUG] XexModule::Load ReadImage returned; stopping read-image watchdog");
  read_image_watchdog.Stop();
  XELOGI("[DEBUG] XexModule::Load read-image watchdog stopped");
  if (kernel_state_) {
    kernel_state_->NoteUserModuleLoadProgress();
    XELOGI("[DEBUG] XexModule::Load posted module-load heartbeat after ReadImage");
  }

  XELOGI("[DEBUG] XexModule::Load ReadImage completed in {} ms result={}",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                         t_read_image_begin),
         result_code);

  // Note: caller will have to call LoadContinue once it's determined whether a
  // patch file exists or not!
  XELOGI("[DEBUG] XexModule::Load end total={} ms", static_cast<unsigned long long>(
                                                 Clock::QueryHostUptimeMillis() -
                                                 t0));
  return true;
}

bool XexModule::LoadContinue() {
  uint64_t t0 = Clock::QueryHostUptimeMillis();
  XELOGI("[DEBUG] XexModule::LoadContinue begin path='{}'", path_);

  std::atomic<const char*> load_continue_stage{"init"};
  std::atomic<uint32_t> import_library_index{0};
  std::atomic<uint32_t> import_offset_bytes{0};
  std::atomic<uint32_t> import_total_bytes{0};
  ScopedStageWatchdog load_continue_watchdog(
      "XexModule::LoadContinue waiting in post-ReadImage stages", path_,
      [this, &load_continue_stage, &import_library_index, &import_offset_bytes,
       &import_total_bytes]() {
        if (kernel_state_) {
          kernel_state_->NoteUserModuleLoadProgress();
        }
        XELOGW(
            "[DEBUG] XexModule::LoadContinue heartbeat: stage='{}' "
            "import_library_index={} import_offset={}/{}",
            load_continue_stage.load(std::memory_order_relaxed),
            import_library_index.load(std::memory_order_relaxed),
            import_offset_bytes.load(std::memory_order_relaxed),
            import_total_bytes.load(std::memory_order_relaxed));
      });

  // Second part of image load
  // Split from Load() so that we can patch the XEX before loading this data
  assert_false(finished_load_);
  if (finished_load_) {
    return true;
  }

  finished_load_ = true;

  load_continue_stage.store("ReadPEHeaders", std::memory_order_relaxed);
  uint64_t t_pe_begin = Clock::QueryHostUptimeMillis();
  ScopedStageWatchdog read_pe_watchdog(
      "XexModule::LoadContinue waiting in ReadPEHeaders", path_,
      [this]() {
        if (kernel_state_) {
          kernel_state_->NoteUserModuleLoadProgress();
        }
      });
  if (ReadPEHeaders()) {
    XELOGE("Failed to load XEX PE headers!");
    return false;
  }
  read_pe_watchdog.Stop();
  XELOGI(
      "[DEBUG] XexModule::LoadContinue ReadPEHeaders completed in {} ms",
      static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                      t_pe_begin));

  // Parse any "unsafe" headers into safer variants
  xex2_opt_generic_u32* alternate_titleids;
  if (GetOptHeader(xex2_header_keys::XEX_HEADER_ALTERNATE_TITLE_IDS,
                   &alternate_titleids)) {
    auto count = alternate_titleids->count();
    for (uint32_t i = 0; i < count; i++) {
      opt_alternate_title_ids_.push_back(alternate_titleids->values[i]);
    }
  }

  // Scan and find the low/high addresses.
  // All code sections are continuous, so this should be easy.
  // could use a source for the above information
  load_continue_stage.store("ScanCodeRanges", std::memory_order_relaxed);
  auto heap = memory()->LookupHeap(base_address_);
  auto page_size = heap->page_size();

  low_address_ = UINT_MAX;
  high_address_ = 0;

  auto sec_header = xex_security_info();
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);

    const auto start_address = base_address_ + (page * page_size);
    const auto end_address = start_address + (desc.page_count * page_size);
    if (desc.info == XEX_SECTION_CODE) {
      low_address_ = std::min(low_address_, start_address);
      high_address_ = std::max(high_address_, end_address);
    }

    page += desc.page_count;
  }

  // Notify backend that we have an executable range.
  load_continue_stage.store("CommitExecutableRange",
                            std::memory_order_relaxed);
  processor_->backend()->CommitExecutableRange(low_address_, high_address_);

  // Add all imports (variables/functions).
  xex2_opt_import_libraries* opt_import_libraries = nullptr;
  GetOptHeader(XEX_HEADER_IMPORT_LIBRARIES, &opt_import_libraries);

  uint64_t t_imports_begin = Clock::QueryHostUptimeMillis();
  if (opt_import_libraries) {
    // FIXME: Don't know if 32 is the actual limit, but haven't seen more than
    // 2.
    const char* string_table[32];
    std::memset(string_table, 0, sizeof(string_table));
    const size_t string_table_size = opt_import_libraries->string_table.size;
    const size_t string_table_count = opt_import_libraries->string_table.count;

    // Parse the string table
    for (size_t i = 0, o = 0; i < string_table_size && o < string_table_count;
         ++o) {
      if (o >= xe::countof(string_table)) {
        XELOGE("XEX import string table count {} exceeds supported limit {}.",
               static_cast<unsigned long long>(string_table_count),
               static_cast<unsigned long long>(xe::countof(string_table)));
        return false;
      }
      const char* str = &opt_import_libraries->string_table.data[i];
      const size_t remaining_bytes = string_table_size - i;
      const void* terminator = std::memchr(str, '\0', remaining_bytes);
      if (!terminator) {
        XELOGE(
            "XEX import string table entry at offset {} is not null-terminated "
            "within {} bytes.",
            static_cast<unsigned long long>(i),
            static_cast<unsigned long long>(remaining_bytes));
        return false;
      }

      string_table[o] = str;
      i += static_cast<const char*>(terminator) - str + 1;

      // Padding
      if ((i % 4) != 0) {
        i += 4 - (i % 4);
      }
    }

    auto library_data = reinterpret_cast<uint8_t*>(opt_import_libraries);
    uint32_t library_offset = opt_import_libraries->string_table.size + 12;
    import_total_bytes.store(opt_import_libraries->size,
                             std::memory_order_relaxed);
    load_continue_stage.store("SetupLibraryImports", std::memory_order_relaxed);
    while (library_offset < opt_import_libraries->size) {
      import_offset_bytes.store(library_offset, std::memory_order_relaxed);
      auto library =
          reinterpret_cast<xex2_import_library*>(library_data + library_offset);
      if (!library->size) {
        break;
      }
      import_library_index.fetch_add(1, std::memory_order_relaxed);
      size_t library_name_index = library->name_index & 0xFF;
      assert_true(library_name_index <
                  opt_import_libraries->string_table.count);
      assert_not_null(string_table[library_name_index]);
      auto library_name = std::string(string_table[library_name_index]);

      if (!kernel_state_->IsModuleLoaded(library_name)) {
        load_continue_stage.store("LoadDependencyModule",
                                  std::memory_order_relaxed);
        if (auto module = kernel_state_->LoadUserModule(library_name)) {
          if (kernel_state_->FinishLoadingUserModule(module, false)) {
            library_name = module->path();
          }
        }
        load_continue_stage.store("SetupLibraryImports",
                                  std::memory_order_relaxed);
      }

      SetupLibraryImports(library_name, library);
      library_offset += library->size;
    }
  }
  XELOGI("[DEBUG] XexModule::LoadContinue import setup took {} ms",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                         t_imports_begin));

  // Load a specified module map and diff.
  if (cvars::load_module_map.size()) {
    if (!ReadMap(cvars::load_module_map.c_str())) {
      return false;
    }
  }

  // Disable write protection if plugins are enabled
  if (cvars::allow_plugins && !cvars::writable_code_segments) {
    OVERRIDE_bool(writable_code_segments, true);
  }

  // Setup memory protection.
  load_continue_stage.store("ProtectImagePages", std::memory_order_relaxed);
  uint64_t t_protect_begin = Clock::QueryHostUptimeMillis();
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);

    auto address = base_address_ + (page * page_size);
    auto size = desc.page_count * page_size;
    switch (desc.info) {
      case XEX_SECTION_CODE:
      case XEX_SECTION_READONLY_DATA:
        heap->Protect(address, size,
                      cvars::writable_code_segments
                          ? kMemoryProtectRead | kMemoryProtectWrite
                          : kMemoryProtectRead);
        break;
      case XEX_SECTION_DATA:
        heap->Protect(address, size, kMemoryProtectRead | kMemoryProtectWrite);
        break;
    }

    page += desc.page_count;
  }
  XELOGI("[DEBUG] XexModule::LoadContinue page protection took {} ms",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() -
                                         t_protect_begin));

  XELOGI("[DEBUG] XexModule::LoadContinue end total={} ms",
         static_cast<unsigned long long>(Clock::QueryHostUptimeMillis() - t0));
  load_continue_stage.store("done", std::memory_order_relaxed);
  return true;
}

void XexModule::Precompile() {
  sha1::SHA1 final_image_sha_;

  final_image_sha_.reset();

  unsigned high_code = this->high_address_ - this->low_address_;

  final_image_sha_.processBytes(memory()->TranslateVirtual(this->low_address_),
                                high_code);
  final_image_sha_.finalize(image_sha_bytes_);

  image_sha_str_.clear();
  for (unsigned i = 0; i < sizeof(image_sha_bytes_); ++i) {
    image_sha_str_ += fmt::format("{:02X}", image_sha_bytes_[i]);
  }

  if (image_sha_str_.size() != sizeof(image_sha_bytes_) * 2) {
    XELOGE("XEX hash is the wrong length!");
  }

  // Find __savegprlr_* and __restgprlr_* and the others.
  // We can flag these for special handling (inlining/etc).
  if (!FindSaveRest()) {
    return;
  }

  info_cache_.Init(this);
  PrecompileDiscoveredFunctions();
}
bool XexModule::Unload() {
  if (!loaded_) {
    return true;
  }
  loaded_ = false;

  // If this isn't a patch, just deallocate the memory occupied by the exe
  if (!is_patch()) {
    assert_not_zero(base_address_);

    memory()->LookupHeap(base_address_)->Release(base_address_);
  }

  xex_header_mem_.resize(0);

  return true;
}

bool XexModule::SetupLibraryImports(const std::string_view name,
                                    const xex2_import_library* library) {
  ExportResolver* kernel_resolver = nullptr;
  auto base_name = utf8::find_base_name_from_guest_path(name);
  if (kernel_state_->IsKernelModule(base_name)) {
    kernel_resolver = processor_->export_resolver();
  }

  kernel::object_ref<kernel::XModule> kernel_module;
  if (kernel_resolver) {
    kernel_module = kernel_state_->GetModule(base_name, false);
  }

  auto user_module = kernel_state_->GetModule(name);

  const bool log_gpu_imports =
      (cvars::xex_log_imports_gpu || cvars::trace_gpu_bootstrap) &&
      (base_name == "xboxkrnl");
  uint32_t gpu_imports_seen = 0;
  uint32_t gpu_imports_resolved = 0;
  auto is_gpu_import_ordinal = [](uint16_t ordinal) {
    switch (ordinal) {
      case 0x01B6:  // VdEnableRingBufferRPtrWriteBack
      case 0x01BC:  // VdGetGraphicsAsicID
      case 0x01BE:  // VdGlobalDevice
      case 0x01BF:  // VdGlobalXamDevice
      case 0x01C2:  // VdInitializeEngines
      case 0x01BD:  // VdGetSystemCommandBuffer
      case 0x01C3:  // VdInitializeRingBuffer
      case 0x01C6:  // VdIsHSIOTrainingSucceeded
      case 0x01D5:  // VdSetGraphicsInterruptCallback
      case 0x025B:  // VdSwap
      case 0x0268:  // VdInitializeEDRAM
      case 0x0269:  // VdRetrainEDRAM
      case 0x026A:  // VdRetrainEDRAMWorker
        return true;
      default:
        return false;
    }
  };

  ImportLibrary library_info;
  library_info.name = base_name;
  library_info.id = library->id;
  library_info.version.value = library->version().value;
  library_info.min_version.value = library->version_min().value;
  library_info.import_record_count = library->count;

  // Imports are stored as {import descriptor, thunk addr, import desc, ...}
  // Even thunks have an import descriptor (albeit unused/useless)
  for (uint32_t i = 0; i < library->count; i++) {
    uint32_t record_addr = library->import_table[i];
    assert_not_zero(record_addr);

    auto record_slot =
        memory()->TranslateVirtual<xe::be<uint32_t>*>(record_addr);
    uint32_t record_value = *record_slot;

    uint16_t record_type = (record_value & 0xFF000000) >> 24;
    uint16_t ordinal = record_value & 0xFFFF;
    if (record_type == 0) {
      library_info.variable_record_count++;
    } else if (record_type == 1) {
      library_info.thunk_record_count++;
    } else {
      library_info.unknown_record_count++;
    }

    Export* kernel_export = nullptr;
    uint32_t user_export_addr = 0;

    if (kernel_resolver) {
      // Use normalized module names to avoid misses from path / extension
      // variants in import descriptors.
      kernel_export = kernel_resolver->GetExportByOrdinal(base_name, ordinal);
    } else if (user_module) {
      user_export_addr = user_module->GetProcAddressByOrdinal(ordinal);
    }

    bool is_gpu_import = log_gpu_imports && is_gpu_import_ordinal(ordinal);
    if (is_gpu_import) {
      gpu_imports_seen++;
      if (kernel_export || user_export_addr) {
        gpu_imports_resolved++;
      }
      XELOGI(
          "[DEBUG] XEX static import record (module-load, {}): ordinal=0x{:03X} "
          "type={} record=0x{:08X} kernel_export={} resolved={} "
          "user_export=0x{:08X}",
          base_name, ordinal, record_type, record_addr,
          kernel_export ? kernel_export->name : "<null>",
          (kernel_export || user_export_addr) ? "YES" : "NO", user_export_addr);
      XELOGI("[DEBUG] GPU static import raw record (module-load)=0x{:08X} "
             "slot_ptr=0x{:08X}",
             record_value, record_addr);
      XELOGI("[DEBUG] GPU static import export binding (module-load) "
             "export_ptr={} export_type={} implemented={}",
             kernel_export ? kernel_export->name : "<null>",
             kernel_export ? static_cast<int>(kernel_export->get_type()) : -1,
             kernel_export ? (kernel_export->is_implemented() ? "YES" : "NO")
                           : "NO");
    }

    // Import not resolved?
    if (!kernel_export && !user_export_addr) {
      XELOGW(
          "WARNING: an import variable was not resolved! (library: {}, import "
          "lib: {}, ordinal: {:03X})",
          name_, name, ordinal);
    }

    StringBuffer import_name;
    if (record_type == 0) {
      // Variable.

      ImportLibraryFn import_info;
      import_info.ordinal = ordinal;
      import_info.value_address = record_addr;
      library_info.imports.push_back(import_info);

      import_name.Append("__imp__");
      if (kernel_export) {
        import_name.Append(kernel_export->name);
      } else {
        import_name.AppendFormat("{}_{:03X}", base_name, ordinal);
      }

      if (kernel_export) {
        if (kernel_export->get_type() == Export::Type::kFunction) {
          // Some titles read the variable slot as a function pointer.
          // Prefer patching with a direct trampoline immediately so missing or
          // malformed thunk records don't leave this slot unresolved.
          uint32_t fallback_function_ptr = 0;
          if (kernel_module) {
            fallback_function_ptr =
                kernel_module->GetProcAddressByOrdinal(ordinal);
          }
          *record_slot = fallback_function_ptr;
          if (is_gpu_import) {
            XELOGI(
                "[DEBUG] GPU import variable for function: slot=0x{:08X} "
                "fallback_ptr=0x{:08X} (thunk may override)",
                record_addr, fallback_function_ptr);
          }
          if (!fallback_function_ptr) {
            XELOGW(
                "WARNING: function import variable has no fallback trampoline "
                "(library={}, ordinal=0x{:03X}, slot=0x{:08X})",
                base_name, ordinal, record_addr);
          }
        } else if (kernel_export->get_type() == Export::Type::kVariable) {
          // Kernel import variable
          if (kernel_export->is_implemented()) {
            // Implemented - replace with pointer.
            *record_slot = kernel_export->variable_ptr;
          } else {
            // Not implemented - write with a dummy value.
            *record_slot = 0xD000BEEF | (kernel_export->ordinal & 0xFFF) << 16;
            XELOGCPU("WARNING: imported a variable with no value: {}",
                     kernel_export->name);
          }
        }
      } else if (user_export_addr) {
        *record_slot = user_export_addr;
      } else {
        *record_slot = 0xF00DF00D;
      }
      if (is_gpu_import) {
        uint32_t patched_value = xe::load_and_swap<uint32_t>(record_slot);
        XELOGI("[DEBUG] GPU static import variable patched (module-load): "
               "slot=0x{:08X} value=0x{:08X}",
               record_addr, patched_value);
        if (kernel_export &&
            kernel_export->get_type() == Export::Type::kVariable &&
            (ordinal == 0x01BE || ordinal == 0x01BF)) {
          uint32_t export_value = 0;
          auto* export_host =
              memory()->TranslateVirtual<xe::be<uint32_t>*>(patched_value);
          if (export_host) {
            export_value = xe::load_and_swap<uint32_t>(export_host);
          }
          XELOGI(
              "[DEBUG] GPU import variable value: ordinal=0x{:03X} "
              "value_addr=0x{:08X} current_value=0x{:08X} mapped={}",
              ordinal, patched_value, export_value,
              export_host ? "YES" : "NO");
        }
      }

      // Setup a variable and define it.
      Symbol* var_info;
      DeclareVariable(record_addr, &var_info);
      var_info->set_name(import_name.to_string_view());
      var_info->set_status(Symbol::Status::kDeclared);
      DefineVariable(var_info);
      var_info->set_status(Symbol::Status::kDefined);
    } else if (record_type == 1) {
      // Thunk.
      if (library_info.imports.size() > 0) {
        auto& prev_import =
            library_info.imports[library_info.imports.size() - 1];
        assert_true(prev_import.ordinal == ordinal);
        prev_import.thunk_address = record_addr;
        // If the preceding import record was a variable for a function,
        // patch it to point at the thunk so indirect calls work.
        if (kernel_export &&
            kernel_export->get_type() == Export::Type::kFunction &&
            prev_import.value_address) {
          auto variable_slot = memory()->TranslateVirtual<xe::be<uint32_t>*>(
              prev_import.value_address);
          *variable_slot = record_addr;
          if (is_gpu_import) {
            XELOGI(
                "[DEBUG] GPU static import variable patched to thunk "
                "(module-load): slot=0x{:08X} thunk=0x{:08X}",
                prev_import.value_address, record_addr);
          }
        }
      }

      if (kernel_export) {
        import_name.Append(kernel_export->name);
      } else {
        import_name.AppendFormat("__{}_{:03X}", base_name, ordinal);
      }

      Function* function;
      DeclareFunction(record_addr, &function);
      function->set_end_address(record_addr + 16 - 4);
      function->set_name(import_name.to_string_view());
      if (is_gpu_import) {
        XELOGI(
            "[DEBUG] GPU static import thunk declared (module-load): "
            "name={} addr=0x{:08X} kernel_export={}",
            import_name.to_string_view(), record_addr,
            kernel_export ? kernel_export->name : "<null>");
      }

      if (user_export_addr) {
        // Rewrite PPC code to set r11 to the target address
        // So we'll have:
        //    lis r11, user_export_addr
        //    ori r11, r11, user_export_addr
        //    mtspr CTR, r11
        //    bctr
        uint16_t hi_addr = (user_export_addr >> 16) & 0xFFFF;
        uint16_t low_addr = user_export_addr & 0xFFFF;

        uint8_t* p = memory()->TranslateVirtual(record_addr);
        xe::store_and_swap<uint32_t>(p + 0x0, 0x3D600000 | hi_addr);
        xe::store_and_swap<uint32_t>(p + 0x4, 0x616B0000 | low_addr);
      } else {
        // On load we have something like this in memory:
        //     li r3, 0
        //     li r4, 0x1F5
        //     mtspr CTR, r11
        //     bctr
        // Real consoles rewrite this with some code that sets r11.
        // If we did that we'd still have to put a thunk somewhere and do the
        // dynamic lookup. Instead, we rewrite it to use syscalls.
        // We use sc with a LEV operand of 2, which is reserved usage and
        // should never see actual usage outside of our rewrite.
        // CPU backends can either take the special form syscall or do
        // something smarter.
        //     sc 2
        //     blr
        //     nop
        //     nop
        uint8_t* p = memory()->TranslateVirtual(record_addr);
        xe::store_and_swap<uint32_t>(p + 0x0, 0x44000042);
        xe::store_and_swap<uint32_t>(p + 0x4, 0x4E800020);
        xe::store_and_swap<uint32_t>(p + 0x8, 0x60000000);
        xe::store_and_swap<uint32_t>(p + 0xC, 0x60000000);

        // Note that we may not have a handler registered - if not, eventually
        // we'll get directed to UndefinedImport.
        GuestFunction::ExternHandler handler = nullptr;
        if (kernel_export) {
          if (kernel_export->function_data.trampoline) {
            handler = (GuestFunction::ExternHandler)
                          kernel_export->function_data.trampoline;
          } else {
            //__debugbreak();
            // handler =
            //     (GuestFunction::ExternHandler)kernel_export->function_data.shim;
          }
        } else {
          XELOGW("WARNING: Imported kernel function {} is unimplemented!",
                 import_name.to_string_view());
        }
        static_cast<GuestFunction*>(function)->SetupExtern(handler,
                                                           kernel_export);
      }
      function->set_status(Symbol::Status::kDeclared);
    } else {
      // Bad.
      assert_always();
    }
  }

  if (log_gpu_imports) {
    auto find_static_import = [&](uint16_t ordinal) -> const ImportLibraryFn* {
      for (const auto& import_fn : library_info.imports) {
        if (import_fn.ordinal == ordinal) {
          return &import_fn;
        }
      }
      return nullptr;
    };
    auto log_static_import_surface = [&](uint16_t ordinal,
                                         const char* fallback_name) {
      const auto* import_fn = find_static_import(ordinal);
      auto* export_entry =
          kernel_resolver ? kernel_resolver->GetExportByOrdinal(base_name, ordinal)
                          : nullptr;
      XELOGI(
          "[DEBUG] XEX static import surface (module-load): library={} "
          "ordinal=0x{:03X} name={} value_addr=0x{:08X} thunk_addr=0x{:08X} "
          "resolved={} runtime=not_observed",
          base_name, ordinal,
          export_entry && export_entry->name ? export_entry->name : fallback_name,
          import_fn ? import_fn->value_address : 0,
          import_fn ? import_fn->thunk_address : 0,
          (import_fn && export_entry) ? "YES" : "NO");
    };
    auto has_static_import_surface = [&](uint16_t ordinal) -> bool {
      const auto* import_fn = find_static_import(ordinal);
      return import_fn && import_fn->value_address != 0 &&
             import_fn->thunk_address != 0;
    };
    log_static_import_surface(0x0195, "XexGetModuleHandle");
    log_static_import_surface(0x0197, "XexGetProcedureAddress");
    log_static_import_surface(0x0194, "XexCheckExecutablePrivilege");
    log_static_import_surface(0x01C3, "VdInitializeRingBuffer");
    log_static_import_surface(0x01D5, "VdSetGraphicsInterruptCallback");
    log_static_import_surface(0x025B, "VdSwap");
    XELOGI(
        "[DEBUG] XEX static import readiness detail (module-load, {}): "
        "critical_gpu_imports_ready={} loader_imports_ready={} "
        "xexcheck_import_ready={} "
        "gpu_imports_seen={} gpu_imports_resolved={}",
        base_name,
        (has_static_import_surface(0x01C3) && has_static_import_surface(0x01D5) &&
         has_static_import_surface(0x025B))
            ? "YES"
            : "NO",
        (has_static_import_surface(0x0195) && has_static_import_surface(0x0197))
            ? "YES"
            : "NO",
        has_static_import_surface(0x0194) ? "YES" : "NO",
        gpu_imports_seen, gpu_imports_resolved);
  }
  if (library_info.unknown_record_count != 0) {
    XELOGW(
        "XEX import record anomaly ({}): records={} variables={} thunks={} "
        "unknown={}",
        base_name, library_info.import_record_count,
        library_info.variable_record_count, library_info.thunk_record_count,
        library_info.unknown_record_count);
  }

  import_libs_.push_back(library_info);

  return true;
}

bool XexModule::ContainsAddress(uint32_t address) {
  return address >= low_address_ && address < high_address_;
}

std::unique_ptr<Function> XexModule::CreateFunction(uint32_t address) {
  return std::unique_ptr<Function>(
      processor_->backend()->CreateGuestFunction(this, address));
}
void XexInfoCache::Init(XexModule* xexmod) {
  if (cvars::disable_instruction_infocache) {
    return;
  }

  auto emu = xexmod->kernel_state_->emulator();
  std::filesystem::path infocache_path = emu->cache_root();

  infocache_path.append(L"modules");

  infocache_path.append(xexmod->image_sha_str_);

  std::filesystem::create_directories(infocache_path);
  infocache_path.append("executable_addr_flags.bin");

  unsigned num_codebytes = xexmod->high_address_ - xexmod->low_address_;
  num_codebytes += 3;  // round up to nearest multiple of 4
  num_codebytes &= ~3;

  auto try_open = [this, &infocache_path, num_codebytes]() {
    bool did_exist = true;

    if (!std::filesystem::exists(infocache_path)) {
      xe::filesystem::CreateEmptyFile(infocache_path);
      did_exist = false;
    }

    // todo: prepopulate with stuff from pdata, dll exports

    this->executable_addr_flags_ = std::move(xe::MappedMemory::Open(
        infocache_path, xe::MappedMemory::Mode::kReadWrite, 0,
        sizeof(InfoCacheFlagsHeader) +
            (sizeof(InfoCacheFlags) *
             (num_codebytes /
              4))));  // one infocacheflags entry for each PPC instr-sized addr
    return did_exist;
  };

  bool did_exist = try_open();
  if (!GetHeader()) {
    return;
  }

  if (!did_exist) {
    GetHeader()->version = CURRENT_INFOCACHE_VERSION;

  } else {
    if (GetHeader()->version != CURRENT_INFOCACHE_VERSION) {
      this->executable_addr_flags_->Close();
      std::filesystem::remove(infocache_path);
      try_open();
    }
  }
}
InfoCacheFlags* XexModule::GetInstructionAddressFlags(uint32_t guest_addr) {
  if (guest_addr < low_address_ || guest_addr > high_address_) {
    return nullptr;
  }

  guest_addr -= low_address_;

  return info_cache_.LookupFlags(guest_addr);
}
void XexModule::PrecompileDiscoveredFunctions() {
  if (!cvars::enable_early_precompilation) {
    return;
  }
  auto others = PreanalyzeCode();

  for (auto&& other : others) {
    if (other < low_address_ || other >= high_address_) {
      continue;
    }
    auto sym = processor_->LookupFunction(other);

    if (!sym || sym->status() != Symbol::Status::kDefined) {
      processor_->ResolveFunction(other);
    }
  }
}
void XexModule::PrecompileKnownFunctions() {
  if (!cvars::enable_early_precompilation) {
    return;
  }

  //uint32_t start = 0;
#ifdef __APPLE__
  (void)start;
#endif

  uint32_t end = (high_address_ - low_address_) / 4;
  auto flags = info_cache_.LookupFlags(0);
  if (!flags) {
    return;
  }
  // maybe should pre-acquire global crit?
  for (uint32_t i = 0; i < end; i++) {
    if (flags[i].was_resolved) {
      uint32_t addr = low_address_ + (i * 4);
      auto sym = processor_->LookupFunction(addr);

      if (!sym || sym->status() != Symbol::Status::kDefined) {
        processor_->ResolveFunction(addr);
      }
    }
  }
}

static uint32_t GetBLCalledFunction(XexModule* xexmod, uint32_t current_base,
                                    ppc::PPCOpcodeBits wrd) {
  int32_t displ = static_cast<int32_t>(ppc::XEEXTS26(wrd.I.LI << 2));

  if (wrd.I.AA) {
    return static_cast<uint32_t>(displ);
  } else {
    return static_cast<uint32_t>(static_cast<int32_t>(current_base) + displ);
  }
}
static bool IsOpcodeBL(unsigned w) {
  return (w >> (32 - 6)) == 18 && ppc::PPCOpcodeBits{w}.I.LK;
}

std::vector<uint32_t> XexModule::PreanalyzeCode() {
  uint32_t low_8_aligned = xe::align<uint32_t>(low_address_, 8);

  uint32_t highest_exec_addr = 0;

  for (auto&& sec : pe_sections_) {
    if ((sec.flags & kXEPESectionContainsCode)) {
      highest_exec_addr =
          std::max<uint32_t>(highest_exec_addr, sec.address + sec.size);
    }
  }
  uint32_t high_8_aligned = highest_exec_addr & ~(8U - 1);
  uint32_t n_possible_8byte_addresses = (high_8_aligned - low_8_aligned) / 8;
  uint32_t* funcstart_candidate_stack =
      new uint32_t[n_possible_8byte_addresses];
  uint32_t* funcstart_candstack2 = new uint32_t[n_possible_8byte_addresses];

  uint32_t stack_pos = 0;
  {
    // all functions seem to start on 8 byte boundaries, except for obvious ones
    // like the save/rest funcs
    uint32_t* range_start =
        memory()->TranslateVirtual<uint32_t*>(low_8_aligned);
    uint32_t* range_end = memory()->TranslateVirtual<uint32_t*>(
        high_8_aligned);  // align down to multiple of 8

    constexpr uint8_t mfspr_r12_lr[4] = {0x7D, 0x88, 0x02, 0xA6};

    // a blr instruction, with 4 zero bytes afterwards to pad the next address
    // to 8 byte alignment
    // if we see this prior to our address, we can assume we are a function
    // start
    constexpr uint8_t blr[4] = {0x4E, 0x80, 0x0, 0x20};

    uint32_t blr32 = *reinterpret_cast<const uint32_t*>(&blr[0]);

    uint32_t mfspr_r12_lr32 =
        *reinterpret_cast<const uint32_t*>(&mfspr_r12_lr[0]);

    auto add_new_func = [funcstart_candidate_stack, &stack_pos](uint32_t addr) {
      funcstart_candidate_stack[stack_pos++] = addr;
    };
    /*
                First pass: detect save of the link register at an eight byte
       aligned address
        */
    for (uint32_t* first_pass = range_start; first_pass < range_end;
         first_pass += 2) {
      if (*first_pass == mfspr_r12_lr32) {
        // Push our newly discovered function start into our list
        // All addresses in the list are sorted until the second pass
        add_new_func(
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first_pass) -
                                  reinterpret_cast<uintptr_t>(range_start)) +
            low_8_aligned);
      } else if (first_pass[-1] == 0 && *first_pass != 0) {
        // originally i checked for blr followed by 0, but some functions are
        // actually aligned to greater boundaries. something that appears to be
        // longjmp (it occurs in most games, so standard library, and loads ctx,
        // so longjmp) is aligned to 16 bytes in most games
        uint32_t* check_iter = &first_pass[-2];

        while (!*check_iter) {
          --check_iter;
        }

        XE_LIKELY_IF(*check_iter == blr32) {
          add_new_func(
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(first_pass) -
                                    reinterpret_cast<uintptr_t>(range_start)) +
              low_8_aligned);
        }
      }
    }
    uint32_t current_guestaddr = low_8_aligned;
    // Second pass: detect branch with link instructions and decode the target
    // address. We can safely assume that if bl is to address, that address is
    // the start of the function
    for (uint32_t* second_pass = range_start; second_pass < range_end;
         second_pass++, current_guestaddr += 4) {
      uint32_t current_call = xe::byte_swap(*second_pass);

      if (IsOpcodeBL(current_call)) {
        uint32_t called_function = GetBLCalledFunction(
            this, current_guestaddr, ppc::PPCOpcodeBits{current_call});
        // must be 8 byte aligned and in range
        if ((called_function & (8 - 1)) == 0 &&
            called_function >= low_address_ &&
            called_function < high_address_) {
          add_new_func(called_function);
        }
      }
    }

    auto pdata = this->GetPESection(".pdata");

    if (pdata) {
      uint32_t* pdata_base =
          this->memory()->TranslateVirtual<uint32_t*>(pdata->address);

      uint32_t n_pdata_entries = pdata->raw_size / 8;

      for (uint32_t i = 0; i < n_pdata_entries; ++i) {
        uint32_t funcaddr = xe::load_and_swap<uint32_t>(&pdata_base[i * 2]);
        if (funcaddr >= low_address_ && funcaddr <= highest_exec_addr) {
          add_new_func(funcaddr);
        } else {
          // we hit 0 for func addr, that means we're done
          break;
        }
      }
    }
  }

  // Sort the list of function starts and then ensure that all addresses are
  // unique
  uint32_t n_known_funcaddrs = 0;
  {
    // make addresses unique

    std::sort(funcstart_candidate_stack, funcstart_candidate_stack + stack_pos);

    uint32_t read_pos = 0;
    uint32_t write_pos = 0;
    uint32_t previous_addr = ~0u;
    while (read_pos < stack_pos) {
      uint32_t current_addr = funcstart_candidate_stack[read_pos++];

      if (current_addr != previous_addr) {
        previous_addr = current_addr;
        funcstart_candstack2[write_pos++] = current_addr;
      }
    }
    n_known_funcaddrs = write_pos;
  }

  delete[] funcstart_candidate_stack;

  std::vector<uint32_t> result;
  result.resize(n_known_funcaddrs);
  memcpy(&result[0], funcstart_candstack2,
         sizeof(uint32_t) * n_known_funcaddrs);
  delete[] funcstart_candstack2;
  return result;
}
bool XexModule::FindSaveRest() {
  // Special stack save/restore functions.
  // http://research.microsoft.com/en-us/um/redmond/projects/invisible/src/crt/md/ppc/xxx.s.htm
  // It'd be nice to stash these away and mark them as such to allow for
  // special codegen.
  // __savegprlr_14 to __savegprlr_31
  // __restgprlr_14 to __restgprlr_31
  static constexpr uint32_t gprlr_code_values[] = {
      0x68FFC1F9,  // __savegprlr_14
      0x70FFE1F9,  // __savegprlr_15
      0x78FF01FA,  // __savegprlr_16
      0x80FF21FA,  // __savegprlr_17
      0x88FF41FA,  // __savegprlr_18
      0x90FF61FA,  // __savegprlr_19
      0x98FF81FA,  // __savegprlr_20
      0xA0FFA1FA,  // __savegprlr_21
      0xA8FFC1FA,  // __savegprlr_22
      0xB0FFE1FA,  // __savegprlr_23
      0xB8FF01FB,  // __savegprlr_24
      0xC0FF21FB,  // __savegprlr_25
      0xC8FF41FB,  // __savegprlr_26
      0xD0FF61FB,  // __savegprlr_27
      0xD8FF81FB,  // __savegprlr_28
      0xE0FFA1FB,  // __savegprlr_29
      0xE8FFC1FB,  // __savegprlr_30
      0xF0FFE1FB,  // __savegprlr_31
      0xF8FF8191, 0x2000804E,
      0x68FFC1E9,  // __restgprlr_14
      0x70FFE1E9,  // __restgprlr_15
      0x78FF01EA,  // __restgprlr_16
      0x80FF21EA,  // __restgprlr_17
      0x88FF41EA,  // __restgprlr_18
      0x90FF61EA,  // __restgprlr_19
      0x98FF81EA,  // __restgprlr_20
      0xA0FFA1EA,  // __restgprlr_21
      0xA8FFC1EA,  // __restgprlr_22
      0xB0FFE1EA,  // __restgprlr_23
      0xB8FF01EB,  // __restgprlr_24
      0xC0FF21EB,  // __restgprlr_25
      0xC8FF41EB,  // __restgprlr_26
      0xD0FF61EB,  // __restgprlr_27
      0xD8FF81EB,  // __restgprlr_28
      0xE0FFA1EB,  // __restgprlr_29
      0xE8FFC1EB,  // __restgprlr_30
      0xF0FFE1EB,  // __restgprlr_31
      0xF8FF8181, 0xA603887D, 0x2000804E,
  };
  // __savefpr_14 to __savefpr_31
  // __restfpr_14 to __restfpr_31
  static constexpr uint32_t fpr_code_values[] = {
      0x70FFCCD9,  // __savefpr_14
      0x78FFECD9,  // __savefpr_15
      0x80FF0CDA,  // __savefpr_16
      0x88FF2CDA,  // __savefpr_17
      0x90FF4CDA,  // __savefpr_18
      0x98FF6CDA,  // __savefpr_19
      0xA0FF8CDA,  // __savefpr_20
      0xA8FFACDA,  // __savefpr_21
      0xB0FFCCDA,  // __savefpr_22
      0xB8FFECDA,  // __savefpr_23
      0xC0FF0CDB,  // __savefpr_24
      0xC8FF2CDB,  // __savefpr_25
      0xD0FF4CDB,  // __savefpr_26
      0xD8FF6CDB,  // __savefpr_27
      0xE0FF8CDB,  // __savefpr_28
      0xE8FFACDB,  // __savefpr_29
      0xF0FFCCDB,  // __savefpr_30
      0xF8FFECDB,  // __savefpr_31
      0x2000804E,
      0x70FFCCC9,  // __restfpr_14
      0x78FFECC9,  // __restfpr_15
      0x80FF0CCA,  // __restfpr_16
      0x88FF2CCA,  // __restfpr_17
      0x90FF4CCA,  // __restfpr_18
      0x98FF6CCA,  // __restfpr_19
      0xA0FF8CCA,  // __restfpr_20
      0xA8FFACCA,  // __restfpr_21
      0xB0FFCCCA,  // __restfpr_22
      0xB8FFECCA,  // __restfpr_23
      0xC0FF0CCB,  // __restfpr_24
      0xC8FF2CCB,  // __restfpr_25
      0xD0FF4CCB,  // __restfpr_26
      0xD8FF6CCB,  // __restfpr_27
      0xE0FF8CCB,  // __restfpr_28
      0xE8FFACCB,  // __restfpr_29
      0xF0FFCCCB,  // __restfpr_30
      0xF8FFECCB,  // __restfpr_31
      0x2000804E,
  };
  // __savevmx_14 to __savevmx_31
  // __savevmx_64 to __savevmx_127
  // __restvmx_14 to __restvmx_31
  // __restvmx_64 to __restvmx_127
  static constexpr uint32_t vmx_code_values[] = {
      0xE0FE6039,  // __savevmx_14
      0xCE61CB7D, 0xF0FE6039, 0xCE61EB7D, 0x00FF6039, 0xCE610B7E, 0x10FF6039,
      0xCE612B7E, 0x20FF6039, 0xCE614B7E, 0x30FF6039, 0xCE616B7E, 0x40FF6039,
      0xCE618B7E, 0x50FF6039, 0xCE61AB7E, 0x60FF6039, 0xCE61CB7E, 0x70FF6039,
      0xCE61EB7E, 0x80FF6039, 0xCE610B7F, 0x90FF6039, 0xCE612B7F, 0xA0FF6039,
      0xCE614B7F, 0xB0FF6039, 0xCE616B7F, 0xC0FF6039, 0xCE618B7F, 0xD0FF6039,
      0xCE61AB7F, 0xE0FF6039, 0xCE61CB7F, 0xF0FF6039,  // __savevmx_31
      0xCE61EB7F, 0x2000804E,

      0x00FC6039,  // __savevmx_64
      0xCB610B10, 0x10FC6039, 0xCB612B10, 0x20FC6039, 0xCB614B10, 0x30FC6039,
      0xCB616B10, 0x40FC6039, 0xCB618B10, 0x50FC6039, 0xCB61AB10, 0x60FC6039,
      0xCB61CB10, 0x70FC6039, 0xCB61EB10, 0x80FC6039, 0xCB610B11, 0x90FC6039,
      0xCB612B11, 0xA0FC6039, 0xCB614B11, 0xB0FC6039, 0xCB616B11, 0xC0FC6039,
      0xCB618B11, 0xD0FC6039, 0xCB61AB11, 0xE0FC6039, 0xCB61CB11, 0xF0FC6039,
      0xCB61EB11, 0x00FD6039, 0xCB610B12, 0x10FD6039, 0xCB612B12, 0x20FD6039,
      0xCB614B12, 0x30FD6039, 0xCB616B12, 0x40FD6039, 0xCB618B12, 0x50FD6039,
      0xCB61AB12, 0x60FD6039, 0xCB61CB12, 0x70FD6039, 0xCB61EB12, 0x80FD6039,
      0xCB610B13, 0x90FD6039, 0xCB612B13, 0xA0FD6039, 0xCB614B13, 0xB0FD6039,
      0xCB616B13, 0xC0FD6039, 0xCB618B13, 0xD0FD6039, 0xCB61AB13, 0xE0FD6039,
      0xCB61CB13, 0xF0FD6039, 0xCB61EB13, 0x00FE6039, 0xCF610B10, 0x10FE6039,
      0xCF612B10, 0x20FE6039, 0xCF614B10, 0x30FE6039, 0xCF616B10, 0x40FE6039,
      0xCF618B10, 0x50FE6039, 0xCF61AB10, 0x60FE6039, 0xCF61CB10, 0x70FE6039,
      0xCF61EB10, 0x80FE6039, 0xCF610B11, 0x90FE6039, 0xCF612B11, 0xA0FE6039,
      0xCF614B11, 0xB0FE6039, 0xCF616B11, 0xC0FE6039, 0xCF618B11, 0xD0FE6039,
      0xCF61AB11, 0xE0FE6039, 0xCF61CB11, 0xF0FE6039, 0xCF61EB11, 0x00FF6039,
      0xCF610B12, 0x10FF6039, 0xCF612B12, 0x20FF6039, 0xCF614B12, 0x30FF6039,
      0xCF616B12, 0x40FF6039, 0xCF618B12, 0x50FF6039, 0xCF61AB12, 0x60FF6039,
      0xCF61CB12, 0x70FF6039, 0xCF61EB12, 0x80FF6039, 0xCF610B13, 0x90FF6039,
      0xCF612B13, 0xA0FF6039, 0xCF614B13, 0xB0FF6039, 0xCF616B13, 0xC0FF6039,
      0xCF618B13, 0xD0FF6039, 0xCF61AB13, 0xE0FF6039, 0xCF61CB13,
      0xF0FF6039,  // __savevmx_127
      0xCF61EB13, 0x2000804E,

      0xE0FE6039,  // __restvmx_14
      0xCE60CB7D, 0xF0FE6039, 0xCE60EB7D, 0x00FF6039, 0xCE600B7E, 0x10FF6039,
      0xCE602B7E, 0x20FF6039, 0xCE604B7E, 0x30FF6039, 0xCE606B7E, 0x40FF6039,
      0xCE608B7E, 0x50FF6039, 0xCE60AB7E, 0x60FF6039, 0xCE60CB7E, 0x70FF6039,
      0xCE60EB7E, 0x80FF6039, 0xCE600B7F, 0x90FF6039, 0xCE602B7F, 0xA0FF6039,
      0xCE604B7F, 0xB0FF6039, 0xCE606B7F, 0xC0FF6039, 0xCE608B7F, 0xD0FF6039,
      0xCE60AB7F, 0xE0FF6039, 0xCE60CB7F, 0xF0FF6039,  // __restvmx_31
      0xCE60EB7F, 0x2000804E,

      0x00FC6039,  // __restvmx_64
      0xCB600B10, 0x10FC6039, 0xCB602B10, 0x20FC6039, 0xCB604B10, 0x30FC6039,
      0xCB606B10, 0x40FC6039, 0xCB608B10, 0x50FC6039, 0xCB60AB10, 0x60FC6039,
      0xCB60CB10, 0x70FC6039, 0xCB60EB10, 0x80FC6039, 0xCB600B11, 0x90FC6039,
      0xCB602B11, 0xA0FC6039, 0xCB604B11, 0xB0FC6039, 0xCB606B11, 0xC0FC6039,
      0xCB608B11, 0xD0FC6039, 0xCB60AB11, 0xE0FC6039, 0xCB60CB11, 0xF0FC6039,
      0xCB60EB11, 0x00FD6039, 0xCB600B12, 0x10FD6039, 0xCB602B12, 0x20FD6039,
      0xCB604B12, 0x30FD6039, 0xCB606B12, 0x40FD6039, 0xCB608B12, 0x50FD6039,
      0xCB60AB12, 0x60FD6039, 0xCB60CB12, 0x70FD6039, 0xCB60EB12, 0x80FD6039,
      0xCB600B13, 0x90FD6039, 0xCB602B13, 0xA0FD6039, 0xCB604B13, 0xB0FD6039,
      0xCB606B13, 0xC0FD6039, 0xCB608B13, 0xD0FD6039, 0xCB60AB13, 0xE0FD6039,
      0xCB60CB13, 0xF0FD6039, 0xCB60EB13, 0x00FE6039, 0xCF600B10, 0x10FE6039,
      0xCF602B10, 0x20FE6039, 0xCF604B10, 0x30FE6039, 0xCF606B10, 0x40FE6039,
      0xCF608B10, 0x50FE6039, 0xCF60AB10, 0x60FE6039, 0xCF60CB10, 0x70FE6039,
      0xCF60EB10, 0x80FE6039, 0xCF600B11, 0x90FE6039, 0xCF602B11, 0xA0FE6039,
      0xCF604B11, 0xB0FE6039, 0xCF606B11, 0xC0FE6039, 0xCF608B11, 0xD0FE6039,
      0xCF60AB11, 0xE0FE6039, 0xCF60CB11, 0xF0FE6039, 0xCF60EB11, 0x00FF6039,
      0xCF600B12, 0x10FF6039, 0xCF602B12, 0x20FF6039, 0xCF604B12, 0x30FF6039,
      0xCF606B12, 0x40FF6039, 0xCF608B12, 0x50FF6039, 0xCF60AB12, 0x60FF6039,
      0xCF60CB12, 0x70FF6039, 0xCF60EB12, 0x80FF6039, 0xCF600B13, 0x90FF6039,
      0xCF602B13, 0xA0FF6039, 0xCF604B13, 0xB0FF6039, 0xCF606B13, 0xC0FF6039,
      0xCF608B13, 0xD0FF6039, 0xCF60AB13, 0xE0FF6039, 0xCF60CB13,
      0xF0FF6039,  // __restvmx_127
      0xCF60EB13, 0x2000804E,
  };

  // TODO(benvanik): these are almost always sequential, if present.
  //     It'd be smarter to search around the other ones to prevent
  //     3 full module scans.
  uint32_t gplr_start = 0;
  uint32_t fpr_start = 0;
  uint32_t vmx_start = 0;

  auto page_size = base_address_ <= 0x90000000 ? 64 * 1024 : 4 * 1024;
  auto sec_header = xex_security_info();
  std::vector<uint32_t> resolve_on_exit{};
  resolve_on_exit.reserve(256);
  for (uint32_t i = 0, page = 0; i < sec_header->page_descriptor_count; i++) {
    // Byteswap the bitfield manually.
    xex2_page_descriptor desc;
    desc.value = xe::byte_swap(sec_header->page_descriptors[i].value);

    const auto start_address = base_address_ + (page * page_size);
    const auto end_address = start_address + (desc.page_count * page_size);

    if (desc.info == XEX_SECTION_CODE) {
      if (!gplr_start) {
        gplr_start = memory_->SearchAligned(start_address, end_address,
                                            gprlr_code_values,
                                            xe::countof(gprlr_code_values));
      }
      if (!fpr_start) {
        fpr_start =
            memory_->SearchAligned(start_address, end_address, fpr_code_values,
                                   xe::countof(fpr_code_values));
      }
      if (!vmx_start) {
        vmx_start =
            memory_->SearchAligned(start_address, end_address, vmx_code_values,
                                   xe::countof(vmx_code_values));
      }
      if (gplr_start && fpr_start && vmx_start) {
        break;
      }
    }

    page += desc.page_count;
  }

  // Add function stubs.
  char name[32];

  auto AddXexFunction = [this, &resolve_on_exit](uint32_t address,
                                                 Function** function) {
    DeclareFunction(address, function);
    resolve_on_exit.push_back(address);
  };
  if (gplr_start) {
    uint32_t address = gplr_start;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savegprlr_{}", n);
      Function* function;

      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 2 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveGprLr;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::GPR, false, n);

      address += 4;
    }
    address = gplr_start + 20 * 4;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restgprlr_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 3 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestGprLr;
      function->set_behavior(Function::Behavior::kEpilogReturn);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::GPR, true, n);
      address += 4;
    }
  }
  if (fpr_start) {
    uint32_t address = fpr_start;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savefpr_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 1 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveFpr;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);

      function->SetSaverest(cpu::SaveRestoreType::FPR, false, n);
      address += 4;
    }
    address = fpr_start + (18 * 4) + (1 * 4);
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restfpr_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_end_address(address + (31 - n) * 4 + 1 * 4);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestFpr;
      function->set_behavior(Function::Behavior::kEpilog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::FPR, true, n);
      address += 4;
    }
  }
  if (vmx_start) {
    // vmx is:
    // 14-31 save
    // 64-127 save
    // 14-31 rest
    // 64-127 rest
    uint32_t address = vmx_start;
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savevmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveVmx;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, false, n);
      address += 2 * 4;
    }
    address += 4;
    for (int n = 64; n <= 127; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__savevmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagSaveVmx;
      function->set_behavior(Function::Behavior::kProlog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, false, n);
      address += 2 * 4;
    }
    address = vmx_start + (18 * 2 * 4) + (1 * 4) + (64 * 2 * 4) + (1 * 4);
    for (int n = 14; n <= 31; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restvmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestVmx;
      function->set_behavior(Function::Behavior::kEpilog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, true, n);
      address += 2 * 4;
    }
    address += 4;
    for (int n = 64; n <= 127; n++) {
      auto format_result =
          fmt::format_to_n(name, xe::countof(name), "__restvmx_{}", n);
      Function* function;
      AddXexFunction(address, &function);
      function->set_name(std::string_view(name, format_result.size));
      // TODO(benvanik): set type  fn->type = FunctionSymbol::User;
      // TODO(benvanik): set flags fn->flags |= FunctionSymbol::kFlagRestVmx;
      function->set_behavior(Function::Behavior::kEpilog);
      function->set_status(Symbol::Status::kDeclared);
      function->SetSaverest(cpu::SaveRestoreType::VMX, true, n);
      address += 2 * 4;
    }
  }
  if (cvars::enable_early_precompilation) {
    for (auto&& to_ensure_precompiled : resolve_on_exit) {
      // we want to make sure an address for these functions is available before
      // any other functions are compiled for code generation purposes but we do
      // it outside of our loops, because we also want to make sure we've marked
      // up the symbol with info about it being save/rest and whatnot
      processor_->ResolveFunction(to_ensure_precompiled);
    }
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
