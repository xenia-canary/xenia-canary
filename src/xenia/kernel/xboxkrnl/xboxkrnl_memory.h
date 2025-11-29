/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MEMORY_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MEMORY_H_

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// https://code.google.com/p/vdash/source/browse/trunk/vdash/include/kernel.h
struct X_MM_QUERY_STATISTICS_SECTION {
  xe::be<uint32_t> available_pages;
  xe::be<uint32_t> total_virtual_memory_bytes;
  xe::be<uint32_t> reserved_virtual_memory_bytes;
  xe::be<uint32_t> physical_pages;
  xe::be<uint32_t> pool_pages;
  xe::be<uint32_t> stack_pages;
  xe::be<uint32_t> image_pages;
  xe::be<uint32_t> heap_pages;
  xe::be<uint32_t> virtual_pages;
  xe::be<uint32_t> page_table_pages;
  xe::be<uint32_t> cache_pages;
};

struct X_MM_QUERY_STATISTICS_RESULT {
  xe::be<uint32_t> size;
  xe::be<uint32_t> total_physical_pages;
  xe::be<uint32_t> kernel_pages;
  X_MM_QUERY_STATISTICS_SECTION title;
  X_MM_QUERY_STATISTICS_SECTION system;
  xe::be<uint32_t> highest_physical_page;
};
static_assert_size(X_MM_QUERY_STATISTICS_RESULT, 104);

uint32_t xeMmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t region_size,
                                      uint32_t protect_bits,
                                      uint32_t min_addr_range,
                                      uint32_t max_addr_range,
                                      uint32_t alignment);
dword_result_t xeMmQueryStatistics(
    pointer_t<X_MM_QUERY_STATISTICS_RESULT> stats_ptr);
uint32_t xeAllocatePoolTypeWithTag(PPCContext* context, uint32_t size,
                                   uint32_t tag, uint32_t zero);

void xeFreePool(PPCContext* context, uint32_t base_address);

uint32_t xeMmCreateKernelStack(uint32_t size, uint32_t r4);
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MEMORY_H_
