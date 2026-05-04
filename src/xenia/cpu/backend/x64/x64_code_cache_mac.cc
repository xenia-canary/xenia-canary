/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_code_cache_mac.h"

#include <cstdlib>
#include <cstring>

#if ENABLE_VTUNE
#include "../../../third_party/DirectXShaderCompiler/lib/ExecutionEngine/IntelJITEvents/jitprofiling.h"
#endif

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math_mac.h"
#include "xenia/base/memory_mac.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/backend/x64/x64_backend_mac.h"

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

using namespace xe::literals;

X64CodeCache::X64CodeCache() = default;

X64CodeCache::~X64CodeCache() {
  if (indirection_table_base_) {
    xe::memory::DeallocFixed(indirection_table_base_, 0,
                             xe::memory::DeallocationType::kRelease);
  }

  // Unmap all views and close mapping.
  if (mapping_ != xe::memory::kFileMappingHandleInvalid) {
    if (generated_code_write_base_ &&
        generated_code_write_base_ != generated_code_execute_base_) {
      xe::memory::UnmapFileView(mapping_, generated_code_write_base_,
                                kGeneratedCodeSize);
    }
    if (generated_code_execute_base_) {
      xe::memory::UnmapFileView(mapping_, generated_code_execute_base_,
                                kGeneratedCodeSize);
    }
    xe::memory::CloseFileMappingHandle(mapping_, file_name_);
    mapping_ = xe::memory::kFileMappingHandleInvalid;
  }
}

bool X64CodeCache::Initialize() {
  void* indirection_base = reinterpret_cast<void*>(kIndirectionTableBase);
#ifdef __APPLE__
  // macOS MAP_JIT requires the OS to pick the address.
  indirection_base = nullptr;
#endif

  indirection_table_base_ = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
      indirection_base, kIndirectionTableSize,
      xe::memory::AllocationType::kReserve,
      xe::memory::PageAccess::kReadWrite));
  if (!indirection_table_base_) {
    XELOGE("Unable to allocate code cache indirection table");
    XELOGE(
        "This is likely because the {:X}-{:X} range is in use by some other "
        "system DLL",
        static_cast<uint64_t>(kIndirectionTableBase),
        kIndirectionTableBase + kIndirectionTableSize);
  }

  // Create mmap file. This allows us to share the code cache with the debugger.
  // macOS uses MAP_ANONYMOUS instead, so we allocate anonymous memory instead
#ifndef __APPLE__
  file_name_ = fmt::format("xenia_code_cache_{}", Clock::QueryHostTickCount());
  mapping_ = xe::memory::CreateFileMappingHandle(
      file_name_, kGeneratedCodeSize, xe::memory::PageAccess::kExecuteReadWrite,
      false);
  if (mapping_ == xe::memory::kFileMappingHandleInvalid) {
    XELOGE("Unable to create code cache mmap");
    return false;
  }
#endif

  // Map generated code region into the file. Pages are committed as required.
  if (xe::memory::IsWritableExecutableMemoryPreferred()) {
#ifdef __APPLE__
    // use AllocFixed to get MAP_JIT, MAP_ANONYMOUS support.
    // Allow OS to pick address (nullptr).
    generated_code_execute_base_ = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
          nullptr, kGeneratedCodeSize,
          xe::memory::AllocationType::kReserve, xe::memory::PageAccess::kExecuteReadWrite));
#else
    generated_code_execute_base_ =
        reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
            mapping_, reinterpret_cast<void*>(kGeneratedCodeExecuteBase),
            kGeneratedCodeSize, xe::memory::PageAccess::kExecuteReadWrite, 0));
#endif

    generated_code_write_base_ = generated_code_execute_base_;
    if (!generated_code_execute_base_ || !generated_code_write_base_) {
      XELOGE("Unable to allocate code cache generated code storage");
      XELOGE(
          "This is likely because the {:X}-{:X} range is in use by some other "
          "system DLL",
          uint64_t(kGeneratedCodeExecuteBase),
          uint64_t(kGeneratedCodeExecuteBase + kGeneratedCodeSize));
      return false;
    }
  } else {
#ifndef __APPLE__
    generated_code_execute_base_ =
        reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
            mapping_, reinterpret_cast<void*>(kGeneratedCodeExecuteBase),
            kGeneratedCodeSize, xe::memory::PageAccess::kExecuteReadOnly, 0));
    generated_code_write_base_ =
        reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
            mapping_, reinterpret_cast<void*>(kGeneratedCodeWriteBase),
            kGeneratedCodeSize, xe::memory::PageAccess::kReadWrite, 0));
#else
    // macOS needs MAP_JIT which implies RWX usually, or pthread_jit_write_protect.
    generated_code_execute_base_ = nullptr;
    generated_code_write_base_ = nullptr;
#endif
    if (!generated_code_execute_base_ || !generated_code_write_base_) {
      XELOGE("Unable to allocate code cache generated code storage");
      XELOGE(
          "This is likely because the {:X}-{:X} and {:X}-{:X} ranges are in "
          "use by some other system DLL",
          uint64_t(kGeneratedCodeExecuteBase),
          uint64_t(kGeneratedCodeExecuteBase + kGeneratedCodeSize),
          uint64_t(kGeneratedCodeWriteBase),
          uint64_t(kGeneratedCodeWriteBase + kGeneratedCodeSize));
      return false;
    }
  }

  // Preallocate the function map to a large, reasonable size.
  generated_code_map_.reserve(kMaximumFunctionCount);

  return true;
}

void X64CodeCache::set_indirection_default(uint32_t default_value) {
  indirection_default_value_ = default_value;
}

void X64CodeCache::AddIndirection(uint32_t guest_address,
                                  uint32_t host_address) {
  if (!indirection_table_base_) {
    return;
  }

  uint32_t* indirection_slot = reinterpret_cast<uint32_t*>(
      indirection_table_base_ + (guest_address - kIndirectionTableBase));
  *indirection_slot = host_address;
}

void X64CodeCache::CommitExecutableRange(uint32_t guest_low,
                                         uint32_t guest_high) {
  if (!indirection_table_base_) {
    return;
  }

  // Commit the memory.
  xe::memory::AllocFixed(
      indirection_table_base_ + (guest_low - kIndirectionTableBase),
      guest_high - guest_low, xe::memory::AllocationType::kCommit,
      xe::memory::PageAccess::kReadWrite);

  // Fill memory with the default value.
  uint32_t* p = reinterpret_cast<uint32_t*>(indirection_table_base_);
  for (uint32_t address = guest_low; address < guest_high; ++address) {
    p[(address - kIndirectionTableBase) / 4] = indirection_default_value_;
  }
}

void X64CodeCache::PlaceHostCode(uint32_t guest_address, void* machine_code,
                                 const EmitFunctionInfo& func_info,
                                 void*& code_execute_address_out,
                                 void*& code_write_address_out) {
  // Same for now. We may use different pools or whatnot later on, like when
  // we only want to place guest code in a serialized cache on disk.
  PlaceGuestCode(guest_address, machine_code, func_info, nullptr,
                 code_execute_address_out, code_write_address_out);
}

void X64CodeCache::PlaceGuestCode(uint32_t guest_address, void* machine_code,
                                  const EmitFunctionInfo& func_info,
                                  GuestFunction* function_info,
                                  void*& code_execute_address_out,
                                  void*& code_write_address_out) {
  // Hold a lock while we bump the pointers up. This is important as the
  // unwind table requires entries AND code to be sorted in order.
  size_t low_mark;
  size_t high_mark;
  uint8_t* code_execute_address;
  UnwindReservation unwind_reservation;
  {
    auto global_lock = global_critical_region_.Acquire();

    //low_mark = generated_code_offset_;

    // Reserve code.
    // Always move the code to land on 16b alignment.
    code_execute_address =
        generated_code_execute_base_ + generated_code_offset_;
    code_execute_address_out = code_execute_address;
    uint8_t* code_write_address =
        generated_code_write_base_ + generated_code_offset_;
    code_write_address_out = code_write_address;
    generated_code_offset_ += xe::round_up(func_info.code_size.total, 16);

    auto tail_write_address =
        generated_code_write_base_ + generated_code_offset_;

    // Reserve unwind info.
    // We go on the high size of the unwind info as we don't know how big we
    // need it, and a few extra bytes of padding isn't the worst thing.
    unwind_reservation = RequestUnwindReservation(generated_code_write_base_ +
                                                  generated_code_offset_);
    generated_code_offset_ += xe::round_up(unwind_reservation.data_size, 16);

    auto end_write_address =
        generated_code_write_base_ + generated_code_offset_;

    high_mark = generated_code_offset_;

    // Store in map. It is maintained in sorted order of host PC dependent on
    // us also being append-only.
    generated_code_map_.emplace_back(
        (uint64_t(code_execute_address - generated_code_execute_base_) << 32) |
            generated_code_offset_,
        function_info);

    // TODO(DrChat): The following code doesn't really need to be under the
    // global lock except for PlaceCode (but it depends on the previous code
    // already being ran)

    // If we are going above the high water mark of committed memory, commit
    // some more. It's ok if multiple threads do this, as redundant commits
    // aren't harmful.
    size_t old_commit_mark, new_commit_mark;
    do {
      old_commit_mark = generated_code_commit_mark_;
      if (high_mark <= old_commit_mark) break;

      new_commit_mark = old_commit_mark + 16_MiB;
      if (generated_code_execute_base_ == generated_code_write_base_) {
        xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                               xe::memory::AllocationType::kCommit,
                               xe::memory::PageAccess::kExecuteReadWrite);
      } else {
        xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                               xe::memory::AllocationType::kCommit,
                               xe::memory::PageAccess::kExecuteReadOnly);
        xe::memory::AllocFixed(generated_code_write_base_, new_commit_mark,
                               xe::memory::AllocationType::kCommit,
                               xe::memory::PageAccess::kReadWrite);
      }
    } while (generated_code_commit_mark_.compare_exchange_weak(
        old_commit_mark, new_commit_mark));

    // Copy code.
    std::memcpy(code_write_address, machine_code, func_info.code_size.total);

    // Fill unused slots with 0xCC
    std::memset(tail_write_address, 0xCC,
                static_cast<size_t>(end_write_address - tail_write_address));

    // Notify subclasses of placed code.
    PlaceCode(guest_address, machine_code, func_info, code_execute_address,
              unwind_reservation);
  }

#if ENABLE_VTUNE
  if (iJIT_IsProfilingActive() == iJIT_SAMPLING_ON) {
    std::string method_name;
    if (function_info && function_info->name().size() != 0) {
      method_name = function_info->name();
    } else {
      method_name = xe::format_string("sub_%.8X", guest_address);
    }

    iJIT_Method_Load_V2 method = {0};
    method.method_id = iJIT_GetNewMethodID();
    method.method_load_address = code_execute_address;
    method.method_size = uint32_t(code_size);
    method.method_name = const_cast<char*>(method_name.data());
    method.module_name =
        function_info
            ? const_cast<char*>(function_info->module()->name().c_str())
            : nullptr;
    iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED_V2, &method);
  }
#endif

  // Now that everything is ready, fix up the indirection table.
  // Note that we do support code that doesn't have an indirection fixup, so
  // ignore those when we see them.
  if (guest_address && indirection_table_base_) {
    uint32_t* indirection_slot = reinterpret_cast<uint32_t*>(
        indirection_table_base_ + (guest_address - kIndirectionTableBase));
    *indirection_slot =
        uint32_t(reinterpret_cast<uint64_t>(code_execute_address));
  }
  
  // Notify performance monitor of code generation
  if (backend_) {
    auto x64_backend = static_cast<X64Backend*>(backend_);
    auto perf_monitor = x64_backend->GetPerformanceMonitor();
    if (perf_monitor) {
      perf_monitor->NotifyCodeGenerated(guest_address, code_execute_address, 
                                      func_info.code_size.total);
      
      // Update code cache metrics
      perf_monitor->UpdateCodeCacheMetrics(
          kGeneratedCodeSize, generated_code_offset_, 
          static_cast<uint32_t>(generated_code_map_.size()),
          static_cast<uint32_t>(generated_code_map_.size()));
      
      // Check for cache pressure and handle if needed
      double utilization = (static_cast<double>(generated_code_offset_) / kGeneratedCodeSize) * 100.0;
      if (utilization > 90.0) {  // 90% threshold
        perf_monitor->HandleCachePressure();
      }
      
      // Notify VTune of function compilation if function info is available
      if (function_info) {
        char func_name[128];
        snprintf(func_name, sizeof(func_name), "guest_func_0x%08X", guest_address);
        perf_monitor->NotifyFunctionCompiled(guest_address, func_name);
      }
    }
  }
}

uint32_t X64CodeCache::PlaceData(const void* data, size_t length) {
  // Hold a lock while we bump the pointers up.
  size_t high_mark;
  uint8_t* data_address = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();

    // Reserve code.
    // Always move the code to land on 16b alignment.
    data_address = generated_code_write_base_ + generated_code_offset_;
    generated_code_offset_ += xe::round_up(length, 16);

    high_mark = generated_code_offset_;
  }

  // If we are going above the high water mark of committed memory, commit some
  // more. It's ok if multiple threads do this, as redundant commits aren't
  // harmful.
  size_t old_commit_mark, new_commit_mark;
  do {
    old_commit_mark = generated_code_commit_mark_;
    if (high_mark <= old_commit_mark) break;

    new_commit_mark = old_commit_mark + 16_MiB;
    if (generated_code_execute_base_ == generated_code_write_base_) {
      xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                             xe::memory::AllocationType::kCommit,
                             xe::memory::PageAccess::kExecuteReadWrite);
    } else {
      xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                             xe::memory::AllocationType::kCommit,
                             xe::memory::PageAccess::kExecuteReadOnly);
      xe::memory::AllocFixed(generated_code_write_base_, new_commit_mark,
                             xe::memory::AllocationType::kCommit,
                             xe::memory::PageAccess::kReadWrite);
    }
  } while (generated_code_commit_mark_.compare_exchange_weak(old_commit_mark,
                                                             new_commit_mark));

  // Copy code.
  std::memcpy(data_address, data, length);

  return uint32_t(uintptr_t(data_address));
}

GuestFunction* X64CodeCache::LookupFunction(uint64_t host_pc) {
  // Use dynamic base address calculation
  if (!generated_code_execute_base_) {
    return nullptr;
  }
  uint32_t key = uint32_t(host_pc - reinterpret_cast<uintptr_t>(generated_code_execute_base_));
  void* fn_entry = std::bsearch(
      &key, generated_code_map_.data(), generated_code_map_.size() + 1,
      sizeof(std::pair<uint32_t, Function*>),
      [](const void* key_ptr, const void* element_ptr) {
        auto key = *reinterpret_cast<const uint32_t*>(key_ptr);
        auto element =
            reinterpret_cast<const std::pair<uint64_t, GuestFunction*>*>(
                element_ptr);
        if (key < (element->first >> 32)) {
          return -1;
        } else if (key > uint32_t(element->first)) {
          return 1;
        } else {
          return 0;
        }
      });
  if (fn_entry) {
    return reinterpret_cast<const std::pair<uint64_t, GuestFunction*>*>(
               fn_entry)
        ->second;
  } else {
    return nullptr;
  }
}

void X64CodeCache::InvalidateCodeRange(uint32_t guest_start, uint32_t guest_end) {
  if (!indirection_table_base_ || guest_start >= guest_end) {
    return;
  }
  
  auto global_lock = global_critical_region_.Acquire();
  
  // Clear indirection table entries for the range
  uint32_t start_offset = guest_start - kIndirectionTableBase;
  uint32_t end_offset = guest_end - kIndirectionTableBase;
  
  if (start_offset < kIndirectionTableSize && end_offset <= kIndirectionTableSize) {
    uint32_t* table_start = reinterpret_cast<uint32_t*>(indirection_table_base_ + start_offset);
    uint32_t* table_end = reinterpret_cast<uint32_t*>(indirection_table_base_ + end_offset);
    
    for (uint32_t* ptr = table_start; ptr < table_end; ++ptr) {
      *ptr = indirection_default_value_;
    }
  }
  
  // Remove functions from generated code map
  auto it = generated_code_map_.begin();
  while (it != generated_code_map_.end()) {
    uint32_t func_guest_addr = static_cast<uint32_t>(it->first >> 32);
    //uint32_t func_end_addr = static_cast<uint32_t>(it->first);
    
    // Check if function is in the invalidation range
    if (func_guest_addr >= guest_start && func_guest_addr < guest_end) {
      // Clear from indirection table
      uint32_t* indirection_slot = reinterpret_cast<uint32_t*>(
          indirection_table_base_ + (func_guest_addr - kIndirectionTableBase));
      *indirection_slot = indirection_default_value_;
      
      it = generated_code_map_.erase(it);
    } else {
      ++it;
    }
  }
  
  XELOGI("Invalidated code range: 0x{:08X} - 0x{:08X}", guest_start, guest_end);
}

void X64CodeCache::InvalidateFunction(uint32_t guest_address) {
  InvalidateCodeRange(guest_address, guest_address + 4);
}

void X64CodeCache::SetCachePressureHandler(void* handler) {
  // Store handler reference for cache pressure notifications
  // This would be implemented based on specific requirements
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
