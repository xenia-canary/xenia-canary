/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Guest C++ / SEH exception dispatch for the Xbox 360 (PPC MSVC EH model).
 *
 * WIP -- RXDK-360 MSVC-EH interop (see RXDK-360/docs/msvc-eh-interop.md). Stock
 * xenia logs a guest C++ throw and returns without unwinding, so catches never
 * run. This drafts the console kernel's SEH dispatch against the reverse-
 * engineered Xenon EH data format.
 *
 * INCREMENT 1 (this file): resolve the throwing frame's RUNTIME_FUNCTION from
 * the executable module's .pdata and read its [__CxxFrameHandler, FuncInfo]
 * pair, logging them -- proving the .pdata scan + word decode on a LIVE throw.
 * Later increments: marshal a DISPATCHER_CONTEXT + Execute the guest handler,
 * RtlVirtualUnwind (outer frames), and RtlUnwind control transfer to the catch.
 * Test bed: RXDK-360/tests/eh; done when it prints "[T] caught-int 1234".
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Xenon RUNTIME_FUNCTION (.pdata, 8 bytes, big-endian), reverse-engineered from
// a real cl.exe object (RXDK-360/tests/eh/tc.obj):
//   +0  BeginAddress -- ABSOLUTE guest address of the function's first code
//       instruction (the loader has relocated it; xex_module.cc uses it raw).
//   +4  packed word: PrologLen:8 | FuncLen:22 | ThirtyTwoBit:1 | ExceptionFlag:1
//       (lengths in INSTRUCTIONS; *4 for bytes).
// When ExceptionFlag is set, the two dwords at BeginAddress-8 are
//   [ __CxxFrameHandler addr ][ FuncInfo (_s_FuncInfo) addr ].
struct XenonRuntimeFunction {
  uint32_t begin_address;
  uint32_t prolog_length;
  uint32_t function_length;
  bool thirty_two_bit;
  bool exception_flag;
};

static XenonRuntimeFunction DecodeRuntimeFunction(uint32_t begin, uint32_t word) {
  XenonRuntimeFunction rf;
  rf.begin_address = begin;
  rf.prolog_length = word & 0xFF;
  rf.function_length = (word >> 8) & 0x3FFFFF;
  rf.thirty_two_bit = (word >> 30) & 1;
  rf.exception_flag = (word >> 31) & 1;
  return rf;
}

// RtlLookupFunctionEntry: find the RUNTIME_FUNCTION whose code range covers `pc`.
// The .pdata comes from the executable module's PE ".pdata" section (RXDK's
// elf2xex now emits the exception directory; xenia already parses this section).
static bool LookupFunctionEntry(uint32_t pc, XenonRuntimeFunction* out_rf) {
  auto exe = kernel_state()->GetExecutableModule();
  if (!exe) {
    return false;
  }
  auto xex = exe->xex_module();
  if (!xex) {
    return false;
  }
  auto pdata = xex->GetPESection(".pdata");
  if (!pdata) {
    return false;
  }
  auto* mem = kernel_memory();
  uint8_t* base = mem->TranslateVirtual(pdata->address);
  uint32_t count = pdata->raw_size / 8;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t begin = xe::load_and_swap<uint32_t>(base + i * 8);
    uint32_t word = xe::load_and_swap<uint32_t>(base + i * 8 + 4);
    if (!begin) {
      break;  // zero terminator
    }
    auto rf = DecodeRuntimeFunction(begin, word);
    if (pc >= rf.begin_address &&
        pc < rf.begin_address + rf.function_length * 4) {
      *out_rf = rf;
      return true;
    }
  }
  return false;
}

// The two-pass MSVC EH dispatch. Called from HandleCppException with the guest
// EXCEPTION_RECORD address. Returns true if control transferred to a catch.
// INCREMENT 1: resolve + log the throwing frame's handler; does not yet catch.
bool DispatchCppException(uint32_t exception_record_ptr) {
  auto thread = XThread::GetCurrentThread();
  if (!thread) {
    return false;
  }
  auto* ctx = thread->thread_state()->context();
  auto* mem = kernel_memory();
  auto rd32 = [&](uint32_t addr) -> uint32_t {
    return xe::load_and_swap<uint32_t>(mem->TranslateVirtual(addr));
  };

  // Walk guest frames from the throw point up to the catching frame. The throw
  // is nested: RaiseException(shim) <- _CxxThrowException <- rxdk_eh_test, so
  // pc=LR starts in _CxxThrowException (exc=0). Step up the PPC back-chain until
  // a frame with an exception handler (exc=1) is found.
  uint32_t pc = static_cast<uint32_t>(ctx->lr);
  uint32_t sp = static_cast<uint32_t>(ctx->r[1]);

  for (int frame = 0; frame < 64; ++frame) {
    XenonRuntimeFunction rf;
    bool have = LookupFunctionEntry(pc, &rf);
    if (have) {
      XELOGD("EH: frame {} pc={:08X} sp={:08X} func@{:08X} prolog={} len={} exc={}",
             frame, pc, sp, rf.begin_address, rf.prolog_length,
             rf.function_length, rf.exception_flag ? 1 : 0);
    } else {
      XELOGD("EH: frame {} pc={:08X} sp={:08X} (no RF)", frame, pc, sp);
    }
    if (have && rf.exception_flag) {
      uint32_t handler = rd32(rf.begin_address - 8);
      uint32_t funcinfo = rd32(rf.begin_address - 4);
      XELOGD("EH: -> catching frame __CxxFrameHandler@{:08X} FuncInfo@{:08X} sp={:08X}",
             handler, funcinfo, sp);

      // ---- Host-side C++ match (reverse-engineered 360 MSVC EH layout) ----
      // FuncInfo:      magic@0 maxState@4 pUnwindMap@8 nTryBlocks@0xC
      //                pTryBlockMap@0x10 ...
      // TryBlockMap(0x14): tryLow tryHigh catchHigh nCatches pHandlerArray
      // HandlerType(0x10): adjectives pType(type_info*) dispCatchObj handler
      // EXCEPTION_RECORD: exception_information[] @0x14; [2]=ThrowInfo.
      // ThrowInfo: attributes@0 unwind@4 fwd@8 catchableTypeArray@0xC
      // CatchableTypeArray: count@0 ptrs@4; CatchableType: properties@0 pType@4
      uint32_t magic = rd32(funcinfo);
      if (magic != 0x19930520 && magic != 0x19930521 && magic != 0x19930522) {
        XELOGE("EH: unexpected FuncInfo magic {:08X}", magic);
        return false;
      }
      uint32_t throw_info = rd32(exception_record_ptr + 0x1C);
      uint32_t cta = throw_info ? rd32(throw_info + 0x0C) : 0;
      uint32_t n_thrown = cta ? rd32(cta) : 0;

      uint32_t n_try = rd32(funcinfo + 0x0C);
      uint32_t try_map = rd32(funcinfo + 0x10);
      for (uint32_t t = 0; t < n_try; ++t) {
        uint32_t tb = try_map + t * 0x14;
        uint32_t n_catch = rd32(tb + 0x0C);
        uint32_t harray = rd32(tb + 0x10);
        for (uint32_t h = 0; h < n_catch; ++h) {
          uint32_t ht = harray + h * 0x10;
          uint32_t catch_type = rd32(ht + 4);       // type_info*, 0 == catch(...)
          uint32_t disp_catchobj = rd32(ht + 8);
          uint32_t handler_addr = rd32(ht + 0x0C);
          bool match = (catch_type == 0);           // ellipsis catches anything
          for (uint32_t c = 0; !match && c < n_thrown; ++c) {
            uint32_t ct = rd32(cta + 4 + c * 4);
            if (ct && rd32(ct + 4) == catch_type) {
              match = true;
            }
          }
          if (match) {
            uint32_t try_low = rd32(tb + 0x00);
            // The MSVC PPC "establisher frame" passed to funclets in r12 is the
            // function's INCOMING sp (before its `stwu`). The catching frame's
            // working sp holds its caller's sp in the back-chain word, so the
            // establisher == *(sp). Funclets recover the frame pointer as
            // `addi r31, r12, -framesize` (verified: rxdk_eh_test2's prologue is
            // `addi r31,r1,-0x70; stwu r1,-0x70(r1)`, so r31 == working sp), and
            // read locals / the catch object relative to r12 (dispCatchObj).
            uint32_t est = rd32(sp);
            XELOGD(
                "EH: MATCH try {} catch {} type={:08X} catchobj_disp={:08X} "
                "funclet@{:08X} tryLow={} est(r12)={:08X}",
                t, h, catch_type, disp_catchobj, handler_addr, try_low, est);

            // ---- Determine the current EH state in this frame (ip2state) ----
            // FuncInfo: nIPMapEntries@0x14, pIPtoStateMap@0x18. Each entry is
            // {ip(4), state(4)}; the active state is the one whose ip is the
            // greatest <= the CALL site. `pc` is the return address (call+4), so
            // look up pc-1 to land inside the throwing call instruction.
            uint32_t n_ip = rd32(funcinfo + 0x14);
            uint32_t ip_map = rd32(funcinfo + 0x18);
            uint32_t look = pc - 1;
            int32_t cur_state = -1;
            uint32_t best_ip = 0;
            for (uint32_t e = 0; e < n_ip; ++e) {
              uint32_t eip = rd32(ip_map + e * 8);
              int32_t est_state = static_cast<int32_t>(rd32(ip_map + e * 8 + 4));
              if (eip <= look && eip >= best_ip) {
                best_ip = eip;
                cur_state = est_state;
              }
            }
            int32_t target_state = static_cast<int32_t>(try_low) - 1;
            XELOGD("EH: curState={} targetState={} (look={:08X})", cur_state,
                   target_state, look);

            // ---- Build the catch object in the establisher frame ----
            // exception_information[1] (record+0x18) points at the thrown object;
            // for catch-by-value of int, copy 4 bytes. dispCatchObj is a signed
            // displacement from the establisher frame base (r12 == est).
            uint32_t thrown_obj = rd32(exception_record_ptr + 0x18);
            uint32_t catchobj_addr = est + disp_catchobj;
            uint32_t val = rd32(thrown_obj);
            xe::store_and_swap<uint32_t>(mem->TranslateVirtual(catchobj_addr), val);
            XELOGD("EH: copied thrown {:08X} -> catchobj@{:08X}", val,
                   catchobj_addr);

            // ---- Second pass: run destructor (cleanup) funclets, curState down
            // to targetState, via the UnwindMap. UnwindMapEntry(8): toState(4),
            // action(4). Each cleanup funclet takes the establisher frame ptr in
            // r12. This runs BEFORE the catch body, so RAII locals (e.g. Guard)
            // are destroyed in the correct order.
            uint32_t unwind_map = rd32(funcinfo + 0x08);
            int32_t st = cur_state;
            for (int guard = 0; guard < 64 && st != target_state && st >= 0;
                 ++guard) {
              uint32_t ent = unwind_map + static_cast<uint32_t>(st) * 8;
              int32_t to_state = static_cast<int32_t>(rd32(ent));
              uint32_t action = rd32(ent + 4);
              st = to_state;  // advance before running (re-entrancy safe)
              if (action) {
                XELOGD("EH: unwind state -> action funclet@{:08X} (next={})",
                       action, st);
                ctx->r[12] = est;  // establisher frame pointer
                kernel_state()->processor()->Execute(thread->thread_state(),
                                                      action, nullptr, 0);
              }
            }

            // ---- Run the catch funclet; it returns the continuation IP ----
            ctx->r[12] = est;  // establisher frame pointer, in r12
            uint64_t cont = kernel_state()->processor()->Execute(
                thread->thread_state(), handler_addr, nullptr, 0);
            XELOGD("EH: funclet returned continuation IP {:08X}; resuming",
                   static_cast<uint32_t>(cont));

            // ---- Control transfer: abandon every frame between the throw and
            // this catch, and resume the guest in the catching function's body
            // at the continuation IP with its own register context. r1 = the
            // catching frame's working sp; r31 = its frame pointer (== working
            // sp for this prologue form). This is the console kernel's RtlUnwind
            // target-transfer; xenia realizes the non-local transfer with
            // XThread::Reenter (host longjmp/fiber reentry to the Execute loop).
            ctx->r[1] = sp;   // catching frame's stack pointer
            ctx->r[31] = sp;  // catching frame's frame pointer (r31 == sp here)
            thread->Reenter(static_cast<uint32_t>(cont));
            return true;  // unreached
          }
        }
      }
      XELOGD("EH: no matching catch in frame (n_try={} n_thrown={})", n_try,
             n_thrown);
      return false;
    }
    // Unwind one frame via the back-chain. Log LR-slot candidates so the run
    // tells us the correct MS-PPC offset (empirical; CallerSP-8 expected).
    uint32_t caller_sp = rd32(sp);
    if (!caller_sp || caller_sp <= sp) {
      XELOGD("EH: back-chain end (caller_sp={:08X})", caller_sp);
      break;
    }
    XELOGD("EH:   candidates caller_sp={:08X} LR@[csp-8]={:08X} [csp+4]={:08X} "
           "[sp+4]={:08X}",
           caller_sp, rd32(caller_sp - 8), rd32(caller_sp + 4), rd32(sp + 4));
    pc = rd32(caller_sp - 8);  // MS-PPC LR save slot at CallerSP-8 (to verify)
    sp = caller_sp;
  }
  return false;
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
