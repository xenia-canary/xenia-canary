/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Exception dispatch and unwinding. Routines that call guest code leave an
// X_KERNEL_FRAME, so walks from it step over them.

#include "xenia/kernel/xboxkrnl/exception_dispatch.h"

#include <bit>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

using cpu::ppc::PPCContext;

namespace {

constexpr uint32_t kInstructionBlr = 0x4E800020;

// As Processor::Execute gives guest code called from the host.
constexpr uint32_t kHostReturnAddress = 0xBCBCBCBC;

// Upper bound on frames walked, for corrupt stacks that loop.
constexpr uint32_t kMaxFrames = 0x4000;

constexpr fourcc_t kKernelFrameMagic = make_fourcc("XEXF");

constexpr uint32_t kKernelFrameRedZone = 0x100;

bool IsInStack(uint32_t address, uint32_t size, uint32_t low_limit,
               uint32_t high_limit) {
  return address >= low_limit && high_limit - low_limit >= size &&
         address - low_limit <= high_limit - low_limit - size;
}

// Reads from the guest stack that flag a fault outside it, so a corrupt frame
// fails the walk instead of faulting the host.
struct StackReader {
  uint32_t low_limit;
  uint32_t high_limit;
  bool fault = false;

  uint32_t Read32(uint32_t address) {
    if (!IsInStack(address, 4, low_limit, high_limit)) {
      fault = true;
      return 0;
    }

    return xe::load_and_swap<uint32_t>(
        kernel_memory()->TranslateVirtual<uint8_t*>(address));
  }

  uint64_t Read64(uint32_t address) {
    if (!IsInStack(address, 8, low_limit, high_limit)) {
      fault = true;
      return 0;
    }

    return xe::load_and_swap<uint64_t>(
        kernel_memory()->TranslateVirtual<uint8_t*>(address));
  }
};

void GetStackLimits(const PPCContext* ppc, uint32_t* low_limit,
                    uint32_t* high_limit) {
  auto kpcr = ppc->TranslateVirtualGPR<X_KPCR*>(ppc->r[13]);
  if (kpcr->use_alternative_stack) {
    *high_limit = kpcr->alt_stack_base_ptr;
    *low_limit = kpcr->alt_stack_end_ptr;
  } else {
    *high_limit = kpcr->stack_base_ptr;
    *low_limit = kpcr->stack_end_ptr;
  }
}

bool IsHostReturnAddress(uint32_t control_pc) {
  return control_pc == kHostReturnAddress ||
         control_pc == kHostReturnAddress - 4;
}

// Returns 0 if the stack has no room for the frame.
uint32_t AllocateKernelFrame(PPCContext* ppc, uint32_t stack_pointer,
                             const X_CONTEXT* caller_context) {
  const uint32_t address =
      (stack_pointer - kKernelFrameRedZone - sizeof(X_KERNEL_FRAME)) & ~0xFu;

  uint32_t low_limit;
  uint32_t high_limit;
  GetStackLimits(ppc, &low_limit, &high_limit);

  if (address >= stack_pointer ||
      !IsInStack(address, sizeof(X_KERNEL_FRAME), low_limit, high_limit)) {
    XELOGE("No room on the guest stack at {:08X} for exception dispatch",
           stack_pointer);
    return 0;
  }

  auto frame = kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(address);
  frame->back_chain = stack_pointer;
  frame->magic = 0;
  frame->kind = kKernelFrameCallout;
  frame->dispatcher_context_ptr = 0;
  frame->self = address;
  if (caller_context) {
    std::memcpy(&frame->caller_context, caller_context, sizeof(X_CONTEXT));
  } else {
    CaptureContext(ppc, &frame->caller_context);
  }

  return address;
}

// Returns nullptr if address isn't a frame from AllocateKernelFrame.
X_KERNEL_FRAME* FindKernelFrame(uint32_t address, uint32_t low_limit,
                                uint32_t high_limit) {
  if ((address & 0xF) ||
      !IsInStack(address, sizeof(X_KERNEL_FRAME), low_limit, high_limit)) {
    return nullptr;
  }

  auto frame = kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(address);
  if (frame->magic != kKernelFrameMagic || frame->self != address ||
      frame->kind > kKernelFrameUnwindHandler) {
    return nullptr;
  }

  return frame;
}

// As RtlLookupFunctionEntry. Returns the entry's address, or 0.
uint32_t LookupFunctionEntry(uint32_t control_pc) {
  auto processor = kernel_state()->processor();
  auto module =
      dynamic_cast<cpu::XexModule*>(processor->LookupModule(control_pc));
  if (!module) {
    return 0;
  }

  auto pdata = module->GetPESection(".pdata");
  if (!pdata || pdata->size < sizeof(X_RUNTIME_FUNCTION)) {
    return 0;
  }

  auto table = kernel_memory()->TranslateVirtual<const X_RUNTIME_FUNCTION*>(
      pdata->address);
  int32_t high = static_cast<int32_t>(pdata->size / sizeof(X_RUNTIME_FUNCTION));

  // The section can be padded out past the table with empty entries.
  while (high > 0 && !table[high - 1].begin_address) {
    --high;
  }

  int32_t low = 0;
  --high;
  while (high >= low) {
    const int32_t middle = (low + high) >> 1;
    const uint32_t begin = table[middle].begin_address;
    if (control_pc < begin) {
      high = middle - 1;
    } else if (control_pc < begin + FunctionLength(table[middle].flags) * 4) {
      return pdata->address + middle * sizeof(X_RUNTIME_FUNCTION);
    } else {
      low = middle + 1;
    }
  }

  return 0;
}

// What the unwinder does with an instruction.
enum class UnwindOp : uint32_t {
  kNone = 0,
  kMoveLr = 1,          // mflr rS reversed, mtlr rS
  kMoveCr = 2,          // mfcr rS reversed, mtcrf 0xFF, rS
  kLoadDouble = 3,      // std rS reversed
  kLoadWord = 4,        // stw rS reversed, lwz rS
  kPopFrame = 5,        // stwu/stwux r1 reversed
  kFrameToR12 = 6,      // stwu/stwux r1 before a save helper
  kLoadFloat = 7,       // stfd fS reversed, lfd fS
  kCopyToRa = 8,        // mr rA, rS
  kR1FromR12 = 9,       // mr r12, r1 before a save helper
  kCopyToRs = 10,       // mr rA, rS in an epilog
  kR12FromR1 = 11,      // addi r12, r1, d before a save helper
  kAddImmediate = 12,   // addi rT, rA, d in an epilog
  kSaveHelper = 13,     // bl to a save helper
  kRestoreHelper = 14,  // b to a restore helper
  kReturn = 15,         // blr
  kSetFrame = 16,
};

// Modes the unwinder reads instructions in.
constexpr uint32_t kUnwindModeForward = 0;     // epilogs and restore helpers
constexpr uint32_t kUnwindModeFindR12 = 1;     // the base a save helper uses
constexpr uint32_t kUnwindModeProlog = 2;      // prologs, backwards
constexpr uint32_t kUnwindModeSaveHelper = 3;  // save helpers, backwards

UnwindOp ClassifyUnwindOp(uint32_t instruction, uint32_t mode, uint32_t address,
                          uint32_t* branch_target, uint32_t* branch_entry) {
  const uint32_t opcode = instruction >> 26;
  const uint32_t rs = (instruction >> 21) & 0x1F;
  const uint32_t ra = (instruction >> 16) & 0x1F;
  const uint32_t rb = (instruction >> 11) & 0x1F;
  const bool is_lr_spr = (instruction & 0x001FF800) == 0x00080000;

  switch ((opcode << 2) | mode) {
    case (14 << 2) | kUnwindModeForward:  // addi
      return UnwindOp::kAddImmediate;
    case (14 << 2) | kUnwindModeFindR12:  // addi r12, r1, d
      return rs == 12 && ra == 1 ? UnwindOp::kR12FromR1 : UnwindOp::kNone;
    case (18 << 2) | kUnwindModeProlog: {  // b, bl
      uint32_t target =
          static_cast<uint32_t>(static_cast<int32_t>(instruction << 6) >> 6) &
          ~3u;
      if (!(instruction & 2)) {
        target += address;
      }

      const uint32_t entry = LookupFunctionEntry(target);
      *branch_entry = entry;
      if (!entry) {
        return UnwindOp::kNone;
      }

      const bool link = instruction & 1;
      if (link) {
        // A call to a function that saves LR itself isn't a save helper.
        const uint32_t callee =
            *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(target);
        if ((callee & 0xFC000000) == 0x7C000000 && (callee & 0x7FE) == 0x2A6 &&
            (callee & 0x001FF800) == 0x00080000) {
          return UnwindOp::kNone;
        }
      }

      *branch_target = target;
      const uint32_t kind = EntryKind(
          kernel_memory()->TranslateVirtual<X_RUNTIME_FUNCTION*>(entry)->flags);
      if (kind == kEntryKindSaveHelper) {
        return link ? UnwindOp::kSaveHelper : UnwindOp::kNone;
      }

      if (kind == kEntryKindRestoreHelper) {
        return link ? UnwindOp::kNone : UnwindOp::kRestoreHelper;
      }

      return UnwindOp::kNone;
    }
    case (19 << 2) | kUnwindModeForward:
      return instruction == kInstructionBlr ? UnwindOp::kReturn
                                            : UnwindOp::kNone;
    case (19 << 2) | kUnwindModeFindR12:
    case (19 << 2) | kUnwindModeProlog:
    case (19 << 2) | kUnwindModeSaveHelper:
      return (instruction & 0x7FE) == 0x64 ? UnwindOp::kSetFrame
                                           : UnwindOp::kNone;
    case (31 << 2) | kUnwindModeForward:
    case (31 << 2) | kUnwindModeFindR12:
    case (31 << 2) | kUnwindModeProlog:
    case (31 << 2) | kUnwindModeSaveHelper: {
      const uint32_t xo = (instruction >> 1) & 0x3FF;
      switch ((xo << 2) | mode) {
        case (19 << 2) | kUnwindModeProlog:  // mfcr
        case (19 << 2) | kUnwindModeSaveHelper:
          return UnwindOp::kMoveCr;
        case (144 << 2) | kUnwindModeForward:  // mtcrf
          return (instruction & 0xFF000) == 0xFF000 ? UnwindOp::kMoveCr
                                                    : UnwindOp::kNone;
        case (183 << 2) | kUnwindModeFindR12:  // stwux
        case (183 << 2) | kUnwindModeProlog:
        case (183 << 2) | kUnwindModeSaveHelper:
          if (rs != 1 || ra != 1) {
            return UnwindOp::kNone;
          }

          return mode == kUnwindModeFindR12 ? UnwindOp::kFrameToR12
                                            : UnwindOp::kPopFrame;
        case (339 << 2) | kUnwindModeProlog:  // mfspr
          return is_lr_spr ? UnwindOp::kMoveLr : UnwindOp::kNone;
        case (444 << 2) | kUnwindModeForward:  // or
          return rs == rb ? UnwindOp::kCopyToRs : UnwindOp::kNone;
        case (444 << 2) | kUnwindModeFindR12:
          return rs == rb && ra == 12 && rb == 1 ? UnwindOp::kR1FromR12
                                                 : UnwindOp::kNone;
        case (444 << 2) | kUnwindModeProlog:
        case (444 << 2) | kUnwindModeSaveHelper:
          return rs == rb && rb != 1 ? UnwindOp::kCopyToRa : UnwindOp::kNone;
        case (467 << 2) | kUnwindModeForward:  // mtspr
          return is_lr_spr ? UnwindOp::kMoveLr : UnwindOp::kNone;
        default:
          return UnwindOp::kNone;
      }
    }
    case (32 << 2) | kUnwindModeForward:  // lwz
      return UnwindOp::kLoadWord;
    case (36 << 2) | kUnwindModeProlog:  // stw
      return ra == 1 ? UnwindOp::kLoadWord : UnwindOp::kNone;
    case (36 << 2) | kUnwindModeSaveHelper:
      return ra == 12 || ra == 1 ? UnwindOp::kLoadWord : UnwindOp::kNone;
    case (37 << 2) | kUnwindModeFindR12:  // stwu
    case (37 << 2) | kUnwindModeProlog:
    case (37 << 2) | kUnwindModeSaveHelper:
      if (rs != 1 || ra != 1) {
        return UnwindOp::kNone;
      }

      return mode == kUnwindModeFindR12 ? UnwindOp::kFrameToR12
                                        : UnwindOp::kPopFrame;
    case (50 << 2) | kUnwindModeForward:  // lfd
      return UnwindOp::kLoadFloat;
    case (54 << 2) | kUnwindModeProlog:  // stfd
      return ra == 1 ? UnwindOp::kLoadFloat : UnwindOp::kNone;
    case (54 << 2) | kUnwindModeSaveHelper:
      return ra == 12 || ra == 1 ? UnwindOp::kLoadFloat : UnwindOp::kNone;
    case (62 << 2) | kUnwindModeProlog:  // std
      return !(instruction & 3) && ra == 1 ? UnwindOp::kLoadDouble
                                           : UnwindOp::kNone;
    case (62 << 2) | kUnwindModeSaveHelper:
      return !(instruction & 3) && (ra == 12 || ra == 1) ? UnwindOp::kLoadDouble
                                                         : UnwindOp::kNone;
    default:
      return UnwindOp::kNone;
  }
}

struct UnwindRegion {
  uint32_t start;
  uint32_t end;  // exclusive, reached by stepping from start
  int32_t step;
  uint32_t mode;
};

// The prolog, and each save helper called in it, adds two regions.
constexpr int32_t kMaxUnwindRegions = 32;

uint32_t GetHandler(uint32_t function_entry) {
  const uint32_t begin =
      kernel_memory()
          ->TranslateVirtual<const X_RUNTIME_FUNCTION*>(function_entry)
          ->begin_address;
  return xe::load_and_swap<uint32_t>(
      kernel_memory()->TranslateVirtual<uint8_t*>(begin - 8));
}

bool HasHandler(uint32_t function_entry) {
  return EntryKind(
             kernel_memory()
                 ->TranslateVirtual<const X_RUNTIME_FUNCTION*>(function_entry)
                 ->flags) == kEntryKindHandler;
}

// Raises code from the routine's call, chained to chained_record.
void RaiseFromRoutine(PPCContext* ppc, uint32_t frame_address, uint32_t code,
                      uint32_t chained_record) {
  auto frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(frame_address);
  const uint32_t call_pc = frame->caller_context.lr - 4;
  const uint32_t dispatch_address =
      AllocateKernelFrame(ppc, frame_address, &frame->caller_context);
  if (!dispatch_address) {
    return;
  }

  auto dispatch_frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(dispatch_address);
  const uint32_t record_address =
      dispatch_address + offsetof(X_KERNEL_FRAME, exception_record);
  auto record = &dispatch_frame->exception_record;
  std::memset(record, 0, sizeof(X_EXCEPTION_RECORD));
  record->code = code;
  record->exception_flags = kExceptionNoncontinuable;
  record->exception_record = chained_record;
  record->exception_address = call_pc;
  std::memcpy(&dispatch_frame->context, &dispatch_frame->caller_context,
              sizeof(X_CONTEXT));

  // Noncontinuable, so a handler that takes it doesn't return here.
  DispatchException(ppc, dispatch_address, record_address,
                    dispatch_address + offsetof(X_KERNEL_FRAME, context),
                    call_pc);
  XELOGE("No guest handler took exception {:08X} raised at {:08X}", code,
         call_pc);
  ReleaseKernelFrame(dispatch_address);
}

// As RtlVirtualUnwind. Returns the call in the caller (LR - 4).
uint32_t VirtualUnwind(uint32_t control_pc, uint32_t function_entry,
                       X_CONTEXT* context, bool* in_function,
                       uint32_t* establisher_frame, uint32_t low_limit,
                       uint32_t high_limit) {
  if (!function_entry) {
    return context->lr - 4;
  }

  StackReader reader{low_limit, high_limit};
  auto entry = kernel_memory()->TranslateVirtual<const X_RUNTIME_FUNCTION*>(
      function_entry);
  const uint32_t begin = entry->begin_address;
  const uint32_t flags = entry->flags;
  const uint32_t prolog_end = begin + PrologLength(flags) * 4;

  *establisher_frame = static_cast<uint32_t>(context->gpr[1]);

  uint32_t instruction =
      *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(control_pc);
  if (instruction == kInstructionBlr) {
    *in_function = false;
    return context->lr;
  }

  UnwindRegion regions[kMaxUnwindRegions];
  uint32_t branch_target = 0;
  uint32_t branch_entry = 0;
  const UnwindOp first_op =
      ClassifyUnwindOp(instruction, kUnwindModeProlog, control_pc,
                       &branch_target, &branch_entry);

  if (first_op == UnwindOp::kRestoreHelper) {
    // Branching to the restore helper at the end of an epilog: finish it.
    auto helper = kernel_memory()->TranslateVirtual<const X_RUNTIME_FUNCTION*>(
        branch_entry);
    regions[0] = {branch_target,
                  helper->begin_address + FunctionLength(helper->flags) * 4, 4,
                  kUnwindModeForward};
    *in_function = false;
  } else if (EntryKind(flags) == kEntryKindRestoreHelper) {
    // In a restore helper.
    XELOGW("Unwinding from inside the restore helper at {:08X}", control_pc);
    regions[0] = {control_pc, begin + FunctionLength(flags) * 4, 4,
                  kUnwindModeForward};
    *in_function = false;
  } else if (control_pc >= begin && control_pc < prolog_end) {
    // Partway through the prolog: undo what has run of it.
    regions[0] = {control_pc - 4, begin - 4, -4, kUnwindModeProlog};
    *in_function = false;
  } else {
    regions[0] = {prolog_end - 4, begin - 4, -4, kUnwindModeProlog};
    *in_function = true;
  }

  uint32_t next_pc = context->lr - 4;
  bool lr_restored = false;
  bool frame_popped = false;
  bool r12_from_frame = false;

  int32_t top = 0;
  while (top >= 0 && !reader.fault) {
    UnwindRegion& region = regions[top];
    uint32_t address = region.start;
    if (address == region.end) {
      --top;
      continue;
    }

    bool pushed = false;
    bool region_done = false;
    do {
      instruction =
          *kernel_memory()->TranslateVirtual<xe::be<uint32_t>*>(address);

      const uint32_t rs = (instruction >> 21) & 0x1F;
      const uint32_t ra = (instruction >> 16) & 0x1F;
      const int64_t displacement = static_cast<int16_t>(instruction & 0xFFFF);
      const uint64_t effective_address =
          static_cast<uint64_t>(context->gpr[ra]) + displacement;
      const uint32_t effective_address_32 =
          static_cast<uint32_t>(effective_address);

      switch (ClassifyUnwindOp(instruction, region.mode, address,
                               &branch_target, &branch_entry)) {
        case UnwindOp::kMoveLr:
          context->lr = static_cast<uint32_t>(context->gpr[rs]);
          if (!lr_restored) {
            next_pc = context->lr - 4;
            lr_restored = true;
          } else {
            next_pc += 4;
          }

          break;
        case UnwindOp::kMoveCr:
          context->cr = static_cast<uint32_t>(context->gpr[rs]);
          break;
        case UnwindOp::kLoadDouble:
          context->gpr[rs] = reader.Read64(effective_address_32);
          break;
        case UnwindOp::kLoadWord:
          context->gpr[rs] = reader.Read32(effective_address_32);
          break;
        case UnwindOp::kPopFrame:
          context->gpr[1] =
              reader.Read32(static_cast<uint32_t>(context->gpr[1]));
          if (!frame_popped) {
            frame_popped = true;
            *establisher_frame = static_cast<uint32_t>(context->gpr[1]);
          }

          break;
        case UnwindOp::kFrameToR12:
          r12_from_frame = true;
          context->gpr[12] =
              reader.Read32(static_cast<uint32_t>(context->gpr[1]));
          break;
        case UnwindOp::kLoadFloat:
          context->fpr[rs] = reader.Read64(effective_address_32);
          break;
        case UnwindOp::kCopyToRa:
          context->gpr[ra] = context->gpr[rs];
          break;
        case UnwindOp::kR1FromR12:
          context->gpr[rs] = context->gpr[ra];
          region_done = true;
          break;
        case UnwindOp::kCopyToRs:
          context->gpr[rs] = context->gpr[ra];
          break;
        case UnwindOp::kR12FromR1:
          if (!r12_from_frame) {
            context->gpr[rs] = context->gpr[1];
          }

          context->gpr[rs] =
              static_cast<uint64_t>(context->gpr[rs]) + displacement;
          region_done = true;
          break;
        case UnwindOp::kAddImmediate:
          context->gpr[rs] = effective_address_32;
          break;
        case UnwindOp::kSaveHelper: {
          // Undo the helper's stores back to its entry, then the prolog.
          if (top + 2 >= kMaxUnwindRegions) {
            XELOGE("Too many save helper calls in the prolog at {:08X}", begin);
            reader.fault = true;
            break;
          }

          auto helper =
              kernel_memory()->TranslateVirtual<const X_RUNTIME_FUNCTION*>(
                  branch_entry);
          const uint32_t helper_end =
              helper->begin_address + FunctionLength(helper->flags) * 4;
          regions[top + 1] = {helper_end - 4, branch_target - 4, -4,
                              kUnwindModeSaveHelper};
          regions[top + 2] = {address - 4, region.end, -4, kUnwindModeFindR12};
          region.start = address - 4;
          r12_from_frame = false;
          top += 2;
          pushed = true;
          break;
        }
        case UnwindOp::kReturn:
          next_pc = context->lr - 4;
          region_done = true;
          break;
        case UnwindOp::kSetFrame:
          *establisher_frame = static_cast<uint32_t>(context->gpr[1]);
          break;
        case UnwindOp::kRestoreHelper:
        case UnwindOp::kNone:
          break;
      }

      if (pushed || region_done || reader.fault) {
        break;
      }

      address += region.step;
    } while (address != region.end);

    if (!pushed) {
      --top;
    }
  }

  if (reader.fault) {
    XELOGE("Unwinding the frame at {:08X} read outside the stack", control_pc);
    // Fails the callers' check of the establisher frame.
    *establisher_frame = 0;
  }

  return next_pc;
}

}  // namespace

void CaptureContext(const PPCContext* ppc, X_CONTEXT* context) {
  std::memset(context, 0, sizeof(X_CONTEXT));
  context->context_flags = kContextFull;
  context->msr = static_cast<uint32_t>(ppc->msr);
  // Continuing goes to the return address.
  context->iar = static_cast<uint32_t>(ppc->lr);
  context->lr = static_cast<uint32_t>(ppc->lr);
  context->ctr = ppc->ctr;
  for (uint32_t i = 0; i < 32; ++i) {
    context->gpr[i] = ppc->r[i];
  }

  context->cr = static_cast<uint32_t>(ppc->cr());
  context->xer = ppc->xer();
  context->fpscr = ppc->fpscr.value;
  for (uint32_t i = 0; i < 32; ++i) {
    context->fpr[i] = std::bit_cast<uint64_t>(ppc->f[i]);
  }

  context->vscr[3] = ppc->vscr_sat ? 1 : 0;
  for (uint32_t i = 0; i < 128; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      context->vr[i][j] = ppc->v[i].u32[j];
    }
  }
}

void RestoreContext(PPCContext* ppc, const X_CONTEXT* context) {
  for (uint32_t i = 0; i < 32; ++i) {
    ppc->r[i] = context->gpr[i];
  }

  ppc->ctr = context->ctr;
  ppc->set_cr(context->cr);
  ppc->set_xer(context->xer);
  for (uint32_t i = 0; i < 32; ++i) {
    ppc->f[i] = std::bit_cast<double>(static_cast<uint64_t>(context->fpr[i]));
  }

  for (uint32_t i = 0; i < 128; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      ppc->v[i].u32[j] = context->vr[i][j];
    }
  }

  // The import thunk returns through LR to Iar.
  ppc->lr = context->iar;
}

uint32_t AllocateKernelFrame(PPCContext* ppc) {
  return AllocateKernelFrame(ppc, static_cast<uint32_t>(ppc->r[1]), nullptr);
}

void ReleaseKernelFrame(uint32_t address) {
  kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(address)->magic = 0;
}

uint32_t CallGuest(PPCContext* ppc, uint32_t frame_address,
                   KernelFrameKind kind, uint32_t dispatcher_context,
                   uint32_t function_address, const GuestCallArgs& args) {
  auto frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(frame_address);
  frame->kind = kind;
  frame->dispatcher_context_ptr = dispatcher_context;
  frame->magic = kKernelFrameMagic;

  auto function = ppc->processor->ResolveFunction(function_address);
  if (!function) {
    XELOGE("Exception dispatch couldn't find the guest function at {:08X}",
           function_address);
    return kExceptionContinueSearch;
  }

  const uint64_t saved_stack_pointer = ppc->r[1];
  const uint64_t saved_lr = ppc->lr;

  ppc->r[3] = args.r3;
  ppc->r[4] = args.r4;
  ppc->r[5] = args.r5;
  ppc->r[6] = args.r6;
  ppc->r[7] = args.r7;
  ppc->r[12] = args.r12;
  ppc->r[1] = frame_address;
  ppc->lr = kHostReturnAddress;

  function->Call(ppc->thread_state, kHostReturnAddress);

  const uint32_t result = static_cast<uint32_t>(ppc->r[3]);
  ppc->r[1] = saved_stack_pointer;
  ppc->lr = saved_lr;
  return result;
}

bool DispatchException(PPCContext* ppc, uint32_t frame_address,
                       uint32_t record_address, uint32_t context_address,
                       uint32_t start_pc) {
  auto frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(frame_address);
  auto record =
      kernel_memory()->TranslateVirtual<X_EXCEPTION_RECORD*>(record_address);
  const uint32_t dispatcher_context_address =
      frame_address + offsetof(X_KERNEL_FRAME, dispatcher_context);
  auto dispatcher_context = &frame->dispatcher_context;

  uint32_t low_limit;
  uint32_t high_limit;
  GetStackLimits(ppc, &low_limit, &high_limit);

  // Where the exception was, for the exceptions raised for it.
  X_CONTEXT original_context;
  std::memcpy(&original_context,
              kernel_memory()->TranslateVirtual<X_CONTEXT*>(context_address),
              sizeof(X_CONTEXT));

  // Walked up the stack, while handlers get the context of the exception.
  X_CONTEXT context;
  std::memcpy(&context, &original_context, sizeof(X_CONTEXT));

  uint32_t control_pc = start_pc;
  uint32_t flags = record->exception_flags & kExceptionNoncontinuable;
  uint32_t nested_frame = 0;
  uint32_t raised = 0;

  // Raises code chained to the current exception. False once out of records.
  auto raise_chained = [&](uint32_t code) {
    if (raised == kMaxRaisedExceptions) {
      XELOGE(
          "Exception dispatch already raised {} exceptions, giving up on "
          "{:08X}",
          raised, code);
      return false;
    }

    const uint32_t raised_address = frame_address +
                                    offsetof(X_KERNEL_FRAME, raised_records) +
                                    raised * sizeof(X_EXCEPTION_RECORD);
    ++raised;
    auto raised_record =
        kernel_memory()->TranslateVirtual<X_EXCEPTION_RECORD*>(raised_address);
    std::memset(raised_record, 0, sizeof(X_EXCEPTION_RECORD));
    raised_record->code = code;
    raised_record->exception_flags = kExceptionNoncontinuable;
    raised_record->exception_record = record_address;
    raised_record->exception_address = start_pc;
    record_address = raised_address;
    record = raised_record;
    flags = kExceptionNoncontinuable;
    nested_frame = 0;
    std::memcpy(kernel_memory()->TranslateVirtual<X_CONTEXT*>(context_address),
                &original_context, sizeof(X_CONTEXT));
    std::memcpy(&context, &original_context, sizeof(X_CONTEXT));
    control_pc = start_pc;
    return true;
  };

  for (uint32_t depth = 0; depth < kMaxFrames; ++depth) {
    uint32_t next_pc;

    if (IsHostReturnAddress(control_pc)) {
      auto kernel_frame = FindKernelFrame(static_cast<uint32_t>(context.gpr[1]),
                                          low_limit, high_limit);

      if (!kernel_frame) {
        break;
      }

      record->exception_flags = flags;
      if (kernel_frame->kind == kKernelFrameExceptionHandler) {
        // Raised in another exception's handler: skip to that handler's frame.
        auto outer = kernel_memory()->TranslateVirtual<X_DISPATCHER_CONTEXT*>(
            kernel_frame->dispatcher_context_ptr);
        flags |= kExceptionNestedCall;
        if (outer->establisher_frame > nested_frame) {
          nested_frame = outer->establisher_frame;
        }
      }

      // Carry on from the routine's caller.
      std::memcpy(&context, &kernel_frame->caller_context, sizeof(X_CONTEXT));
      next_pc = context.lr - 4;
    } else {
      const uint32_t function_entry = LookupFunctionEntry(control_pc);
      bool in_function = false;
      uint32_t establisher_frame = static_cast<uint32_t>(context.gpr[1]);

      next_pc =
          VirtualUnwind(control_pc, function_entry, &context, &in_function,
                        &establisher_frame, low_limit, high_limit);

      if (!function_entry) {
        if (next_pc == control_pc) {
          break;
        }
      } else if (establisher_frame < low_limit ||
                 establisher_frame > high_limit || (establisher_frame & 7)) {
        flags |= kExceptionStackInvalid;
        break;
      } else if (HasHandler(function_entry) && in_function) {
        const uint32_t handler = GetHandler(function_entry);
        dispatcher_context->control_pc = control_pc;
        dispatcher_context->function_entry = function_entry;
        dispatcher_context->establisher_frame = establisher_frame;
        dispatcher_context->context_record = context_address;
        record->exception_flags = flags;

        GuestCallArgs args;
        args.r3 = record_address;
        args.r4 = establisher_frame;
        args.r5 = context_address;
        args.r6 = dispatcher_context_address;
        args.r7 = handler;
        const uint32_t disposition =
            CallGuest(ppc, frame_address, kKernelFrameExceptionHandler,
                      dispatcher_context_address, handler, args);

        flags |= record->exception_flags & kExceptionNoncontinuable;
        if (nested_frame == establisher_frame) {
          flags &= ~kExceptionNestedCall;
          nested_frame = 0;
        }

        uint32_t raise_code = 0;
        switch (disposition) {
          case kExceptionContinueExecution:
            if (!(flags & kExceptionNoncontinuable)) {
              return true;
            }

            XELOGW(
                "Exception handler at {:08X} continued noncontinuable "
                "exception {:08X}, raising STATUS_NONCONTINUABLE_EXCEPTION",
                handler, static_cast<uint32_t>(record->code));
            raise_code = kStatusNoncontinuableException;
            break;
          case kExceptionContinueSearch:
            break;
          case kExceptionNestedException:
            flags |= kExceptionNestedCall;
            if (dispatcher_context->establisher_frame > nested_frame) {
              nested_frame = dispatcher_context->establisher_frame;
            }

            break;
          default:
            XELOGW(
                "Exception handler at {:08X} returned invalid disposition {}, "
                "raising STATUS_INVALID_DISPOSITION",
                handler, disposition);
            raise_code = kStatusInvalidDisposition;
            break;
        }

        if (raise_code) {
          record->exception_flags = flags;
          if (!raise_chained(raise_code)) {
            return false;
          }

          depth = 0;
          continue;
        }
      }
    }

    control_pc = next_pc;
    if (static_cast<uint32_t>(context.gpr[1]) >= high_limit) {
      break;
    }
  }

  record->exception_flags = flags;
  return false;
}

bool Unwind(PPCContext* ppc, uint32_t frame_address, uint32_t target_frame,
            uint32_t target_ip, uint32_t record_address, uint32_t return_value,
            uint32_t context_address) {
  auto frame =
      kernel_memory()->TranslateVirtual<X_KERNEL_FRAME*>(frame_address);
  const uint32_t dispatcher_context_address =
      frame_address + offsetof(X_KERNEL_FRAME, dispatcher_context);
  auto dispatcher_context = &frame->dispatcher_context;

  uint32_t low_limit;
  uint32_t high_limit;
  GetStackLimits(ppc, &low_limit, &high_limit);

  auto context = kernel_memory()->TranslateVirtual<X_CONTEXT*>(context_address);
  std::memcpy(context, &frame->caller_context, sizeof(X_CONTEXT));
  context->iar = target_ip;

  uint32_t control_pc = context->lr - 4;
  uint32_t establisher_frame = static_cast<uint32_t>(context->gpr[1]);

  if (!record_address) {
    record_address = frame_address + offsetof(X_KERNEL_FRAME, exception_record);
    auto local_record =
        kernel_memory()->TranslateVirtual<X_EXCEPTION_RECORD*>(record_address);
    std::memset(local_record, 0, sizeof(X_EXCEPTION_RECORD));
    local_record->code = kStatusUnwind;
    local_record->exception_address = control_pc;
  }

  auto record =
      kernel_memory()->TranslateVirtual<X_EXCEPTION_RECORD*>(record_address);

  uint32_t flags = kExceptionUnwinding;
  if (!target_frame) {
    // Only set in the record.
    record->exception_flags = record->exception_flags | kExceptionExitUnwind;
  }

  uint32_t function_entry = 0;
  bool in_function = false;

  // Returns false for an invalid disposition.
  auto call_unwind_handlers = [&](uint32_t* next_pc) {
    do {
      if (target_frame == establisher_frame) {
        flags |= kExceptionTargetUnwind;
      }

      record->exception_flags = flags;
      context->gpr[3] = return_value;
      flags &= ~(kExceptionTargetUnwind | kExceptionCollidedUnwind);

      const uint32_t handler = GetHandler(function_entry);
      GuestCallArgs args;
      args.r3 = record_address;
      args.r4 = establisher_frame;
      args.r5 = context_address;
      args.r6 = dispatcher_context_address;
      args.r7 = handler;
      const uint32_t disposition =
          CallGuest(ppc, frame_address, kKernelFrameUnwindHandler,
                    dispatcher_context_address, handler, args);

      if (disposition == kExceptionContinueSearch) {
        if (establisher_frame != target_frame) {
          *next_pc =
              VirtualUnwind(control_pc, function_entry, context, &in_function,
                            &establisher_frame, low_limit, high_limit);
        }
      } else if (disposition == kExceptionCollidedUnwindDisposition) {
        // Carry on from where the unwind this one collided with got to.
        context_address = dispatcher_context->context_record;
        context =
            kernel_memory()->TranslateVirtual<X_CONTEXT*>(context_address);
        flags |= kExceptionCollidedUnwind;
        control_pc = dispatcher_context->control_pc;
        function_entry = dispatcher_context->function_entry;
        context->iar = target_ip;
        establisher_frame = dispatcher_context->establisher_frame;
      } else {
        XELOGW(
            "Unwind handler at {:08X} returned invalid disposition {}, raising "
            "STATUS_INVALID_DISPOSITION",
            handler, disposition);
        RaiseFromRoutine(ppc, frame_address, kStatusInvalidDisposition,
                         record_address);
        return false;
      }
    } while (flags & kExceptionCollidedUnwind);
    return true;
  };

  for (uint32_t depth = 0; depth < kMaxFrames; ++depth) {
    uint32_t next_pc = control_pc;

    if (IsHostReturnAddress(control_pc)) {
      auto kernel_frame = FindKernelFrame(
          static_cast<uint32_t>(context->gpr[1]), low_limit, high_limit);

      if (!kernel_frame) {
        XELOGE("Unwinding to {:08X} reached the bottom of the guest stack",
               target_frame);
        return false;
      }

      if (kernel_frame->kind == kKernelFrameUnwindHandler) {
        // Inside another unwind's handler: carry on where that unwind got to.
        record->exception_flags = flags;
        context->gpr[3] = return_value;
        flags &= ~(kExceptionTargetUnwind | kExceptionCollidedUnwind);

        std::memcpy(dispatcher_context,
                    kernel_memory()->TranslateVirtual<X_DISPATCHER_CONTEXT*>(
                        kernel_frame->dispatcher_context_ptr),
                    sizeof(X_DISPATCHER_CONTEXT));
        context_address = dispatcher_context->context_record;
        context =
            kernel_memory()->TranslateVirtual<X_CONTEXT*>(context_address);
        flags |= kExceptionCollidedUnwind;
        control_pc = dispatcher_context->control_pc;
        function_entry = dispatcher_context->function_entry;
        context->iar = target_ip;
        establisher_frame = dispatcher_context->establisher_frame;

        if (!call_unwind_handlers(&next_pc)) {
          return false;
        }
      } else {
        if (kernel_frame->kind == kKernelFrameExceptionHandler) {
          record->exception_flags = flags;
          context->gpr[3] = return_value;
          flags &= ~(kExceptionTargetUnwind | kExceptionCollidedUnwind);
        }

        // Carry on from the routine's caller.
        const uint32_t iar = context->iar;
        std::memcpy(context, &kernel_frame->caller_context, sizeof(X_CONTEXT));
        context->iar = iar;
        establisher_frame = static_cast<uint32_t>(context->gpr[1]);
        next_pc = context->lr - 4;
      }
    } else {
      function_entry = LookupFunctionEntry(control_pc);
      if (!function_entry) {
        next_pc = VirtualUnwind(control_pc, 0, context, &in_function,
                                &establisher_frame, low_limit, high_limit);
        if (next_pc == control_pc) {
          XELOGE("Unwinding to {:08X} found no function table entry for {:08X}",
                 target_frame, control_pc);
          return false;
        }
      } else {
        X_CONTEXT local_context;
        std::memcpy(&local_context, context, sizeof(X_CONTEXT));
        next_pc = VirtualUnwind(control_pc, function_entry, &local_context,
                                &in_function, &establisher_frame, low_limit,
                                high_limit);

        if (establisher_frame < low_limit || establisher_frame > high_limit ||
            (target_frame && target_frame < establisher_frame) ||
            (establisher_frame & 7)) {
          XELOGE("Unwinding to {:08X} found a bad stack frame {:08X} at {:08X}",
                 target_frame, establisher_frame, control_pc);
          return false;
        }

        if (HasHandler(function_entry) && in_function) {
          dispatcher_context->control_pc = control_pc;
          dispatcher_context->function_entry = function_entry;
          dispatcher_context->establisher_frame = establisher_frame;
          dispatcher_context->context_record = context_address;
          if (!call_unwind_handlers(&next_pc)) {
            return false;
          }
        } else if (establisher_frame != target_frame) {
          next_pc =
              VirtualUnwind(control_pc, function_entry, context, &in_function,
                            &establisher_frame, low_limit, high_limit);
        }
      }
    }

    control_pc = next_pc;
    if (establisher_frame >= high_limit || establisher_frame == target_frame) {
      break;
    }
  }

  if (establisher_frame != target_frame) {
    XELOGE("Unwinding didn't find the target frame {:08X}", target_frame);
    return false;
  }

  context->gpr[3] = return_value;
  RestoreContext(ppc, context);
  return true;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
