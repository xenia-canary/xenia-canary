/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_tracers.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

#define ITRACE 0
#define DTRACE 0

#define TARGET_THREAD 0

bool trace_enabled = true;

#define THREAD_MATCH (!TARGET_THREAD || ppc_context->thread_id == TARGET_THREAD)
#define IFLUSH()
#define IPRINT(s)                    \
  if (trace_enabled && THREAD_MATCH) \
  xe::logging::AppendLogLine(xe::LogLevel::Debug, 't', s, xe::LogSrc::Cpu)
#define DFLUSH()
#define DPRINT(...)                                                           \
  if (trace_enabled && THREAD_MATCH)                                          \
  xe::logging::AppendLogLineFormat(xe::LogSrc::Cpu, xe::LogLevel::Debug, 't', \
                                   __VA_ARGS__)

// Helper to read float/int lanes from a V128 passed as const uint8_t*.
static inline float v128_f32(const uint8_t* v, int lane) {
  float f;
  std::memcpy(&f, v + lane * 4, 4);
  return f;
}
static inline uint32_t v128_i32(const uint8_t* v, int lane) {
  uint32_t i;
  std::memcpy(&i, v + lane * 4, 4);
  return i;
}

uint32_t GetTracingMode() {
  uint32_t mode = 0;
#if ITRACE
  mode |= TRACING_INSTR;
#endif
#if DTRACE
  mode |= TRACING_DATA;
#endif
  return mode;
}

void TraceString(void* raw_context, const char* str) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  IPRINT(str);
  IFLUSH();
}

void TraceContextLoadI8(void* raw_context, uint64_t offset, uint8_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = ctx i8 +{}\n", (int8_t)value, value, offset);
}
void TraceContextLoadI16(void* raw_context, uint64_t offset, uint16_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = ctx i16 +{}\n", (int16_t)value, value, offset);
}
void TraceContextLoadI32(void* raw_context, uint64_t offset, uint32_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = ctx i32 +{}\n", (int32_t)value, value, offset);
}
void TraceContextLoadI64(void* raw_context, uint64_t offset, uint64_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = ctx i64 +{}\n", (int64_t)value, value, offset);
}
void TraceContextLoadF32(void* raw_context, uint64_t offset, float value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint32_t iv;
  std::memcpy(&iv, &value, 4);
  DPRINT("{} ({:X}) = ctx f32 +{}\n", value, iv, offset);
}
void TraceContextLoadF64(void* raw_context, uint64_t offset, double value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint64_t iv;
  std::memcpy(&iv, &value, 8);
  DPRINT("{} ({:X}) = ctx f64 +{}\n", value, iv, offset);
}
void TraceContextLoadV128(void* raw_context, uint64_t offset,
                          const uint8_t* value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("[{}, {}, {}, {}] [{:08X}, {:08X}, {:08X}, {:08X}] = ctx v128 +{}\n",
         v128_f32(value, 0), v128_f32(value, 1), v128_f32(value, 2),
         v128_f32(value, 3), v128_i32(value, 0), v128_i32(value, 1),
         v128_i32(value, 2), v128_i32(value, 3), offset);
}

void TraceContextStoreI8(void* raw_context, uint64_t offset, uint8_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("ctx i8 +{} = {} ({:X})\n", offset, (int8_t)value, value);
}
void TraceContextStoreI16(void* raw_context, uint64_t offset, uint16_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("ctx i16 +{} = {} ({:X})\n", offset, (int16_t)value, value);
}
void TraceContextStoreI32(void* raw_context, uint64_t offset, uint32_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("ctx i32 +{} = {} ({:X})\n", offset, (int32_t)value, value);
}
void TraceContextStoreI64(void* raw_context, uint64_t offset, uint64_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("ctx i64 +{} = {} ({:X})\n", offset, (int64_t)value, value);
}
void TraceContextStoreF32(void* raw_context, uint64_t offset, float value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint32_t iv;
  std::memcpy(&iv, &value, 4);
  DPRINT("ctx f32 +{} = {} ({:X})\n", offset, value, iv);
}
void TraceContextStoreF64(void* raw_context, uint64_t offset, double value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint64_t iv;
  std::memcpy(&iv, &value, 8);
  DPRINT("ctx f64 +{} = {} ({:X})\n", offset, value, iv);
}
void TraceContextStoreV128(void* raw_context, uint64_t offset,
                           const uint8_t* value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("ctx v128 +{} = [{}, {}, {}, {}] [{:08X}, {:08X}, {:08X}, {:08X}]\n",
         offset, v128_f32(value, 0), v128_f32(value, 1), v128_f32(value, 2),
         v128_f32(value, 3), v128_i32(value, 0), v128_i32(value, 1),
         v128_i32(value, 2), v128_i32(value, 3));
}

void TraceMemoryLoadI8(void* raw_context, uint32_t address, uint8_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = load.i8 {:08X}\n", (int8_t)value, value, address);
}
void TraceMemoryLoadI16(void* raw_context, uint32_t address, uint16_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = load.i16 {:08X}\n", (int16_t)value, value, address);
}
void TraceMemoryLoadI32(void* raw_context, uint32_t address, uint32_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = load.i32 {:08X}\n", (int32_t)value, value, address);
}
void TraceMemoryLoadI64(void* raw_context, uint32_t address, uint64_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("{} ({:X}) = load.i64 {:08X}\n", (int64_t)value, value, address);
}
void TraceMemoryLoadF32(void* raw_context, uint32_t address, float value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint32_t iv;
  std::memcpy(&iv, &value, 4);
  DPRINT("{} ({:X}) = load.f32 {:08X}\n", value, iv, address);
}
void TraceMemoryLoadF64(void* raw_context, uint32_t address, double value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint64_t iv;
  std::memcpy(&iv, &value, 8);
  DPRINT("{} ({:X}) = load.f64 {:08X}\n", value, iv, address);
}
void TraceMemoryLoadV128(void* raw_context, uint32_t address,
                         const uint8_t* value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT(
      "[{}, {}, {}, {}] [{:08X}, {:08X}, {:08X}, {:08X}] = load.v128 {:08X}\n",
      v128_f32(value, 0), v128_f32(value, 1), v128_f32(value, 2),
      v128_f32(value, 3), v128_i32(value, 0), v128_i32(value, 1),
      v128_i32(value, 2), v128_i32(value, 3), address);
}

void TraceMemoryStoreI8(void* raw_context, uint32_t address, uint8_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("store.i8 {:08X} = {} ({:X})\n", address, (int8_t)value, value);
}
void TraceMemoryStoreI16(void* raw_context, uint32_t address, uint16_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("store.i16 {:08X} = {} ({:X})\n", address, (int16_t)value, value);
}
void TraceMemoryStoreI32(void* raw_context, uint32_t address, uint32_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("store.i32 {:08X} = {} ({:X})\n", address, (int32_t)value, value);
}
void TraceMemoryStoreI64(void* raw_context, uint32_t address, uint64_t value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("store.i64 {:08X} = {} ({:X})\n", address, (int64_t)value, value);
}
void TraceMemoryStoreF32(void* raw_context, uint32_t address, float value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint32_t iv;
  std::memcpy(&iv, &value, 4);
  DPRINT("store.f32 {:08X} = {} ({:X})\n", address, value, iv);
}
void TraceMemoryStoreF64(void* raw_context, uint32_t address, double value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  uint64_t iv;
  std::memcpy(&iv, &value, 8);
  DPRINT("store.f64 {:08X} = {} ({:X})\n", address, value, iv);
}
void TraceMemoryStoreV128(void* raw_context, uint32_t address,
                          const uint8_t* value) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT(
      "store.v128 {:08X} = [{}, {}, {}, {}] [{:08X}, {:08X}, {:08X}, {:08X}]\n",
      address, v128_f32(value, 0), v128_f32(value, 1), v128_f32(value, 2),
      v128_f32(value, 3), v128_i32(value, 0), v128_i32(value, 1),
      v128_i32(value, 2), v128_i32(value, 3));
}

void TraceMemset(void* raw_context, uint32_t address, uint8_t value,
                 uint32_t length) {
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  DPRINT("memset {:08X}-{:08X} ({}) = {:02X}", address, address + length,
         length, value);
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
