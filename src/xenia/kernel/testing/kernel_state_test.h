/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_TESTING_KERNEL_STATE_TEST_H_
#define XENIA_KERNEL_TESTING_KERNEL_STATE_TEST_H_

#include <memory>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/platform.h"
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/vfs/virtual_file_system.h"

namespace xe {
namespace kernel {
namespace test {

// Real memory, CPU and kernel globals, without Emulator UI or persistent data.
class KernelStateTest {
 public:
  KernelStateTest() : processor_(&memory_, &export_resolver_) {
    REQUIRE(memory_.Initialize());
    std::unique_ptr<cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
    backend = std::make_unique<cpu::backend::x64::X64Backend>();
#elif XE_ARCH_ARM64
    backend = std::make_unique<cpu::backend::a64::A64Backend>();
#endif
    REQUIRE(backend != nullptr);
    REQUIRE(processor_.Setup(std::move(backend)));
    kernel_state_.reset(new KernelState(&memory_, &processor_, &file_system_));
  }

  Memory* memory() { return &memory_; }
  cpu::Processor* processor() { return &processor_; }
  KernelState* kernel_state() { return kernel_state_.get(); }

 private:
  Memory memory_;
  cpu::ExportResolver export_resolver_;
  cpu::Processor processor_;
  vfs::VirtualFileSystem file_system_;
  std::unique_ptr<KernelState> kernel_state_;
};

}  // namespace test
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_TESTING_KERNEL_STATE_TEST_H_
