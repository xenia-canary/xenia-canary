/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "xenia/base/platform.h"

#if XE_ARCH_AMD64
#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/capstone/include/capstone/x86.h"
#endif  // XE_ARCH_AMD64

#include "xenia/cpu/function.h"
#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;

#if XE_ARCH_AMD64

namespace {

enum class Float16_2CodegenKernel { kPack, kUnpack };

constexpr uint32_t kRegionBeginGuestAddress = 0x80000000;
constexpr uint32_t kRegionEndGuestAddress = 0x80000004;

struct CodegenStats {
  size_t region_byte_count;
  size_t region_instruction_count;
  size_t region_call_count;
  size_t function_byte_count;
};

CodegenStats AnalyzeFloat16_2Codegen(Float16_2CodegenKernel kernel) {
  TestFunction test([kernel](HIRBuilder& b) {
    auto* source = LoadVR(b, 4);
    b.SourceOffset(kRegionBeginGuestAddress);
    auto* result = kernel == Float16_2CodegenKernel::kPack
                       ? b.Pack(source, PACK_TYPE_FLOAT16_2)
                       : b.Unpack(source, PACK_TYPE_FLOAT16_2);
    b.SourceOffset(kRegionEndGuestAddress);
    StoreVR(b, 3, result);
    b.Return();
  });
  REQUIRE(test.processors.size() == 1);

  auto* function = static_cast<GuestFunction*>(
      test.processors.front()->ResolveFunction(0x80000000));
  REQUIRE(function != nullptr);
  REQUIRE(function->machine_code() != nullptr);

  const SourceMapEntry* region_begin =
      function->LookupGuestAddress(kRegionBeginGuestAddress);
  const SourceMapEntry* region_end =
      function->LookupGuestAddress(kRegionEndGuestAddress);
  REQUIRE(region_begin != nullptr);
  REQUIRE(region_end != nullptr);
  REQUIRE(region_end->code_offset > region_begin->code_offset);
  REQUIRE(region_end->code_offset <= function->machine_code_length());

  const size_t region_byte_count =
      region_end->code_offset - region_begin->code_offset;
  const uint8_t* region_code =
      function->machine_code() + region_begin->code_offset;

  csh capstone_handle = 0;
  REQUIRE(cs_open(CS_ARCH_X86, CS_MODE_64, &capstone_handle) == CS_ERR_OK);
  cs_option(capstone_handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
  cs_option(capstone_handle, CS_OPT_DETAIL, CS_OPT_OFF);

  cs_insn* instructions = nullptr;
  const size_t instruction_count =
      cs_disasm(capstone_handle, region_code, region_byte_count,
                reinterpret_cast<uint64_t>(region_code), 0, &instructions);
  if (instruction_count == 0) {
    cs_close(&capstone_handle);
    REQUIRE(instruction_count != 0);
  }

  size_t call_count = 0;
  size_t decoded_byte_count = 0;
  for (size_t i = 0; i < instruction_count; ++i) {
    call_count += instructions[i].id == X86_INS_CALL ? 1 : 0;
    decoded_byte_count += instructions[i].size;
  }
  cs_free(instructions, instruction_count);
  cs_close(&capstone_handle);
  REQUIRE(decoded_byte_count == region_byte_count);

  return CodegenStats{region_byte_count, instruction_count, call_count,
                      function->machine_code_length()};
}

void PrintCodegenStats(const char* kernel_name, const CodegenStats& stats) {
  std::printf(
      "FLOAT16_2 codegen %-6s region_bytes=%zu instructions=%zu calls=%zu "
      "function_bytes=%zu\n",
      kernel_name, stats.region_byte_count, stats.region_instruction_count,
      stats.region_call_count, stats.function_byte_count);
}

}  // namespace

TEST_CASE("FLOAT16_2_EMITTER_CODEGEN", "[.float16_2_codegen]") {
  const CodegenStats pack =
      AnalyzeFloat16_2Codegen(Float16_2CodegenKernel::kPack);
  const CodegenStats unpack =
      AnalyzeFloat16_2Codegen(Float16_2CodegenKernel::kUnpack);

  PrintCodegenStats("PACK", pack);
  PrintCodegenStats("UNPACK", unpack);

  REQUIRE(pack.region_call_count == 0);
  REQUIRE(unpack.region_call_count == 0);
}

#endif  // XE_ARCH_AMD64
