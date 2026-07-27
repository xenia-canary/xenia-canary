/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/backend.h"

#include <cstring>

#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace backend {

Backend::Backend() { std::memset(&machine_info_, 0, sizeof(machine_info_)); }
Backend::~Backend() = default;

bool Backend::Initialize(Processor* processor) {
  processor_ = processor;
  return true;
}

void* Backend::AllocThreadData() { return nullptr; }

void Backend::FreeThreadData(void* thread_data) {}

// The defaults store with a compare-exchange against the value the load read,
// so they stay atomic for backends without their own reservations.
static thread_local uint64_t reserved_value;

uint32_t Backend::ReservedLoad32(ppc::PPCContext* context, uint32_t address) {
  const uint32_t raw = *context->TranslateVirtual<volatile uint32_t*>(address);
  reserved_value = raw;
  return xe::byte_swap(raw);
}

uint64_t Backend::ReservedLoad64(ppc::PPCContext* context, uint32_t address) {
  const uint64_t raw = *context->TranslateVirtual<volatile uint64_t*>(address);
  reserved_value = raw;
  return xe::byte_swap(raw);
}

bool Backend::ReservedStore32(ppc::PPCContext* context, uint32_t address,
                              uint32_t value) {
  return xe::atomic_cas(static_cast<uint32_t>(reserved_value),
                        xe::byte_swap(value),
                        context->TranslateVirtual<volatile uint32_t*>(address));
}

bool Backend::ReservedStore64(ppc::PPCContext* context, uint32_t address,
                              uint64_t value) {
  return xe::atomic_cas(reserved_value, xe::byte_swap(value),
                        context->TranslateVirtual<volatile uint64_t*>(address));
}

}  // namespace backend
}  // namespace cpu
}  // namespace xe
