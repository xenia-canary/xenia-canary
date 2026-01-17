/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_emitter.h"

#include <array>
#include <atomic>
#include <cstddef>

#include <cctype>
#include <climits>
#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/debugging.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/backend/a64/a64_sequences.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/function_debug_info.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/symbol.h"
#include "xenia/cpu/thread_state.h"

#include "oaknut/feature_detection/cpu_feature.hpp"
#include "oaknut/feature_detection/feature_detection.hpp"
#include "oaknut/feature_detection/feature_detection_idregs.hpp"

DEFINE_bool(debugprint_trap_log, false,
            "Log debugprint traps to the active debugger", "CPU");
DEFINE_bool(ignore_undefined_externs, true,
            "Don't exit when an undefined extern is called.", "CPU");
DEFINE_bool(log_undefined_extern_args, false,
            "Log PPC args for undefined externs (once per function).", "CPU");
DEFINE_bool(emit_source_annotations, false,
            "Add extra movs and nops to make disassembly easier to read.",
            "CPU");
DEFINE_bool(a64_resolve_function_log, false,
            "Log A64 ResolveFunction failures with module ranges.", "CPU");
DEFINE_int32(a64_resolve_function_log_limit, 8,
             "Maximum ResolveFunction failure logs.", "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

using xe::cpu::hir::HIRBuilder;
using xe::cpu::hir::Instr;
using namespace xe::literals;
using namespace oaknut::util;

namespace {

bool ShouldLogResolveFailure() {
  if (!cvars::a64_resolve_function_log) {
    return false;
  }
  const int32_t limit = cvars::a64_resolve_function_log_limit;
  if (limit <= 0) {
    return false;
  }
  static std::atomic<int32_t> log_count{0};
  const int32_t count = log_count.fetch_add(1, std::memory_order_relaxed);
  return count < limit;
}

void AdjustStackPointer(A64Emitter& emitter, size_t stack_size, bool add) {
  if (!stack_size) {
    return;
  }
  const uint64_t size_u64 = static_cast<uint64_t>(stack_size);
  const bool imm_valid = size_u64 <= 0xFFF ||
                         ((size_u64 & 0xFFF) == 0 && (size_u64 >> 12) <= 0xFFF);
  if (imm_valid) {
    if (add) {
      emitter.ADD(SP, SP, size_u64);
    } else {
      emitter.SUB(SP, SP, size_u64);
    }
    return;
  }
  emitter.MOV(X15, size_u64);
  if (add) {
    emitter.ADD(SP, SP, X15);
  } else {
    emitter.SUB(SP, SP, X15);
  }
}

}  // namespace

static const size_t kStashOffset = 32;
// static const size_t kStashOffsetHigh = 32 + 32;

// Register indices that the HIR is allowed to use for operands
const uint8_t A64Emitter::gpr_reg_map_[A64Emitter::GPR_COUNT] = {
    19, 20, 21, 22, 23, 24, 25, 26,
};

const uint8_t A64Emitter::fpr_reg_map_[A64Emitter::FPR_COUNT] = {
    8, 9, 10, 11, 12, 13, 14, 15,
};

A64Emitter::A64Emitter(A64Backend* backend)
    : VectorCodeGenerator(assembly_buffer),
      processor_(backend->processor()),
      backend_(backend),
      code_cache_(backend->code_cache()) {
  oaknut::CpuFeatures cpu_ = oaknut::detect_features();

  // Combine with id register detection
#if OAKNUT_SUPPORTS_READING_ID_REGISTERS > 0
#if OAKNUT_SUPPORTS_READING_ID_REGISTERS == 1
  const std::optional<oaknut::id::IdRegisters> id_registers =
      oaknut::read_id_registers();
#elif OAKNUT_SUPPORTS_READING_ID_REGISTERS == 2
  const std::optional<oaknut::id::IdRegisters> id_registers =
      oaknut::read_id_registers(0);
#endif
  if (id_registers.has_value()) {
    cpu_ = cpu_ | oaknut::detect_features_via_id_registers(*id_registers);
  }
#endif

#define TEST_EMIT_FEATURE(emit, ext)                \
  if ((cvars::a64_extension_mask & emit) == emit) { \
    feature_flags_ |= (cpu_.has(ext) ? emit : 0);   \
  }

  TEST_EMIT_FEATURE(kA64EmitLSE, oaknut::CpuFeature::LSE);
  TEST_EMIT_FEATURE(kA64EmitF16C, oaknut::CpuFeature::FP16Conv);

#undef TEST_EMIT_FEATURE
}

A64Emitter::~A64Emitter() = default;

bool A64Emitter::Emit(GuestFunction* function, HIRBuilder* builder,
                      uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
                      void** out_code_address, size_t* out_code_size,
                      std::vector<SourceMapEntry>* out_source_map) {
  SCOPE_profile_cpu_f("cpu");

  // Reset.
  debug_info_ = debug_info;
  debug_info_flags_ = debug_info_flags;
  trace_data_ = &function->trace_data();
  source_map_arena_.Reset();

  // Fill the generator with code.
  EmitFunctionInfo func_info = {};
  if (!Emit(builder, func_info)) {
    return false;
  }

  // Copy the final code to the cache and relocate it.
  *out_code_size = offset();
  *out_code_address = Emplace(func_info, function);

  // Stash source map.
  source_map_arena_.CloneContents(out_source_map);

  return true;
}

void* A64Emitter::Emplace(const EmitFunctionInfo& func_info,
                          GuestFunction* function) {
  // Copy the current oaknut instruction-buffer into the code-cache
  void* new_execute_address;
  void* new_write_address;

  assert_true(func_info.code_size.total == static_cast<size_t>(offset()));

  if (function) {
    code_cache_->PlaceGuestCode(function->address(), assembly_buffer.data(),
                                func_info, function, new_execute_address,
                                new_write_address);
  } else {
    code_cache_->PlaceHostCode(0, assembly_buffer.data(), func_info,
                               new_execute_address, new_write_address);
  }

  // Reset the oaknut instruction-buffer
  assembly_buffer.clear();
  label_lookup_.clear();

  return new_execute_address;
}

void A64Emitter::EmitBtiJc() { dw(0xD503241F); }

bool A64Emitter::Emit(HIRBuilder* builder, EmitFunctionInfo& func_info) {
  oaknut::Label epilog_label;
  epilog_label_ = &epilog_label;

  // Calculate stack size. We need to align things to their natural sizes.
  // This could be much better (sort by type/etc).
  auto locals = builder->locals();
  size_t stack_offset = StackLayout::GUEST_STACK_SIZE;
  for (auto it = locals.begin(); it != locals.end(); ++it) {
    auto slot = *it;
    size_t type_size = GetTypeSize(slot->type);

    // Align to natural size.
    stack_offset = xe::align(stack_offset, type_size);
    slot->set_constant((uint32_t)stack_offset);
    stack_offset += type_size;
  }

  // Ensure 16b alignment.
  stack_offset -= StackLayout::GUEST_STACK_SIZE;
  stack_offset = xe::align(stack_offset, static_cast<size_t>(16));

  struct _code_offsets {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  code_offsets.prolog = offset();

  // Function prolog.
  // Must be 16b aligned.
  // Windows is very strict about the form of this and the epilog:
  // https://docs.microsoft.com/en-us/cpp/build/prolog-and-epilog?view=vs-2017
  // IMPORTANT: any changes to the prolog must be kept in sync with
  //     A64CodeCache, which dynamically generates exception information.
  //     Adding or changing anything here must be matched!
  const size_t stack_size = StackLayout::GUEST_STACK_SIZE + stack_offset;
  assert_true(stack_size % 16 == 0);
  func_info.stack_size = stack_size;
  stack_size_ = stack_size;

  EmitBtiJc();

  STP(X29, X30, SP, PRE_INDEXED, -16);
  MOV(X29, SP);

  AdjustStackPointer(*this, stack_size, false);

  code_offsets.prolog_stack_alloc = offset();
  code_offsets.body = offset();

  STR(GetContextReg(), SP, StackLayout::GUEST_CTX_HOME);
  STR(X0, SP, StackLayout::GUEST_RET_ADDR);
  STR(XZR, SP, StackLayout::GUEST_CALL_RET_ADDR);

  // Safe now to do some tracing.
  if (debug_info_flags_ & DebugInfoFlags::kDebugInfoTraceFunctions) {
    // We require 32-bit addresses.
    assert_true(uint64_t(trace_data_->header()) < UINT_MAX);
    auto trace_header = trace_data_->header();

    // Call count.
    MOV(W0, 1);
    MOV(X5, reinterpret_cast<uintptr_t>(
                low_address(&trace_header->function_call_count)));
    LDADDAL(X0, X0, X5);

    // Get call history slot.
    static_assert(FunctionTraceData::kFunctionCallerHistoryCount == 4,
                  "bitmask depends on count");
    LDR(X0, X5);
    AND(W0, W0, 0b00000011);

    // Record call history value into slot (guest addr in W1).
    MOV(X5, reinterpret_cast<uintptr_t>(
                low_address(&trace_header->function_caller_history)));
    STR(W1, X5, X0, oaknut::IndexExt::LSL, 2);

    // Calling thread. Load X0 with thread ID.
    EmitGetCurrentThreadId();
    MOV(W5, 1);
    LSL(W0, W5, W0);

    MOV(X5, reinterpret_cast<uintptr_t>(
                low_address(&trace_header->function_thread_use)));
    LDSET(W0, WZR, X5);
  }

  // Load membase.
  LDR(GetMembaseReg(), GetContextReg(),
      offsetof(ppc::PPCContext, virtual_membase));

  // Body.
  auto block = builder->first_block();
  [[maybe_unused]] int block_count = 0;
  while (block) {
    // Mark block labels.
    auto label = block->label_head;
    while (label) {
      l(label_lookup_[label->name]);
      label = label->next;
    }

    // Process instructions.
    const Instr* instr = block->instr_head;
    [[maybe_unused]] int instr_count = 0;
    while (instr) {
      const Instr* new_tail = instr;
      if (!SelectSequence(this, instr, &new_tail)) {
        // No sequence found!
        // NOTE: If you encounter this after adding a new instruction, do a full
        // rebuild!
        assert_always();
        XELOGE("Unable to process HIR opcode {}", instr->opcode->name);
        break;
      }
      instr = new_tail;
    }

    block = block->next;
  }

  // Function epilog.
  l(epilog_label);
  epilog_label_ = nullptr;
  EmitTraceUserCallReturn();
  LDR(GetContextReg(), SP, StackLayout::GUEST_CTX_HOME);

  code_offsets.epilog = offset();

  AdjustStackPointer(*this, stack_size, true);

  MOV(SP, X29);
  LDP(X29, X30, SP, POST_INDEXED, 16);

  RET();

  code_offsets.tail = offset();

  if (cvars::emit_source_annotations) {
    NOP();
    NOP();
    NOP();
    NOP();
    NOP();
  }

  assert_zero(code_offsets.prolog);
  func_info.code_size.total = offset();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = offset() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;

  return true;
}

void A64Emitter::MarkSourceOffset(const Instr* i) {
  auto entry = source_map_arena_.Alloc<SourceMapEntry>();
  entry->guest_address = static_cast<uint32_t>(i->src1.offset);
  entry->hir_offset = uint32_t(i->block->ordinal << 16) | i->ordinal;
  entry->code_offset = static_cast<uint32_t>(offset());

  if (cvars::emit_source_annotations) {
    NOP();
    NOP();
    MOV(X0, entry->guest_address);
    NOP();
    NOP();
  }

  if (debug_info_flags_ & DebugInfoFlags::kDebugInfoTraceFunctionCoverage) {
    const uint32_t instruction_index =
        (entry->guest_address - trace_data_->start_address()) / 4;
    MOV(X0, 1);
    MOV(X1, reinterpret_cast<uintptr_t>(
                low_address(trace_data_->instruction_execute_counts() +
                            instruction_index * 8)));
    LDADDAL(X0, ZR, X1);
  }
}

void A64Emitter::EmitGetCurrentThreadId() {
  // X27 must point to context. We could fetch from the stack if needed.
  LDRH(W0, GetContextReg(), offsetof(ppc::PPCContext, thread_id));
}

void A64Emitter::EmitTraceUserCallReturn() {}

void A64Emitter::DebugBreak() { BRK(0xF000); }

uint64_t TrapDebugPrint(void* raw_context, uint64_t address) {
  auto thread_state = *reinterpret_cast<ThreadState**>(raw_context);
  uint32_t str_ptr = uint32_t(thread_state->context()->r[3]);
  // uint16_t str_len = uint16_t(thread_state->context()->r[4]);
  auto str = thread_state->memory()->TranslateVirtual<const char*>(str_ptr);
  // TODO(benvanik): truncate to length?
  XELOGD("(DebugPrint) {}", str);

  if (cvars::debugprint_trap_log) {
    debugging::DebugPrint("(DebugPrint) {}", str);
  }

  return 0;
}

uint64_t TrapLogRegs(void* raw_context, uint64_t address) {
  static volatile int32_t log_count = 0;
  if (xe::atomic_inc(&log_count) > 8) {
    return 0;
  }
  auto guest_context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!guest_context) {
    return 0;
  }
  auto thread_state = guest_context->thread_state;
  XELOGI(
      "TraceOnInstruction 0x{:08X}: r3=0x{:016X} r4=0x{:016X} r11=0x{:016X} "
      "r30=0x{:016X} r31=0x{:016X} lr=0x{:016X} ctr=0x{:016X}",
      static_cast<uint32_t>(cvars::break_on_instruction), guest_context->r[3],
      guest_context->r[4], guest_context->r[11], guest_context->r[30],
      guest_context->r[31], guest_context->lr, guest_context->ctr);
  if (thread_state) {
    auto memory = thread_state->memory();
    if (memory) {
      auto page_access_to_string = [](xe::memory::PageAccess access) {
        switch (access) {
          case xe::memory::PageAccess::kNoAccess:
            return "no-access";
          case xe::memory::PageAccess::kReadOnly:
            return "read-only";
          case xe::memory::PageAccess::kReadWrite:
            return "read-write";
          case xe::memory::PageAccess::kExecuteReadOnly:
            return "exec-read";
          case xe::memory::PageAccess::kExecuteReadWrite:
            return "exec-read-write";
        }
        return "unknown";
      };
      auto heap_type_to_string = [](HeapType type) {
        switch (type) {
          case HeapType::kGuestVirtual:
            return "guest-virtual";
          case HeapType::kGuestXex:
            return "guest-xex";
          case HeapType::kGuestPhysical:
            return "guest-physical";
          case HeapType::kHostPhysical:
            return "host-physical";
        }
        return "unknown";
      };
      auto can_read_guest = [&](uint32_t addr) -> bool {
        if (!addr) {
          return false;
        }
        auto* heap = memory->LookupHeap(addr);
        if (!heap) {
          return false;
        }
        return heap->QueryRangeAccess(addr, addr) !=
               xe::memory::PageAccess::kNoAccess;
      };
      auto log_guest_bytes = [&](uint32_t addr, const char* label) {
        if (!can_read_guest(addr)) {
          auto* heap = memory->LookupHeap(addr);
          XELOGI(
              "TraceOnInstruction {}: addr=0x{:08X} unreadable heap={} "
              "access={}",
              label, addr,
              heap ? heap_type_to_string(heap->heap_type()) : "none",
              heap ? page_access_to_string(heap->QueryRangeAccess(addr, addr))
                   : "no-access");
          return;
        }
        const auto* heap = memory->LookupHeap(addr);
        const uint8_t* host_ptr = nullptr;
        if (heap && heap->heap_type() == HeapType::kGuestPhysical) {
          uint32_t physical_address = memory->GetPhysicalAddress(addr);
          host_ptr = memory->TranslatePhysical(physical_address);
        } else {
          host_ptr = memory->TranslateVirtual<const uint8_t*>(addr);
        }
        if (!host_ptr) {
          XELOGI("TraceOnInstruction {}: addr=0x{:08X} null", label, addr);
          return;
        }
        uint8_t bytes[16] = {};
        std::memcpy(bytes, host_ptr, sizeof(bytes));
        char ascii[sizeof(bytes) + 1] = {};
        for (size_t i = 0; i < sizeof(bytes); ++i) {
          uint8_t ch = bytes[i];
          ascii[i] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '.';
        }
        XELOGI(
            "TraceOnInstruction {}: addr=0x{:08X} {:02X} {:02X} {:02X} "
            "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
            "{:02X} {:02X} {:02X} {:02X} {:02X} ascii={}",
            label, addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
            bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
            bytes[11], bytes[12], bytes[13], bytes[14], bytes[15], ascii);
      };
      auto log_guest_string = [&](uint32_t addr, const char* label) {
        if (!can_read_guest(addr)) {
          return;
        }
        const uint8_t* ptr = memory->TranslateVirtual<const uint8_t*>(addr);
        if (!ptr) {
          return;
        }
        char buffer[129] = {};
        size_t len = 0;
        for (; len < sizeof(buffer) - 1; ++len) {
          char ch = static_cast<char>(ptr[len]);
          if (!ch) {
            break;
          }
          if (!std::isprint(static_cast<unsigned char>(ch))) {
            return;
          }
          buffer[len] = ch;
        }
        if (len > 0) {
          XELOGI("TraceOnInstruction {}: {}", label, buffer);
        }
      };
      const uint32_t guest_address = static_cast<uint32_t>(guest_context->r[4]);
      const auto* heap = memory->LookupHeap(guest_address);
      if (heap) {
        const uint8_t* host_ptr = nullptr;
        if (heap->heap_type() == HeapType::kGuestPhysical) {
          uint32_t physical_address = memory->GetPhysicalAddress(guest_address);
          host_ptr = memory->TranslatePhysical(physical_address);
        } else {
          host_ptr = memory->TranslateVirtual<const uint8_t*>(guest_address);
        }
        if (host_ptr) {
          uint8_t bytes[16] = {};
          std::memcpy(bytes, host_ptr, sizeof(bytes));
          char ascii[sizeof(bytes) + 1] = {};
          for (size_t i = 0; i < sizeof(bytes); ++i) {
            uint8_t ch = bytes[i];
            ascii[i] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '.';
          }
          XELOGI(
              "TraceOnInstruction mem[r4]=0x{:08X}: {:02X} {:02X} {:02X} "
              "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
              "{:02X} {:02X} {:02X} {:02X} {:02X} ascii={}",
              guest_address, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
              bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
              bytes[11], bytes[12], bytes[13], bytes[14], bytes[15], ascii);
        }
      }

      auto read_u32 = [&](uint32_t addr, uint32_t* out) -> bool {
        if (!can_read_guest(addr)) {
          return false;
        }
        const auto* heap = memory->LookupHeap(addr);
        if (heap->heap_type() == HeapType::kGuestPhysical) {
          uint32_t physical_address = memory->GetPhysicalAddress(addr);
          auto ptr =
              memory->TranslatePhysical<const uint32_t*>(physical_address);
          if (!ptr) {
            return false;
          }
          *out = xe::load_and_swap<uint32_t>(ptr);
          return true;
        }
        auto ptr = memory->TranslateVirtual<const uint32_t*>(addr);
        if (!ptr) {
          return false;
        }
        *out = xe::load_and_swap<uint32_t>(ptr);
        return true;
      };

      const uint32_t trace_pc =
          static_cast<uint32_t>(cvars::break_on_instruction);
      auto log_trace_instr = [&](uint32_t pc, const char* label) {
        if (!can_read_guest(pc)) {
          auto* heap = memory->LookupHeap(pc);
          XELOGI(
              "TraceOnInstruction {}: pc=0x{:08X} unreadable heap={} "
              "access={}",
              label, pc, heap ? heap_type_to_string(heap->heap_type()) : "none",
              heap ? page_access_to_string(heap->QueryRangeAccess(pc, pc))
                   : "no-access");
          return;
        }
        uint32_t instr = 0;
        if (!read_u32(pc, &instr)) {
          XELOGI("TraceOnInstruction {}: pc=0x{:08X} unreadable", label, pc);
          return;
        }
        xe::StringBuffer disasm;
        if (cpu::ppc::DisasmPPC(pc, instr, &disasm)) {
          XELOGI("TraceOnInstruction {}: pc=0x{:08X} instr=0x{:08X} {}", label,
                 pc, instr, disasm.to_string_view());
        } else {
          XELOGI("TraceOnInstruction {}: pc=0x{:08X} instr=0x{:08X}", label, pc,
                 instr);
        }
      };
      if (trace_pc) {
        log_trace_instr(trace_pc - 4, "target-4");
        log_trace_instr(trace_pc, "target");
        log_trace_instr(trace_pc + 4, "target+4");
      }
      if (memory->LookupHeap(trace_pc)) {
        for (int offset = -4; offset <= 4; ++offset) {
          uint32_t pc = trace_pc + offset * 4;
          if (!memory->LookupHeap(pc)) {
            continue;
          }
          uint32_t instr = 0;
          if (!read_u32(pc, &instr)) {
            XELOGI("TraceOnInstruction window: pc=0x{:08X} unreadable", pc);
            continue;
          }
          xe::StringBuffer disasm_window;
          if (cpu::ppc::DisasmPPC(pc, instr, &disasm_window)) {
            XELOGI("TraceOnInstruction window: pc=0x{:08X} instr=0x{:08X} {}",
                   pc, instr, disasm_window.to_string_view());
          } else {
            XELOGI("TraceOnInstruction window: pc=0x{:08X} instr=0x{:08X}", pc,
                   instr);
          }
        }
      }

      const uint32_t obj_address = static_cast<uint32_t>(guest_context->r[3]);
      if (!obj_address) {
        XELOGI("TraceOnInstruction r3 fields: base=0x00000000");
      } else {
        const auto* obj_heap = memory->LookupHeap(obj_address);
        if (obj_heap) {
          uint32_t value_4 = 0;
          uint32_t value_8 = 0;
          uint32_t value_c = 0;
          uint32_t value_20 = 0;
          uint32_t value_470 = 0;
          bool have_any = false;
          have_any |= read_u32(obj_address + 0x4, &value_4);
          have_any |= read_u32(obj_address + 0x8, &value_8);
          have_any |= read_u32(obj_address + 0xC, &value_c);
          have_any |= read_u32(obj_address + 0x20, &value_20);
          have_any |= read_u32(obj_address + 0x470, &value_470);
          if (have_any) {
            XELOGI(
                "TraceOnInstruction r3 fields: base=0x{:08X} +0x4=0x{:08X} "
                "+0x8=0x{:08X} +0xC=0x{:08X} +0x20=0x{:08X} +0x470=0x{:08X}",
                obj_address, value_4, value_8, value_c, value_20, value_470);
            if (value_20) {
              log_guest_bytes(value_20, "r3+0x20");
            }
            if (value_470) {
              log_guest_bytes(value_470, "r3+0x470");
            }
          } else {
            XELOGI("TraceOnInstruction r3 fields: base=0x{:08X} unmapped",
                   obj_address);
          }
        } else {
          XELOGI("TraceOnInstruction r3 fields: base=0x{:08X} heap=null",
                 obj_address);
        }
        log_guest_string(obj_address, "r3 string");
      }

      const uint32_t r31_address = static_cast<uint32_t>(guest_context->r[31]);
      if (!r31_address) {
        XELOGI("TraceOnInstruction r31 fields: base=0x00000000");
      } else {
        const auto* r31_heap = memory->LookupHeap(r31_address);
        if (r31_heap) {
          uint32_t value_4 = 0;
          uint32_t value_8 = 0;
          uint32_t value_c = 0;
          uint32_t value_20 = 0;
          uint32_t value_470 = 0;
          bool have_any = false;
          have_any |= read_u32(r31_address + 0x4, &value_4);
          have_any |= read_u32(r31_address + 0x8, &value_8);
          have_any |= read_u32(r31_address + 0xC, &value_c);
          have_any |= read_u32(r31_address + 0x20, &value_20);
          have_any |= read_u32(r31_address + 0x470, &value_470);
          if (have_any) {
            XELOGI(
                "TraceOnInstruction r31 fields: base=0x{:08X} +0x4=0x{:08X} "
                "+0x8=0x{:08X} +0xC=0x{:08X} +0x20=0x{:08X} +0x470=0x{:08X}",
                r31_address, value_4, value_8, value_c, value_20, value_470);
            if (value_20) {
              log_guest_bytes(value_20, "r31+0x20");
            }
            if (value_470) {
              log_guest_bytes(value_470, "r31+0x470");
            }
          } else {
            XELOGI("TraceOnInstruction r31 fields: base=0x{:08X} unmapped",
                   r31_address);
          }
        } else {
          XELOGI("TraceOnInstruction r31 fields: base=0x{:08X} heap=null",
                 r31_address);
        }
      }
    }
  }
  return 0;
}

uint64_t TrapDebugBreak(void* raw_context, uint64_t address) {
  [[maybe_unused]] auto thread_state =
      *reinterpret_cast<ThreadState**>(raw_context);
  XELOGE("tw/td forced trap hit! This should be a crash!");
  if (cvars::break_on_debugbreak) {
    xe::debugging::Break();
  }
  return 0;
}

void A64Emitter::Trap(uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26:
      // 0x0FE00014 is a 'debug print' where r3 = buffer r4 = length
      CallNative(TrapDebugPrint, 0);
      break;
    case 27:
      CallNative(TrapLogRegs, 0);
      break;
    case 0:
    case 22:
      // Always trap?
      // TODO(benvanik): post software interrupt to debugger.
      CallNative(TrapDebugBreak, 0);
      break;
    case 25:
      // ?
      break;
    default:
      XELOGW("Unknown trap type {}", trap_type);
      BRK(0xF000);
      break;
  }
}

void A64Emitter::UnimplementedInstr(const hir::Instr* i) {
  // TODO(benvanik): notify debugger.
  BRK(0xF000);
  assert_always();
}

// This is used by the A64ThunkEmitter's ResolveFunctionThunk.
uint64_t ResolveFunction(void* raw_context, uint64_t target_address) {
  auto thread_state = *reinterpret_cast<ThreadState**>(raw_context);
  auto guest_context = thread_state->context();

  uint32_t guest_address;

  // Check if this is a 64-bit host address that needs to be mapped back to
  // guest address
  if (target_address > 0xFFFFFFFF) {
    // Precise guard: if target_address is within the PPCContext, this is a bug.
    auto ctx_ptr = reinterpret_cast<uint64_t>(thread_state->context());
    if (target_address >= ctx_ptr &&
        target_address < ctx_ptr + sizeof(ppc::PPCContext)) {
      XELOGE(
          "ResolveFunction: target_address 0x{:016X} is within PPCContext "
          "[0x{:016X}, 0x{:016X})",
          target_address, ctx_ptr, ctx_ptr + sizeof(ppc::PPCContext));
      XELOGE(
          "ResolveFunction: The target register contains a context pointer "
          "instead of a function address");
      return 0;
    }

    // Try to find a function that contains this host address
    auto code_cache = static_cast<A64CodeCache*>(
        thread_state->processor()->backend()->code_cache());
    auto guest_function = code_cache->LookupFunction(target_address);
    if (guest_function) {
      guest_address =
          guest_function->MapMachineCodeToGuestAddress(target_address);
    } else {
      // This might be a guest memory address stored in 64-bit form
      // Extract the lower 32 bits as the potential guest address
      uint32_t potential_guest = static_cast<uint32_t>(target_address);
      guest_address = potential_guest;
    }
  } else {
    // Normal 32-bit guest address
    guest_address = static_cast<uint32_t>(target_address);
  }

  // Xbox 360 guest addresses can be in these ranges:
  // 0x00000000-0x3FFFFFFF: v00000000 heap (virtual)
  // 0x40000000-0x7EFFFFFF: v40000000 heap (virtual)
  // 0x70000000-0x7F000000: Thread stacks
  // 0x80000000-0x8FFFFFFF: v80000000 heap (XEX)
  // 0x90000000-0x9FFFFFFF: v90000000 heap (XEX)
  // 0xA0000000-0xBFFFFFFF: vA0000000 heap (physical)
  // 0xC0000000-0xDFFFFFFF: vC0000000 heap (physical)
  // 0xE0000000-0xFFCFFFFF: vE0000000 heap (physical)

  // Most executable code should be in XEX ranges (0x80000000-0x9FFFFFFF)
  // but allow other ranges as they may contain valid code
  if (guest_address == 0) {
    XELOGE("ResolveFunction: guest_address is 0! This should not happen");
    return 0;
  }

  auto fn = thread_state->processor()->ResolveFunction(guest_address);
  if (!fn) {
    XELOGE(
        "ResolveFunction: Failed to resolve function at guest address 0x{:08X}",
        guest_address);
    XELOGE("ResolveFunction: Original target_address was 0x{:016X}",
           target_address);
    if (ShouldLogResolveFailure()) {
      const uint32_t lr_guest = static_cast<uint32_t>(guest_context->lr);
      XELOGI(
          "ResolveFunction: lr=0x{:016X} ctr=0x{:016X} thread_id={} "
          "target_is_host={} guest_address=0x{:08X}",
          guest_context->lr, guest_context->ctr, guest_context->thread_id,
          target_address > 0xFFFFFFFF, guest_address);
      auto log_modules_for_address = [&](uint32_t address, const char* label) {
        bool found = false;
        for (auto* module : thread_state->processor()->GetModules()) {
          if (!module) {
            continue;
          }
          if (module->ContainsAddress(address)) {
            XELOGI("ResolveFunction: {} module '{}' contains 0x{:08X}", label,
                   module->name(), address);
            found = true;
          }
        }
        if (!found) {
          XELOGI("ResolveFunction: {} no module contains 0x{:08X}", label,
                 address);
        }
      };
      log_modules_for_address(lr_guest, "lr");
      log_modules_for_address(guest_address, "guest");

      auto lr_functions =
          thread_state->processor()->FindFunctionsWithAddress(lr_guest);
      if (lr_functions.empty()) {
        XELOGI("ResolveFunction: no resolved function covers LR 0x{:08X}",
               lr_guest);
      } else {
        const auto* fn = lr_functions.front();
        XELOGI("ResolveFunction: LR function {} [0x{:08X},0x{:08X}) name='{}'",
               lr_functions.size(), fn->address(), fn->end_address(),
               fn->name());
      }

      auto* memory = thread_state->memory();
      if (!memory) {
        XELOGI("ResolveFunction: no Memory available for guest dump");
      } else if (!memory->LookupHeap(guest_address)) {
        XELOGI(
            "ResolveFunction: guest_address 0x{:08X} not in any heap for dump",
            guest_address);
      } else {
        const uint8_t* data =
            memory->TranslateVirtual<const uint8_t*>(guest_address);
        std::array<uint8_t, 16> bytes = {};
        std::memcpy(bytes.data(), data, bytes.size());
        XELOGI(
            "ResolveFunction: guest[0x{:08X}] = {:02X} {:02X} {:02X} {:02X} "
            "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
            "{:02X} {:02X} {:02X} {:02X}",
            guest_address, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
            bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
            bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
      }
    }

    // This can happen if the target address doesn't point to a valid function
    // Return 0 to indicate failure - the calling code should handle this
    return 0;
  }

  auto a64_fn = static_cast<A64Function*>(fn);
  uint64_t addr = reinterpret_cast<uint64_t>(a64_fn->machine_code());
  if (!a64_fn->machine_code()) {
    XELOGE(
        "ResolveFunction: Function at guest address 0x{:08X} has no machine "
        "code",
        guest_address);
    return 0;
  }

  return addr;
}

void A64Emitter::Call(const hir::Instr* instr, GuestFunction* function) {
  assert_not_null(function);
  auto fn = static_cast<A64Function*>(function);
  // Resolve address to the function to call and store in X16.
  if (fn->machine_code()) {
    // TODO(benvanik): is it worth it to do this? It removes the need for
    // a ResolveFunction call, but makes the table less useful.
#if XE_A64_INDIRECTION_64BIT
    MOV(X16, reinterpret_cast<uint64_t>(fn->machine_code()));
#else
    assert_zero(uint64_t(fn->machine_code()) & 0xFFFFFFFF00000000);
    MOV(X16, uint32_t(uint64_t(fn->machine_code())));
#endif
  } else if (code_cache_->has_indirection_table()) {
    // Load the pointer to the indirection table maintained in A64CodeCache.
    // The target dword will either contain the address of the generated code
    // or a thunk to ResolveAddress.
    MOV(W17, function->address());
#if XE_A64_INDIRECTION_64BIT
    // ARM64 platforms with 64-bit indirection entries must compute an offset.
    MOV(X16, code_cache_->indirection_table_base_bias());
    LSL(X15, X17, 1);
    ADD(X16, X16, X15);
    LDR(X16, X16);
#else
    // Other platforms use 32-bit addresses mapped at guest address space.
    if (code_cache_->indirection_table_base_address() ==
        A64CodeCache::kIndirectionTableBase) {
      LDR(W16, X17);
    } else {
      MOV(W16, static_cast<uint32_t>(A64CodeCache::kIndirectionTableBase));
      SUB(W17, W17, W16);
      MOV(X16, code_cache_->indirection_table_base_address());
      ADD(X16, X16, W17, UXTW);
      LDR(W16, X16);
    }
#endif
  } else {
    XELOGE("A64 indirection table missing; ResolveFunction fallback removed");
    BRK(0xF000);
    B(epilog_label());
  }

  // Actually jump/call to X16.
  if (instr->flags & hir::CALL_TAIL) {
    // Since we skip the prolog we need to mark the return here.
    EmitTraceUserCallReturn();

    // Pass the callers return address over.
    LDR(X0, SP, StackLayout::GUEST_RET_ADDR);

    AdjustStackPointer(*this, stack_size(), true);

    MOV(SP, X29);
    LDP(X29, X30, SP, POST_INDEXED, 16);

    BR(X16);
  } else {
    // Return address is from the previous SET_RETURN_ADDRESS.
    LDR(X0, SP, StackLayout::GUEST_CALL_RET_ADDR);

    BLR(X16);
  }
}

void A64Emitter::CallIndirect(const hir::Instr* instr,
                              const oaknut::XReg& reg) {
  // Check if return.
  if (instr->flags & hir::CALL_POSSIBLE_RETURN) {
    LDR(W16, SP, StackLayout::GUEST_RET_ADDR);
    CMP(reg.toW(), W16);
    B(oaknut::Cond::EQ, epilog_label());
  }

  // Load the pointer to the indirection table maintained in A64CodeCache.
  // The target dword will either contain the address of the generated code
  // or a thunk to ResolveAddress.
  if (code_cache_->has_indirection_table()) {
    if (reg.toW().index() != W17.index()) {
      MOV(W17, reg.toW());
    }
#if XE_A64_INDIRECTION_64BIT
    // ARM64 platforms with 64-bit indirection entries must compute an offset.
    MOV(X16, code_cache_->indirection_table_base_bias());
    LSL(X15, X17, 1);
    ADD(X16, X16, X15);
    LDR(X16, X16);
#else
    // Other platforms use 32-bit addresses mapped at guest address space.
    if (code_cache_->indirection_table_base_address() ==
        A64CodeCache::kIndirectionTableBase) {
      LDR(W16, X17);
    } else {
      MOV(W16, static_cast<uint32_t>(A64CodeCache::kIndirectionTableBase));
      SUB(W17, W17, W16);
      MOV(X16, code_cache_->indirection_table_base_address());
      ADD(X16, X16, W17, UXTW);
      LDR(W16, X16);
    }
#endif
  } else {
    XELOGE("A64 indirection table missing; ResolveFunction fallback removed");
    BRK(0xF000);
    B(epilog_label());
  }

  // Actually jump/call to X16.
  if (instr->flags & hir::CALL_TAIL) {
    // Since we skip the prolog we need to mark the return here.
    EmitTraceUserCallReturn();

    // Pass the callers return address over.
    LDR(X0, SP, StackLayout::GUEST_RET_ADDR);

    AdjustStackPointer(*this, stack_size(), true);

    MOV(SP, X29);
    LDP(X29, X30, SP, POST_INDEXED, 16);

    BR(X16);
  } else {
    // Return address is from the previous SET_RETURN_ADDRESS.
    LDR(X0, SP, StackLayout::GUEST_CALL_RET_ADDR);

    BLR(X16);
  }
}

uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {
  auto function = reinterpret_cast<Function*>(function_ptr);
  if (cvars::log_undefined_extern_args &&
      function->name() == "XeKeysConsolePrivateKeySign") {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
      XELOGI(
          "Undefined extern {} args: r3={:016X} r4={:016X} r5={:016X} "
          "r6={:016X} r7={:016X} r8={:016X} r9={:016X} r10={:016X}",
          function->name(), context->r[3], context->r[4], context->r[5],
          context->r[6], context->r[7], context->r[8], context->r[9],
          context->r[10]);
    }
  }
  if (!cvars::ignore_undefined_externs) {
    xe::FatalError(fmt::format("undefined extern call to {:08X} {}",
                               function->address(), function->name().c_str()));
  } else {
    XELOGE("undefined extern call to {:08X} {}", function->address(),
           function->name());
  }
  return 0;
}
void A64Emitter::CallExtern(const hir::Instr* instr, const Function* function) {
  bool undefined = true;
  if (function->behavior() == Function::Behavior::kBuiltin) {
    auto builtin_function = static_cast<const BuiltinFunction*>(function);
    if (builtin_function->handler()) {
      undefined = false;
      // x0 = target function
      // x1 = arg0
      // x2 = arg1
      // x3 = arg2
      MOV(X0, reinterpret_cast<uint64_t>(builtin_function->handler()));
      MOV(X1, reinterpret_cast<uint64_t>(builtin_function->arg0()));
      MOV(X2, reinterpret_cast<uint64_t>(builtin_function->arg1()));

      auto thunk = backend()->guest_to_host_thunk();
      MOV(X16, reinterpret_cast<uint64_t>(thunk));
      BLR(X16);

      // x0 = host return
    }
  } else if (function->behavior() == Function::Behavior::kExtern) {
    auto extern_function = static_cast<const GuestFunction*>(function);
    if (extern_function->extern_handler()) {
      undefined = false;
      // x0 = target function
      // x1 = arg0
      // x2 = arg1
      // x3 = arg2
      MOV(X0, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
      LDR(X1, GetContextReg(), offsetof(ppc::PPCContext, kernel_state));

      auto thunk = backend()->guest_to_host_thunk();
      MOV(X16, reinterpret_cast<uint64_t>(thunk));
      BLR(X16);

      // x0 = host return
    }
  }
  if (undefined) {
    CallNative(UndefinedCallExtern, reinterpret_cast<uint64_t>(function));
  }
}

void A64Emitter::CallNative(void* fn) { CallNativeSafe(fn); }

void A64Emitter::CallNative(uint64_t (*fn)(void* raw_context)) {
  CallNativeSafe(reinterpret_cast<void*>(fn));
}

void A64Emitter::CallNative(uint64_t (*fn)(void* raw_context, uint64_t arg0)) {
  CallNativeSafe(reinterpret_cast<void*>(fn));
}

void A64Emitter::CallNative(uint64_t (*fn)(void* raw_context, uint64_t arg0),
                            uint64_t arg0) {
  MOV(GetNativeParam(0), arg0);
  CallNativeSafe(reinterpret_cast<void*>(fn));
}

void A64Emitter::CallNativeSafe(void* fn) {
  // X0 = target function
  // X1 = arg0
  // X2 = arg1
  // X3 = arg2
  auto thunk = backend()->guest_to_host_thunk();

  MOV(X0, reinterpret_cast<uint64_t>(fn));

  MOV(X16, reinterpret_cast<uint64_t>(thunk));
  BLR(X16);

  // X0 = host return
}

void A64Emitter::SetReturnAddress(uint64_t value) {
  MOV(X0, value);
  STR(X0, SP, StackLayout::GUEST_CALL_RET_ADDR);
}

oaknut::XReg A64Emitter::GetNativeParam(uint32_t param) {
  if (param == 0)
    return X1;
  else if (param == 1)
    return X2;
  else if (param == 2)
    return X3;

  assert_always();
  return X3;
}

// Important: If you change these, you must update the thunks in a64_backend.cc!
oaknut::XReg A64Emitter::GetContextReg() { return X27; }
oaknut::XReg A64Emitter::GetMembaseReg() { return X28; }

void A64Emitter::ReloadContext() {
  LDR(GetContextReg(), SP, StackLayout::GUEST_CTX_HOME);
}

void A64Emitter::ReloadMembase() {
  LDR(GetMembaseReg(), GetContextReg(),
      offsetof(ppc::PPCContext, virtual_membase));
}

bool A64Emitter::ConstantFitsIn32Reg(uint64_t v) {
  if ((v & ~0x7FFFFFFF) == 0) {
    // Fits under 31 bits, so just load using normal mov.
    return true;
  } else if ((v & ~0x7FFFFFFFUL) == ~0x7FFFFFFFUL) {
    // Negative number that fits in 32bits.
    return true;
  }
  return false;
}

void A64Emitter::MovMem64(const oaknut::XRegSp& addr, intptr_t offset,
                          uint64_t v) {
  if (v == 0) {
    STR(XZR, addr, offset);
  } else if (!(v >> 32)) {
    // All high bits are zero, 32-bit MOV
    MOV(W0, static_cast<uint32_t>(v));
    STR(X0, addr, offset);
  } else {
    // 64bit number that needs double movs.
    MOV(X0, v);
    STR(X0, addr, offset);
  }
}

static const vec128_t v_consts[] = {
    /* VZero                */ vec128f(0.0f),
    /* VOnePD               */ vec128d(1.0),
    /* VNegativeOne         */ vec128f(-1.0f, -1.0f, -1.0f, -1.0f),
    /* VFFFF                */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VMaskX16Y16          */
    vec128i(0x0000FFFFu, 0xFFFF0000u, 0x00000000u, 0x00000000u),
    /* VFlipX16Y16          */
    vec128i(0x00008000u, 0x00000000u, 0x00000000u, 0x00000000u),
    /* VFixX16Y16           */ vec128f(-32768.0f, 0.0f, 0.0f, 0.0f),
    /* VNormalizeX16Y16     */
    vec128f(1.0f / 32767.0f, 1.0f / (32767.0f * 65536.0f), 0.0f, 0.0f),
    /* V0001                */ vec128f(0.0f, 0.0f, 0.0f, 1.0f),
    /* V3301                */ vec128f(3.0f, 3.0f, 0.0f, 1.0f),
    /* V3331                */ vec128f(3.0f, 3.0f, 3.0f, 1.0f),
    /* V3333                */ vec128f(3.0f, 3.0f, 3.0f, 3.0f),
    /* VSignMaskPS          */
    vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    /* VSignMaskPD          */
    vec128i(0x00000000u, 0x80000000u, 0x00000000u, 0x80000000u),
    /* VAbsMaskPS           */
    vec128i(0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu),
    /* VAbsMaskPD           */
    vec128i(0xFFFFFFFFu, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu),
    /* VByteSwapMask        */
    vec128i(0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu),
    /* VByteOrderMask       */
    vec128i(0x01000302u, 0x05040706u, 0x09080B0Au, 0x0D0C0F0Eu),
    /* VPermuteControl15    */ vec128b(15),
    /* VPermuteByteMask     */ vec128b(0x1F),
    /* VPackD3DCOLORSat     */ vec128i(0x404000FFu),
    /* VPackD3DCOLOR        */
    // Note: x86 PSHUFB uses 0xFF to zero bytes, ARM TBL uses indices >= 16
    // Keep original 0xFF for consistency, handle in implementation
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0C000408u),
    /* VUnpackD3DCOLOR      */
    vec128i(0xFFFFFF0Eu, 0xFFFFFF0Du, 0xFFFFFF0Cu, 0xFFFFFF0Fu),
    /* VPackFLOAT16_2       */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000302u),
    /* VUnpackFLOAT16_2     */
    vec128i(0x0D0C0F0Eu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VPackFLOAT16_4       */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000302u, 0x05040706u),
    /* VUnpackFLOAT16_4     */
    vec128i(0x09080B0Au, 0x0D0C0F0Eu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VPackSHORT_Min       */ vec128i(0x403F8001u),
    /* VPackSHORT_Max       */ vec128i(0x40407FFFu),
    /* VPackSHORT_2         */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000504u),
    /* VPackSHORT_4         */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x01000504u, 0x09080D0Cu),
    /* VUnpackSHORT_2       */
    vec128i(0xFFFF0F0Eu, 0xFFFF0D0Cu, 0xFFFFFFFFu, 0xFFFFFFFFu),
    /* VUnpackSHORT_4       */
    vec128i(0xFFFF0B0Au, 0xFFFF0908u, 0xFFFF0F0Eu, 0xFFFF0D0Cu),
    /* VUnpackSHORT_Overflow */ vec128i(0x403F8000u),
    /* VPackUINT_2101010_MinUnpacked */
    vec128i(0x403FFE01u, 0x403FFE01u, 0x403FFE01u, 0x40400000u),
    /* VPackUINT_2101010_MaxUnpacked */
    vec128i(0x404001FFu, 0x404001FFu, 0x404001FFu, 0x40400003u),
    /* VPackUINT_2101010_MaskUnpacked */
    vec128i(0x3FFu, 0x3FFu, 0x3FFu, 0x3u),
    /* VPackUINT_2101010_MaskPacked */
    vec128i(0x3FFu, 0x3FFu << 10, 0x3FFu << 20, 0x3u << 30),
    /* VPackUINT_2101010_Shift */ vec128i(0, 10, 20, 30),
    /* VUnpackUINT_2101010_Overflow */ vec128i(0x403FFE00u),
    /* VPackULONG_4202020_MinUnpacked */
    vec128i(0x40380001u, 0x40380001u, 0x40380001u, 0x40400000u),
    /* VPackULONG_4202020_MaxUnpacked */
    vec128i(0x4047FFFFu, 0x4047FFFFu, 0x4047FFFFu, 0x4040000Fu),
    /* VPackULONG_4202020_MaskUnpacked */
    vec128i(0xFFFFFu, 0xFFFFFu, 0xFFFFFu, 0xFu),
    /* VPackULONG_4202020_PermuteXZ */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x0A0908FFu, 0xFF020100u),
    /* VPackULONG_4202020_PermuteYW */
    vec128i(0xFFFFFFFFu, 0xFFFFFFFFu, 0x0CFFFF06u, 0x0504FFFFu),
    /* VUnpackULONG_4202020_Permute */
    vec128i(0xFF0E0D0Cu, 0xFF0B0A09u, 0xFF080F0Eu, 0xFFFFFF0Bu),
    /* VUnpackULONG_4202020_Overflow */ vec128i(0x40380000u),
    /* VOneOver255          */ vec128f(1.0f / 255.0f),
    /* VMaskEvenPI16        */
    vec128i(0x0000FFFFu, 0x0000FFFFu, 0x0000FFFFu, 0x0000FFFFu),
    /* VShiftMaskEvenPI16   */
    vec128i(0x0000000Fu, 0x0000000Fu, 0x0000000Fu, 0x0000000Fu),
    /* VShiftMaskPS         */
    vec128i(0x0000001Fu, 0x0000001Fu, 0x0000001Fu, 0x0000001Fu),
    /* VShiftByteMask       */
    vec128i(0x000000FFu, 0x000000FFu, 0x000000FFu, 0x000000FFu),
    /* VSwapWordMask        */
    vec128i(0x03030303u, 0x03030303u, 0x03030303u, 0x03030303u),
    /* VUnsignedDwordMax    */
    vec128i(0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u),
    /* V255                 */ vec128f(255.0f),
    /* VPI32                */ vec128i(32),
    /* VSignMaskI8          */
    vec128i(0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u),
    /* VSignMaskI16         */
    vec128i(0x80008000u, 0x80008000u, 0x80008000u, 0x80008000u),
    /* VSignMaskI32         */
    vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    /* VSignMaskF32         */
    vec128i(0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u),
    /* VShortMinPS          */ vec128f(SHRT_MIN),
    /* VShortMaxPS          */ vec128f(SHRT_MAX),
    /* VIntMin              */ vec128i(INT_MIN),
    /* VIntMax              */ vec128i(INT_MAX),
    /* VIntMaxPD            */ vec128d(INT_MAX),
    /* VPosIntMinPS         */ vec128f((float)0x80000000u),
    /* VQNaN                */ vec128i(0x7FC00000u),
    /* VInt127              */ vec128i(0x7Fu),
    /* V2To32               */ vec128f(0x1.0p32f),
};

// First location to try and place constants.
[[maybe_unused]] static const uintptr_t kConstDataLocation = 0x20000000;
static const uintptr_t kConstDataSize = sizeof(v_consts);

// Increment the location by this amount for every allocation failure.
[[maybe_unused]] static const uintptr_t kConstDataIncrement = 0x00001000;

// This function places constant data that is used by the emitter later on.
// Only called once and used by multiple instances of the emitter.
//
// TODO(DrChat): This should be placed in the code cache with the code, but
// doing so requires RIP-relative addressing, which is difficult to support
// given the current setup.
uintptr_t A64Emitter::PlaceConstData() {
  void* mem = nullptr;
#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  // macOS ARM64 PAGEZERO blocks low fixed mappings; use OS-chosen addresses.
  mem = memory::AllocFixed(
      nullptr, xe::round_up(kConstDataSize, memory::page_size()),
      memory::AllocationType::kReserveCommit, memory::PageAccess::kReadWrite);
#else
  uint8_t* ptr = reinterpret_cast<uint8_t*>(kConstDataLocation);
  while (!mem) {
    mem = memory::AllocFixed(
        ptr, xe::round_up(kConstDataSize, memory::page_size()),
        memory::AllocationType::kReserveCommit, memory::PageAccess::kReadWrite);

    ptr += kConstDataIncrement;
  }
#endif

#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  // On macOS ARM64, memory is often allocated in high address space
  if (reinterpret_cast<uintptr_t>(mem) & ~0x7FFFFFFF) {
    XELOGD(
        "Const data allocated at high address {:#x}, may cause compatibility "
        "issues",
        reinterpret_cast<uintptr_t>(mem));
    // Continue anyway since we'll handle it later
  }
#else
  // The pointer must not be greater than 31 bits.
  assert_zero(reinterpret_cast<uintptr_t>(mem) & ~0x7FFFFFFF);
#endif
  std::memcpy(mem, v_consts, sizeof(v_consts));
  memory::Protect(mem, kConstDataSize, memory::PageAccess::kReadOnly, nullptr);

  return reinterpret_cast<uintptr_t>(mem);
}

void A64Emitter::FreeConstData(uintptr_t data) {
  memory::DeallocFixed(reinterpret_cast<void*>(data), 0,
                       memory::DeallocationType::kRelease);
}

uintptr_t A64Emitter::GetVConstPtr() const { return backend_->emitter_data(); }

uintptr_t A64Emitter::GetVConstPtr(VConst id) const {
  // Load through fixed constant table setup by PlaceConstData.
  // It's important that the pointer is not signed, as it will be sign-extended.
  return GetVConstPtr() + GetVConstOffset(id);
}

// Attempts to convert an fp32 bit-value into an fp8-immediate value for FMOV
// returns false if the value cannot be represented
// C2.2.3 Modified immediate constants in A64 floating-point instructions
// abcdefgh
//    V
// aBbbbbbc defgh000 00000000 00000000
// B = NOT(b)
static bool f32_to_fimm8(uint32_t u32, oaknut::FImm8& fp8) {
  const uint32_t sign = (u32 >> 31) & 1;
  int32_t exp = ((u32 >> 23) & 0xff) - 127;
  int64_t mantissa = u32 & 0x7fffff;

  // Too many mantissa bits
  if (mantissa & 0x7ffff) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 19;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }

  // exp = (NOT(b):c:d) - 3
  exp = ((exp + 3) & 0b111) ^ 0b100;

  fp8 = oaknut::FImm8(sign, exp, uint8_t(mantissa));
  return true;
}

// Attempts to convert an fp64 bit-value into an fp8-immediate value for FMOV
// returns false if the value cannot be represented
// C2.2.3 Modified immediate constants in A64 floating-point instructions
// abcdefgh
//    V
// aBbbbbbb bbcdefgh 00000000 00000000 00000000 00000000 00000000 00000000
// B = NOT(b)
static bool f64_to_fimm8(uint64_t u64, oaknut::FImm8& fp8) {
  const uint32_t sign = (u64 >> 63) & 1;
  int32_t exp = ((u64 >> 52) & 0x7ff) - 1023;
  int64_t mantissa = u64 & 0xfffffffffffffULL;

  // Too many mantissa bits
  if (mantissa & 0xffffffffffffULL) {
    return false;
  }
  // Too many exp bits
  if (exp < -3 || exp > 4) {
    return false;
  }

  // mantissa = (16 + e:f:g:h) / 16.
  mantissa >>= 48;
  if ((mantissa & 0b1111) != mantissa) {
    return false;
  }

  // exp = (NOT(b):c:d) - 3
  exp = ((exp + 3) & 0b111) ^ 0b100;

  fp8 = oaknut::FImm8(sign, exp, uint8_t(mantissa));
  return true;
}

// Implies possible StashV(0, ...)!
void A64Emitter::LoadConstantV(oaknut::QReg dest, const vec128_t& v) {
  if (!v.low && !v.high) {
    // 0000...
    // MOVI is implemented as a register-rename while EOR(x, x, x) is not
    // https://dougallj.github.io/applecpu/firestorm.html
    MOVI(dest.B16(), 0);
  } else if (v.low == ~uint64_t(0) && v.high == ~uint64_t(0)) {
    // 1111...
    MOVI(dest.B16(), 0xFF);
  } else {
    // Try to figure out some common splat-patterns to utilize MOVI rather than
    // stashing to memory.
    const bool all_same_u8 =
        std::adjacent_find(std::cbegin(v.u8), std::cend(v.u8),
                           std::not_equal_to<>()) == std::cend(v.u8);

    if (all_same_u8) {
      // 0xXX, 0xXX, 0xXX...
      MOVI(dest.B16(), v.u8[0]);
      return;
    }

    const bool all_same_u16 =
        std::adjacent_find(std::cbegin(v.u16), std::cend(v.u16),
                           std::not_equal_to<>()) == std::cend(v.u16);

    if (all_same_u16) {
      if ((v.u16[0] & 0xFF00) == 0) {
        // 0x00XX, 0x00XX, 0x00XX...
        MOVI(dest.H8(), uint8_t(v.u16[0]));
        return;
      } else if ((v.u16[0] & 0x00FF) == 0) {
        // 0xXX00, 0xXX00, 0xXX00...
        MOVI(dest.H8(), uint8_t(v.u16[0] >> 8), oaknut::util::LSL, 8);
        return;
      }
    }

    const bool all_same_u32 =
        std::adjacent_find(std::cbegin(v.u32), std::cend(v.u32),
                           std::not_equal_to<>()) == std::cend(v.u32);

    if (all_same_u32) {
      if ((v.u32[0] & 0x00FFFFFF) == 0) {
        // This is used a lot for certain float-splats and should be checked
        // first before the others
        // 0xXX000000, 0xXX000000, 0xXX000000...
        MOVI(dest.S4(), uint8_t(v.u32[0] >> 24), oaknut::util::LSL, 24);
        return;
      } else if ((v.u32[0] & 0xFFFFFF00) == 0) {
        // 0x000000XX, 0x000000XX, 0x000000XX...
        MOVI(dest.S4(), uint8_t(v.u32[0]));
        return;
      } else if ((v.u32[0] & 0xFFFF00FF) == 0) {
        // 0x0000XX00, 0x0000XX00, 0x0000XX00...
        MOVI(dest.S4(), uint8_t(v.u32[0] >> 8), oaknut::util::LSL, 8);
        return;
      } else if ((v.u32[0] & 0xFF00FFFF) == 0) {
        // 0x00XX0000, 0x00XX0000, 0x00XX0000...
        MOVI(dest.S4(), uint8_t(v.u32[0] >> 16), oaknut::util::LSL, 16);
        return;
      }

      // Try to utilize FMOV if possible
      oaknut::FImm8 fp8(0);
      if (f32_to_fimm8(v.u32[0], fp8)) {
        FMOV(dest.S4(), fp8);
        return;
      }
    }

    // TODO(benvanik): see what other common values are.
    // TODO(benvanik): build constant table - 99% are reused.
    MovMem64(SP, kStashOffset, v.low);
    MovMem64(SP, kStashOffset + 8, v.high);
    LDR(dest, SP, kStashOffset);
  }
}

void A64Emitter::LoadConstantV(oaknut::QReg dest, float v) {
  union {
    float f;
    uint32_t i;
  } x = {v};
  if (!x.i) {
    // +0.0f (but not -0.0f because it may be used to flip the sign via xor).
    MOVI(dest.B16(), 0);
  } else if (x.i == ~uint32_t(0)) {
    // 1111...
    MOVI(dest.B16(), 0xFF);
  } else {
    // TODO(benvanik): see what other common values are.
    // TODO(benvanik): build constant table - 99% are reused.

    // Try to utilize FMOV if possible
    oaknut::FImm8 fp8(0);
    if (f32_to_fimm8(x.i, fp8)) {
      FMOV(dest.toS(), fp8);
      return;
    }

    MOV(W0, x.i);
    FMOV(dest.toS(), W0);
  }
}

void A64Emitter::LoadConstantV(oaknut::QReg dest, double v) {
  union {
    double d;
    uint64_t i;
  } x = {v};
  if (!x.i) {
    // +0.0 (but not -0.0 because it may be used to flip the sign via xor).
    MOVI(dest.toD(), oaknut::RepImm(0));
  } else if (x.i == ~uint64_t(0)) {
    // 1111...
    MOVI(dest.toD(), oaknut::RepImm(0xFF));
  } else {
    // TODO(benvanik): see what other common values are.
    // TODO(benvanik): build constant table - 99% are reused.

    // Try to utilize FMOV if possible
    oaknut::FImm8 fp8(0);
    if (f64_to_fimm8(x.i, fp8)) {
      FMOV(dest.toD(), fp8);
      return;
    }

    MOV(X0, x.i);
    FMOV(dest.toD(), X0);
  }
}

uintptr_t A64Emitter::StashV(int index, const oaknut::QReg& r) {
  // auto addr = ptr[rsp + kStashOffset + (index * 16)];
  // vmovups(addr, r);
  const auto addr = kStashOffset + (index * 16);
  STR(r, SP, addr);
  return addr;
}

uintptr_t A64Emitter::StashConstantV(int index, float v) {
  union {
    float f;
    uint32_t i;
  } x = {v};
  const auto addr = kStashOffset + (index * 16);
  MovMem64(SP, addr, x.i);
  MovMem64(SP, addr + 8, 0);
  return addr;
}

uintptr_t A64Emitter::StashConstantV(int index, double v) {
  union {
    double d;
    uint64_t i;
  } x = {v};
  const auto addr = kStashOffset + (index * 16);
  MovMem64(SP, addr, x.i);
  MovMem64(SP, addr + 8, 0);
  return addr;
}

uintptr_t A64Emitter::StashConstantV(int index, const vec128_t& v) {
  const auto addr = kStashOffset + (index * 16);
  MovMem64(SP, addr, v.low);
  MovMem64(SP, addr + 8, v.high);
  return addr;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
