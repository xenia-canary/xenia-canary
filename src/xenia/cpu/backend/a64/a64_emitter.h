/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
#define XENIA_CPU_BACKEND_A64_A64_EMITTER_H_

#include <functional>
#include <unordered_map>
#include <vector>

#include "xenia/base/arena.h"
#include "xenia/cpu/backend/code_cache_base.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/function_trace_data.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

#include "xbyak_aarch64.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {
using namespace arm64;
class A64Backend;
class A64CodeCache;

enum class FPCRMode : uint32_t { Unknown, Fpu, Vmx };

// Unfortunately due to the design of xbyak we have to pass this to the ctor.
class XbyakA64Allocator : public Xbyak_aarch64::Allocator {
 public:
  virtual bool useProtect() const { return false; }
};

class A64Emitter;
using TailEmitCallback =
    std::function<void(A64Emitter& e, Xbyak_aarch64::Label& lbl)>;
struct TailEmitter {
  Xbyak_aarch64::Label label;
  uint32_t alignment;
  TailEmitCallback func;
};

class A64Emitter : public Xbyak_aarch64::CodeGenerator {
 public:
  A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator);
  virtual ~A64Emitter();

  Processor* processor() const { return processor_; }
  A64Backend* backend() const { return backend_; }

  bool Emit(GuestFunction* function, hir::HIRBuilder* builder,
            uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
            void** out_code_address, size_t* out_code_size,
            std::vector<SourceMapEntry>* out_source_map);

 public:
  // Reserved: sp, x19 (backend context), x20 (context), x21 (membase)
  // Scratch: x0-x18 (caller-saved), v0-v3
  // Available GPRs for register allocator: x22-x28
  static constexpr int GPR_COUNT = 7;
  // Available VEC regs: v4-v15, v16-v31
  static constexpr int VEC_COUNT = 28;
  static constexpr size_t kStashOffset = 32;

  static void SetupReg(const hir::Value* v, Xbyak_aarch64::WReg& r) {
    auto idx = gpr_reg_map_[v->reg.index];
    r = Xbyak_aarch64::WReg(idx);
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::XReg& r) {
    auto idx = gpr_reg_map_[v->reg.index];
    r = Xbyak_aarch64::XReg(idx);
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::SReg& r) {
    auto idx = vec_reg_map_[v->reg.index];
    r = Xbyak_aarch64::SReg(idx);
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::DReg& r) {
    auto idx = vec_reg_map_[v->reg.index];
    r = Xbyak_aarch64::DReg(idx);
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::QReg& r) {
    auto idx = vec_reg_map_[v->reg.index];
    r = Xbyak_aarch64::QReg(idx);
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::VReg& r) {
    auto idx = vec_reg_map_[v->reg.index];
    r = Xbyak_aarch64::VReg(idx);
  }

  Xbyak_aarch64::Label& epilog_label() { return *epilog_label_; }

  FunctionDebugInfo* debug_info() const { return debug_info_; }
  size_t stack_size() const { return stack_size_; }

  void MarkSourceOffset(const hir::Instr* i);

  void DebugBreak();
  void Trap(uint16_t trap_type = 0);
  void UnimplementedInstr(const hir::Instr* i);

  void Call(const hir::Instr* instr, GuestFunction* function);
  void CallIndirect(const hir::Instr* instr, int reg_index);
  void CallExtern(const hir::Instr* instr, const Function* function);
  void CallNative(void* fn);
  void CallNativeSafe(void* fn);
  void SetReturnAddress(uint64_t value);

  // Backend context register = x19.
  // Points to A64BackendContext (immediately before PPCContext in memory).
  const Xbyak_aarch64::XReg& GetBackendCtxReg() const { return x19; }
  // Context register = x20.
  const Xbyak_aarch64::XReg& GetContextReg() const { return x20; }
  // Memory base register = x21.
  const Xbyak_aarch64::XReg& GetMembaseReg() const { return x21; }

  void ReloadMembase();
  void EmitIndirectionLookup();

  void PushStackpoint();
  void PopStackpoint();
  void EnsureSynchronizedGuestAndHostStack();

  static void HandleStackpointOverflowError(ppc::PPCContext* context);

  void ForgetFpcrMode() {
    if (fpcr_mode_ == FPCRMode::Vmx) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = FPCRMode::Unknown;
  }
  bool ChangeFpcrMode(FPCRMode new_mode, bool already_set = false);
  bool IsFeatureEnabled(uint64_t feature_flag) const {
    return (feature_flags_ & feature_flag) == feature_flag;
  }

  Xbyak_aarch64::Label& AddToTail(TailEmitCallback callback,
                                  uint32_t alignment = 0);
  Xbyak_aarch64::Label& NewCachedLabel();

  // Get or create a xbyak_aarch64 label for a HIR label ID.
  Xbyak_aarch64::Label& GetLabel(uint32_t label_id);

  XexModule* GuestModule() { return guest_module_; }

 protected:
  void* Emplace(const EmitFunctionInfo& func_info,
                GuestFunction* function = nullptr);
  bool Emit(hir::HIRBuilder* builder, EmitFunctionInfo& func_info);

 protected:
  Processor* processor_ = nullptr;
  A64Backend* backend_ = nullptr;
  A64CodeCache* code_cache_ = nullptr;
  XbyakA64Allocator* allocator_ = nullptr;
  XexModule* guest_module_ = nullptr;
  uint64_t feature_flags_ = 0;
  uint32_t current_guest_function_ = 0;

  Xbyak_aarch64::Label* epilog_label_ = nullptr;

  hir::Instr* current_instr_ = nullptr;

  FunctionDebugInfo* debug_info_ = nullptr;
  uint32_t debug_info_flags_ = 0;
  FunctionTraceData* trace_data_ = nullptr;
  Arena source_map_arena_;

  size_t stack_size_ = 0;

  static const uint32_t gpr_reg_map_[GPR_COUNT];
  static const uint32_t vec_reg_map_[VEC_COUNT];

  std::vector<TailEmitter> tail_code_;
  std::vector<Xbyak_aarch64::Label*> label_cache_;

  // Map from HIR label IDs to xbyak_aarch64 Labels.
  std::unordered_map<uint32_t, Xbyak_aarch64::Label*> label_map_;

  FPCRMode fpcr_mode_ = FPCRMode::Unknown;
  bool synchronize_stack_on_next_instruction_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
