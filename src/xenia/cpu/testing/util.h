/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_TESTING_UTIL_H_
#define XENIA_CPU_TESTING_UTIL_H_

#include <vector>

#include "xenia/base/platform.h"
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/test_module.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace testing {

using xe::cpu::ppc::PPCContext;

class TestFunction {
 public:
  TestFunction(std::function<void(hir::HIRBuilder& b)> generator) {
    memory.reset(new Memory());
    memory->Initialize();

    {
      std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
      backend.reset(new xe::cpu::backend::x64::X64Backend());
#elif XE_ARCH_ARM64
      backend.reset(new xe::cpu::backend::a64::A64Backend());
#endif  // XE_ARCH
      if (backend) {
        auto processor = std::make_unique<Processor>(memory.get(), nullptr);
        processor->Setup(std::move(backend));
        processors.emplace_back(std::move(processor));
      }
    }

    for (auto& processor : processors) {
      auto module = std::make_unique<xe::cpu::TestModule>(
          processor.get(), "Test",
          [](uint64_t address) { return address == 0x80000000; },
          [generator](hir::HIRBuilder& b) {
            generator(b);
            return true;
          });
      processor->AddModule(std::move(module));
      processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);
    }
  }

  ~TestFunction() {
    processors.clear();
    memory.reset();
  }

  void Run(std::function<void(PPCContext*)> pre_call,
           std::function<void(PPCContext*)> post_call) {
    for (auto& processor : processors) {
      auto fn = processor->ResolveFunction(0x80000000);

      uint32_t stack_size = 64 * 1024;
      uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
      uint32_t stack_base = stack_address + stack_size;
      auto thread_state =
          std::make_unique<ThreadState>(processor.get(), 0x100, stack_base);
      auto ctx = thread_state->context();
      ctx->lr = 0xBCBCBCBC;

      // Reset hardware FPU rounding mode to nearest before each run,
      // in case a prior test left it dirty.
      processor->backend()->SetGuestRoundingMode(ctx, 0);

      pre_call(ctx);

      fn->Call(thread_state.get(), uint32_t(ctx->lr));

      post_call(ctx);

      thread_state.reset();
      memory->SystemHeapFree(stack_address);
    }
  }

  std::unique_ptr<Memory> memory;
  std::vector<std::unique_ptr<Processor>> processors;
};

inline hir::Value* LoadGPR(hir::HIRBuilder& b, int reg) {
  return b.LoadContext(offsetof(PPCContext, r) + reg * 8, hir::INT64_TYPE);
}
inline void StoreGPR(hir::HIRBuilder& b, int reg, hir::Value* value) {
  b.StoreContext(offsetof(PPCContext, r) + reg * 8, value);
}

inline hir::Value* LoadFPR(hir::HIRBuilder& b, int reg) {
  return b.LoadContext(offsetof(PPCContext, f) + reg * 8, hir::FLOAT64_TYPE);
}
inline void StoreFPR(hir::HIRBuilder& b, int reg, hir::Value* value) {
  b.StoreContext(offsetof(PPCContext, f) + reg * 8, value);
}

inline hir::Value* LoadVR(hir::HIRBuilder& b, int reg) {
  return b.LoadContext(offsetof(PPCContext, v) + reg * 16, hir::VEC128_TYPE);
}
inline void StoreVR(hir::HIRBuilder& b, int reg, hir::Value* value) {
  b.StoreContext(offsetof(PPCContext, v) + reg * 16, value);
}

// Runs the op twice and requires a match. Operands from the guest context
// stay opaque and the backend emits it. Operands as HIR constants reach
// the folder. When another operand keeps the op unfolded they instead
// reach the emitter's constant path. Give inputs distinct lanes or a
// wrong-lane fold will not show. gprs seed r5 upward for both runs.
template <typename T>
inline void RequireFoldMatchesBackend(
    const std::vector<vec128_t>& inputs, const std::vector<uint64_t>& gprs,
    std::function<hir::Value*(hir::HIRBuilder&,
                              const std::vector<hir::Value*>&)>
        build,
    std::function<void(hir::HIRBuilder&, hir::Value*)> store,
    std::function<T(PPCContext*)> read) {
  T from_backend{};
  TestFunction([&](hir::HIRBuilder& b) {
    std::vector<hir::Value*> ops;
    for (size_t n = 0; n < inputs.size(); ++n) {
      ops.push_back(LoadVR(b, static_cast<int>(4 + n)));
    }
    store(b, build(b, ops));
    b.Return();
  })
      .Run(
          [&](PPCContext* ctx) {
            for (size_t n = 0; n < inputs.size(); ++n) {
              ctx->v[4 + n] = inputs[n];
            }
            for (size_t n = 0; n < gprs.size(); ++n) {
              ctx->r[5 + n] = gprs[n];
            }
          },
          [&](PPCContext* ctx) { from_backend = read(ctx); });

  T from_fold{};
  TestFunction([&](hir::HIRBuilder& b) {
    std::vector<hir::Value*> ops;
    for (const vec128_t& input : inputs) {
      ops.push_back(b.LoadConstantVec128(input));
    }
    store(b, build(b, ops));
    b.Return();
  })
      .Run(
          [&](PPCContext* ctx) {
            for (size_t n = 0; n < gprs.size(); ++n) {
              ctx->r[5 + n] = gprs[n];
            }
          },
          [&](PPCContext* ctx) { from_fold = read(ctx); });

  REQUIRE(from_fold == from_backend);
}

// Result goes to VR3.
inline void RequireVectorFoldMatchesBackend(
    const std::vector<vec128_t>& inputs,
    std::function<hir::Value*(hir::HIRBuilder&,
                              const std::vector<hir::Value*>&)>
        build,
    const std::vector<uint64_t>& gprs = {}) {
  RequireFoldMatchesBackend<vec128_t>(
      inputs, gprs, build,
      [](hir::HIRBuilder& b, hir::Value* v) { StoreVR(b, 3, v); },
      [](PPCContext* ctx) { return ctx->v[3]; });
}

// Result is zero extended into GPR3.
inline void RequireScalarFoldMatchesBackend(
    const std::vector<vec128_t>& inputs,
    std::function<hir::Value*(hir::HIRBuilder&,
                              const std::vector<hir::Value*>&)>
        build,
    const std::vector<uint64_t>& gprs = {}) {
  RequireFoldMatchesBackend<uint64_t>(
      inputs, gprs, build,
      [](hir::HIRBuilder& b, hir::Value* v) {
        StoreGPR(b, 3, b.ZeroExtend(v, hir::INT64_TYPE));
      },
      [](PPCContext* ctx) { return ctx->r[3]; });
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_TESTING_UTIL_H_
