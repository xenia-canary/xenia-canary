/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <array>

#include "xenia/base/platform.h"
#if XE_ARCH_AMD64
#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/capstone/include/capstone/x86.h"
#include "xenia/cpu/function.h"
#endif  // XE_ARCH_AMD64

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

#if XE_ARCH_AMD64
namespace {

size_t CountCallsInRegion(TestFunction& test, uint32_t begin_address,
                          uint32_t end_address) {
  REQUIRE(test.processors.size() == 1);
  auto* function = static_cast<GuestFunction*>(
      test.processors.front()->ResolveFunction(0x80000000));
  REQUIRE(function != nullptr);
  REQUIRE(function->machine_code() != nullptr);
  const SourceMapEntry* begin = function->LookupGuestAddress(begin_address);
  const SourceMapEntry* end = function->LookupGuestAddress(end_address);
  REQUIRE(begin != nullptr);
  REQUIRE(end != nullptr);
  REQUIRE(end->code_offset > begin->code_offset);
  REQUIRE(end->code_offset <= function->machine_code_length());

  const size_t byte_count = end->code_offset - begin->code_offset;
  const uint8_t* code = function->machine_code() + begin->code_offset;
  csh handle = 0;
  REQUIRE(cs_open(CS_ARCH_X86, CS_MODE_64, &handle) == CS_ERR_OK);
  cs_insn* instructions = nullptr;
  const size_t instruction_count =
      cs_disasm(handle, code, byte_count, 0, 0, &instructions);
  size_t decoded_bytes = 0;
  size_t call_count = 0;
  for (size_t i = 0; i < instruction_count; ++i) {
    decoded_bytes += instructions[i].size;
    call_count += instructions[i].id == X86_INS_CALL;
  }
  if (instructions != nullptr) {
    cs_free(instructions, instruction_count);
  }
  cs_close(&handle);

  REQUIRE(instruction_count > 0);
  REQUIRE(decoded_bytes == byte_count);
  return call_count;
}

}  // namespace
#endif  // XE_ARCH_AMD64

TEST_CASE("PACK_D3DCOLOR", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_D3DCOLOR));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128f(1.0f); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(0));
           });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x40400050, 0x40400060, 0x40400070, 0x40400080);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0x80506070));
      });
}

TEST_CASE("PACK_FLOAT16_2", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_FLOAT16_2));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0x3F800000); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(0));
           });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x47FFE000, 0xC7FFE000, 0x00000000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0x7FFFFFFF));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x42AAA000, 0x44CCC000, 0x00000000, 0x3F800000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0x55556666));
      });
}

TEST_CASE("PACK_FLOAT16_4", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_FLOAT16_4));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(0));
           });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x449A4000, 0x45B17000, 0x41103261, 0x40922B6B);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x00000000, 0x00000000, 0x64D26D8C, 0x48824491));
      });
}

TEST_CASE("PACK_SHORT_2", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_SHORT_2));
    b.Return();
  });
  // SHORT_2 operates on pre-biased floats near 3.0 (0x40400000 = short 0)
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x40400000, 0x40400000, 0, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x43817E00, 0xC37CFC00, 0, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0x7FFF8001));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0xC0D47D97, 0xC2256E9D, 0, 0);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0x80018001));
      });
}

TEST_CASE("PACK_UINT_2101010", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_UINT_2101010));
    b.Return();
  });
  // All magic-zero: XYZ and W all at base → packed=0
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x40400000, 0x40400000, 0x40400000, 0x40400000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0));
      });
  // x=100, y=200, z=3, w=2 → packed=0x80332064
  // The packed result is defined in u32[3]; other lanes are don't-care
  // (vpkd3d128 permutes the packed value into the correct position).
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x40400064, 0x404000C8, 0x40400003, 0x40400002);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3].u32[3] == 0x80332064); });
  // x=-100 (0x39C), y=50, z=-1 (0x3FF), w=3 → packed=0xFFF0CB9C
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x403FFF9C, 0x40400032, 0x403FFFFF, 0x40400003);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3].u32[3] == 0xFFF0CB9C); });
}

TEST_CASE("PACK_ULONG_4202020", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_ULONG_4202020));
    b.Return();
  });
  // All magic-zero: → packed=0
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x40400000, 0x40400000, 0x40400000, 0x40400000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128i(0, 0, 0, 0));
      });
  // x=1000, y=2000, z=100, w=5
  // packed=0x500064007D0003E8, u32[2]=high, u32[3]=low
  // Only u32[2]:u32[3] are defined (64-bit packed result).
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x404003E8, 0x404007D0, 0x40400064, 0x40400005);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[2] == 0x50006400);
        REQUIRE(ctx->v[3].u32[3] == 0x7D0003E8);
      });
  // Negative x=-100, y=50, z=-1, w=10
  // packed64=0xAFFFFF00032FFF9C, u32[2]=0xAFFFFF00, u32[3]=0x032FFF9C
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x403FFF9C, 0x40400032, 0x403FFFFF, 0x4040000A);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[2] == 0xAFFFFF00);
        REQUIRE(ctx->v[3].u32[3] == 0x032FFF9C);
      });
}

TEST_CASE("PACK_8_IN_16_UNSIGNED", "[instr]") {
  const vec128_t src1 =
      vec128s(0x0000, 0x0001, 0x007F, 0x0080, 0x00FE, 0x00FF, 0x0100, 0xFFFF);
  const vec128_t src2 =
      vec128s(0x0101, 0x01FF, 0x1234, 0x7FFF, 0x8000, 0xABCD, 0xFF00, 0xFFFE);
  const vec128_t expected_modulo =
      vec128b(0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0x00, 0xFF, 0x01, 0xFF, 0x34,
              0xFF, 0x00, 0xCD, 0x00, 0xFE);
  const vec128_t expected_saturating =
      vec128b(0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
              0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
  constexpr uint32_t kModuloFlags = PACK_TYPE_8_IN_16 | PACK_TYPE_IN_UNSIGNED |
                                    PACK_TYPE_OUT_UNSIGNED |
                                    PACK_TYPE_OUT_UNSATURATE;
  constexpr uint32_t kSaturatingFlags =
      PACK_TYPE_8_IN_16 | PACK_TYPE_IN_UNSIGNED | PACK_TYPE_OUT_UNSIGNED |
      PACK_TYPE_OUT_SATURATE;
  constexpr uint32_t kPackCodegenBegin = 0x80000000;
  constexpr uint32_t kPackCodegenEnd = 0x80000004;

  {
    TestFunction test([=](HIRBuilder& b) {
      auto* dynamic_src1 = LoadVR(b, 4);
      auto* dynamic_src2 = LoadVR(b, 5);
      auto* constant_src1 = b.LoadConstantVec128(src1);
      auto* constant_src2 = b.LoadConstantVec128(src2);

      b.SourceOffset(kPackCodegenBegin);
      auto* dynamic_modulo = b.Pack(dynamic_src1, dynamic_src2, kModuloFlags);
      auto* dynamic_saturating =
          b.Pack(dynamic_src1, dynamic_src2, kSaturatingFlags);
      auto* constant_src1_modulo =
          b.Pack(constant_src1, dynamic_src2, kModuloFlags);
      auto* constant_src2_modulo =
          b.Pack(dynamic_src1, constant_src2, kModuloFlags);
      auto* constant_src1_saturating =
          b.Pack(constant_src1, dynamic_src2, kSaturatingFlags);
      auto* constant_src2_saturating =
          b.Pack(dynamic_src1, constant_src2, kSaturatingFlags);
      b.SourceOffset(kPackCodegenEnd);

      StoreVR(b, 3, dynamic_modulo);
      StoreVR(b, 6, dynamic_saturating);
      StoreVR(b, 7, constant_src1_modulo);
      StoreVR(b, 8, constant_src2_modulo);
      StoreVR(b, 9, constant_src1_saturating);
      StoreVR(b, 10, constant_src2_saturating);
      b.Return();
    });

    test.Run(
        [=](PPCContext* ctx) {
          ctx->v[4] = src1;
          ctx->v[5] = src2;
        },
        [=](PPCContext* ctx) {
          REQUIRE(ctx->v[3] == expected_modulo);
          REQUIRE(ctx->v[6] == expected_saturating);
          REQUIRE(ctx->v[7] == expected_modulo);
          REQUIRE(ctx->v[8] == expected_modulo);
          REQUIRE(ctx->v[9] == expected_saturating);
          REQUIRE(ctx->v[10] == expected_saturating);
        });

#if XE_ARCH_AMD64
    REQUIRE(CountCallsInRegion(test, kPackCodegenBegin, kPackCodegenEnd) == 0);
#endif  // XE_ARCH_AMD64
  }

#if XE_ARCH_AMD64
  constexpr size_t kSurvivorCount = 12;
  std::array<vec128_t, kSurvivorCount> sentinels{};
  for (size_t i = 0; i < sentinels.size(); ++i) {
    const uint32_t value = 0x11000000u + static_cast<uint32_t>(i);
    sentinels[i] =
        vec128i(value, value + 0x100u, value + 0x200u, value + 0x300u);
  }

  TestFunction pressure_test([=](HIRBuilder& b) {
    auto* modulo_src1 = LoadVR(b, 4);
    auto* modulo_src2 = LoadVR(b, 5);
    auto* saturating_src1 = LoadVR(b, 4);
    auto* saturating_src2 = LoadVR(b, 5);
    std::array<Value*, kSurvivorCount> survivors{};
    for (size_t i = 0; i < survivors.size(); ++i) {
      survivors[i] = LoadVR(b, 16 + static_cast<int>(i));
    }

    auto* modulo = b.Pack(modulo_src1, modulo_src2, kModuloFlags);
    auto* saturating =
        b.Pack(saturating_src1, saturating_src2, kSaturatingFlags);

    StoreVR(b, 11, modulo);
    StoreVR(b, 12, saturating);
    StoreVR(b, 13, modulo_src2);
    StoreVR(b, 14, saturating_src1);
    for (size_t i = 0; i < survivors.size(); ++i) {
      StoreVR(b, 32 + static_cast<int>(i), survivors[i]);
    }
    b.Return();
  });

  pressure_test.Run(
      [=](PPCContext* ctx) {
        ctx->v[4] = src1;
        ctx->v[5] = src2;
        for (size_t i = 0; i < sentinels.size(); ++i) {
          ctx->v[16 + i] = sentinels[i];
        }
      },
      [=](PPCContext* ctx) {
        REQUIRE(ctx->v[11] == expected_modulo);
        REQUIRE(ctx->v[12] == expected_saturating);
        REQUIRE(ctx->v[13] == src2);
        REQUIRE(ctx->v[14] == src1);
        for (size_t i = 0; i < sentinels.size(); ++i) {
          REQUIRE(ctx->v[32 + i] == sentinels[i]);
        }
      });
#endif  // XE_ARCH_AMD64
}
