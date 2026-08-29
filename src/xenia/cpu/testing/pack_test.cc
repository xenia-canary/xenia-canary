/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <array>
#include <cstdint>
#include <cstdio>

#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

enum class Pack8Mode {
  kUnsignedModulo,
  kUnsignedSaturate,
  kSignedUnsignedSaturate,
  kSignedSignedSaturate,
};

constexpr size_t kPack8ExhaustiveInvocationCount = 1u << 16;
constexpr size_t kPack8LaneCount = 16;

uint32_t Pack8Flags(Pack8Mode mode) {
  switch (mode) {
    case Pack8Mode::kUnsignedModulo:
      return PACK_TYPE_8_IN_16 | PACK_TYPE_IN_UNSIGNED |
             PACK_TYPE_OUT_UNSIGNED | PACK_TYPE_OUT_UNSATURATE;
    case Pack8Mode::kUnsignedSaturate:
      return PACK_TYPE_8_IN_16 | PACK_TYPE_IN_UNSIGNED |
             PACK_TYPE_OUT_UNSIGNED | PACK_TYPE_OUT_SATURATE;
    case Pack8Mode::kSignedUnsignedSaturate:
      return PACK_TYPE_8_IN_16 | PACK_TYPE_IN_SIGNED | PACK_TYPE_OUT_UNSIGNED |
             PACK_TYPE_OUT_SATURATE;
    case Pack8Mode::kSignedSignedSaturate:
      return PACK_TYPE_8_IN_16 | PACK_TYPE_IN_SIGNED | PACK_TYPE_OUT_SIGNED |
             PACK_TYPE_OUT_SATURATE;
  }
  assert_unhandled_case(mode);
  return 0;
}

const char* Pack8ModeName(Pack8Mode mode) {
  switch (mode) {
    case Pack8Mode::kUnsignedModulo:
      return "unsigned modulo";
    case Pack8Mode::kUnsignedSaturate:
      return "unsigned saturate";
    case Pack8Mode::kSignedUnsignedSaturate:
      return "signed to unsigned saturate";
    case Pack8Mode::kSignedSignedSaturate:
      return "signed saturate";
  }
  assert_unhandled_case(mode);
  return "unknown";
}

uint8_t ReferencePack8Lane(uint16_t raw, Pack8Mode mode) {
  switch (mode) {
    case Pack8Mode::kUnsignedModulo:
      return static_cast<uint8_t>(raw);
    case Pack8Mode::kUnsignedSaturate:
      return raw > 255u ? 255u : static_cast<uint8_t>(raw);
    case Pack8Mode::kSignedUnsignedSaturate: {
      const int32_t value = raw < 0x8000u ? static_cast<int32_t>(raw)
                                          : static_cast<int32_t>(raw) - 0x10000;
      if (value <= 0) {
        return 0;
      }
      return value > 255 ? 255u : static_cast<uint8_t>(value);
    }
    case Pack8Mode::kSignedSignedSaturate: {
      int32_t value = raw < 0x8000u ? static_cast<int32_t>(raw)
                                    : static_cast<int32_t>(raw) - 0x10000;
      if (value < -128) {
        value = -128;
      } else if (value > 127) {
        value = 127;
      }
      return value < 0 ? static_cast<uint8_t>(256 + value)
                       : static_cast<uint8_t>(value);
    }
  }
  assert_unhandled_case(mode);
  return 0;
}

// Independent guest-lane reference. Within each 32-bit guest word, halfword
// lane g is host u16[g ^ 1], while byte lane g is host u8[g ^ 3]. The result
// concatenates the eight narrowed VA lanes and then the eight VB lanes.
vec128_t ReferencePack8In16(const vec128_t& src1, const vec128_t& src2,
                            Pack8Mode mode) {
  vec128_t result = {};
  for (size_t guest_lane = 0; guest_lane < 8; ++guest_lane) {
    result.u8[guest_lane ^ 3] =
        ReferencePack8Lane(src1.u16[guest_lane ^ 1], mode);
    result.u8[(guest_lane + 8) ^ 3] =
        ReferencePack8Lane(src2.u16[guest_lane ^ 1], mode);
  }
  return result;
}

void MakePack8ExhaustiveSources(size_t seed, vec128_t& src1, vec128_t& src2) {
  src1 = {};
  src2 = {};
  for (size_t lane = 0; lane < 8; ++lane) {
    src1.u16[lane] = static_cast<uint16_t>(seed + lane * uint32_t{0x1111});
    src2.u16[lane] =
        static_cast<uint16_t>(seed + (lane + 8) * uint32_t{0x1111});
  }
}

void RunPack8In16Exhaustive(Pack8Mode mode) {
  const uint32_t flags = Pack8Flags(mode);
  TestFunction test([flags](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), LoadVR(b, 5), flags));
    b.Return();
  });

  size_t invocation_count = 0;
  size_t lane_comparison_count = 0;
  size_t mismatch_count = 0;
  size_t first_mismatch_seed = 0;
  size_t first_mismatch_host_byte = 0;
  uint32_t first_expected = 0;
  uint32_t first_actual = 0;

  test.RunRepeated(
      kPack8ExhaustiveInvocationCount,
      [](PPCContext* ctx, size_t seed) {
        MakePack8ExhaustiveSources(seed, ctx->v[4], ctx->v[5]);
        ctx->v[3] = vec128b(0xCD);
      },
      [&](PPCContext* ctx, size_t seed) {
        vec128_t src1;
        vec128_t src2;
        MakePack8ExhaustiveSources(seed, src1, src2);
        const vec128_t expected = ReferencePack8In16(src1, src2, mode);
        for (size_t host_byte = 0; host_byte < kPack8LaneCount; ++host_byte) {
          if (ctx->v[3].u8[host_byte] != expected.u8[host_byte]) {
            if (mismatch_count == 0) {
              first_mismatch_seed = seed;
              first_mismatch_host_byte = host_byte;
              first_expected = expected.u8[host_byte];
              first_actual = ctx->v[3].u8[host_byte];
            }
            ++mismatch_count;
          }
        }
        ++invocation_count;
        lane_comparison_count += kPack8LaneCount;
      });

  std::printf(
      "PACK_8_IN_16 %s exhaustive: %zu emitted-code calls, %zu lane "
      "conversions\n",
      Pack8ModeName(mode), invocation_count, lane_comparison_count);
  REQUIRE(invocation_count == kPack8ExhaustiveInvocationCount);
  REQUIRE(invocation_count == 65536);
  REQUIRE(lane_comparison_count ==
          kPack8ExhaustiveInvocationCount * kPack8LaneCount);
  REQUIRE(lane_comparison_count == 1048576);
  CAPTURE(first_mismatch_seed, first_mismatch_host_byte, first_expected,
          first_actual);
  REQUIRE(mismatch_count == 0);
}

}  // namespace

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

TEST_CASE("PACK_8_IN_16_GUEST_ORDER_ANCHOR", "[instr]") {
  const vec128_t src1 = vec128i(0x00000001, 0x00020003, 0x00040005, 0x00060007);
  const vec128_t src2 = vec128i(0x00080009, 0x000A000B, 0x000C000D, 0x000E000F);
  const vec128_t literal_expected =
      vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Pack(LoadVR(b, 4), LoadVR(b, 5),
                   Pack8Flags(Pack8Mode::kUnsignedModulo)));
    b.Return();
  });

  REQUIRE(ReferencePack8In16(src1, src2, Pack8Mode::kUnsignedModulo) ==
          literal_expected);
  test.Run(
      [src1, src2](PPCContext* ctx) {
        ctx->v[4] = src1;
        ctx->v[5] = src2;
      },
      [literal_expected](PPCContext* ctx) {
        REQUIRE(ctx->v[3] == literal_expected);
      });
}

TEST_CASE("PACK_8_IN_16_UNSIGNED_MODULO_EXHAUSTIVE", "[instr]") {
  RunPack8In16Exhaustive(Pack8Mode::kUnsignedModulo);
}

TEST_CASE("PACK_8_IN_16_UNSIGNED_SATURATE_EXHAUSTIVE", "[instr]") {
  RunPack8In16Exhaustive(Pack8Mode::kUnsignedSaturate);
}

TEST_CASE("PACK_8_IN_16_SIGNED_UNSIGNED_SATURATE_EXHAUSTIVE", "[instr]") {
  RunPack8In16Exhaustive(Pack8Mode::kSignedUnsignedSaturate);
}

TEST_CASE("PACK_8_IN_16_SIGNED_SATURATE_EXHAUSTIVE", "[instr]") {
  RunPack8In16Exhaustive(Pack8Mode::kSignedSignedSaturate);
}

TEST_CASE("PACK_8_IN_16_UNSIGNED_CONSTANT_OPERANDS", "[instr]") {
  const vec128_t constant_src1 =
      vec128s(0x0000, 0x0001, 0x007F, 0x0080, 0x00FF, 0x0100, 0x7FFF, 0xFFFF);
  const vec128_t constant_src2 =
      vec128s(0xFFFF, 0xFF00, 0x01FF, 0x0101, 0x0100, 0x00FE, 0x0080, 0x0001);
  const vec128_t constant_zero = vec128b(0);
  const vec128_t constant_ones = vec128b(0xFF);
  const uint32_t modulo_flags = Pack8Flags(Pack8Mode::kUnsignedModulo);
  const uint32_t saturate_flags = Pack8Flags(Pack8Mode::kUnsignedSaturate);
  TestFunction test([=](HIRBuilder& b) {
    auto* dynamic_src1 = LoadVR(b, 20);
    auto* dynamic_src2 = LoadVR(b, 21);
    auto* const_src1 = b.LoadConstantVec128(constant_src1);
    auto* const_src2 = b.LoadConstantVec128(constant_src2);
    auto* const_zero = b.LoadConstantVec128(constant_zero);
    auto* const_ones = b.LoadConstantVec128(constant_ones);

    StoreVR(b, 3, b.Pack(const_src1, dynamic_src2, modulo_flags));
    StoreVR(b, 4, b.Pack(dynamic_src1, const_src2, modulo_flags));
    StoreVR(b, 5, b.Pack(const_zero, const_ones, modulo_flags));
    StoreVR(b, 6, b.Pack(dynamic_src1, dynamic_src1, modulo_flags));
    StoreVR(b, 7, b.Pack(const_src1, dynamic_src2, saturate_flags));
    StoreVR(b, 8, b.Pack(dynamic_src1, const_src2, saturate_flags));
    StoreVR(b, 9, b.Pack(const_zero, const_ones, saturate_flags));
    StoreVR(b, 10, b.Pack(dynamic_src2, dynamic_src2, saturate_flags));
    StoreVR(b, 22, dynamic_src1);
    StoreVR(b, 23, dynamic_src2);
    b.Return();
  });

  const vec128_t dynamic_src1 =
      vec128s(0xABCD, 0x1234, 0x0100, 0x00FF, 0x00AA, 0x5500, 0x0000, 0xFFFF);
  const vec128_t dynamic_src2 =
      vec128s(0x1357, 0x2468, 0x0001, 0x00FE, 0x0101, 0x7FFF, 0xFF00, 0x0080);
  const std::array<vec128_t, 8> expected = {
      ReferencePack8In16(constant_src1, dynamic_src2,
                         Pack8Mode::kUnsignedModulo),
      ReferencePack8In16(dynamic_src1, constant_src2,
                         Pack8Mode::kUnsignedModulo),
      ReferencePack8In16(constant_zero, constant_ones,
                         Pack8Mode::kUnsignedModulo),
      ReferencePack8In16(dynamic_src1, dynamic_src1,
                         Pack8Mode::kUnsignedModulo),
      ReferencePack8In16(constant_src1, dynamic_src2,
                         Pack8Mode::kUnsignedSaturate),
      ReferencePack8In16(dynamic_src1, constant_src2,
                         Pack8Mode::kUnsignedSaturate),
      ReferencePack8In16(constant_zero, constant_ones,
                         Pack8Mode::kUnsignedSaturate),
      ReferencePack8In16(dynamic_src2, dynamic_src2,
                         Pack8Mode::kUnsignedSaturate),
  };

  size_t comparison_count = 0;
  test.Run(
      [dynamic_src1, dynamic_src2](PPCContext* ctx) {
        ctx->v[20] = dynamic_src1;
        ctx->v[21] = dynamic_src2;
      },
      [&](PPCContext* ctx) {
        for (size_t i = 0; i < expected.size(); ++i) {
          REQUIRE(ctx->v[3 + i] == expected[i]);
          ++comparison_count;
        }
        REQUIRE(ctx->v[22] == dynamic_src1);
        REQUIRE(ctx->v[23] == dynamic_src2);
        comparison_count += 2;
      });
  std::printf("PACK_8_IN_16 unsigned constants: %zu comparisons\n",
              comparison_count);
  REQUIRE(comparison_count == 10);
}

TEST_CASE("PACK_8_IN_16_UNSIGNED_ARCHITECTURAL_ALIAS", "[instr]") {
  struct AliasCase {
    int dest;
    int src1;
    int src2;
  };
  constexpr std::array<AliasCase, 4> kAliasCases = {{
      {4, 4, 5},  // VD == VA
      {5, 4, 5},  // VD == VB
      {3, 4, 4},  // VA == VB
      {4, 4, 4},  // VD == VA == VB
  }};
  constexpr std::array<Pack8Mode, 2> kModes = {
      Pack8Mode::kUnsignedModulo,
      Pack8Mode::kUnsignedSaturate,
  };
  const vec128_t src1 =
      vec128s(0x0000, 0x00FF, 0x0100, 0x0101, 0x1234, 0xABCD, 0xFF00, 0xFFFF);
  const vec128_t src2 =
      vec128s(0xFFFF, 0x7FFF, 0x01FF, 0x0100, 0x00FE, 0x0080, 0x0001, 0x0000);

  size_t run_count = 0;
  for (Pack8Mode mode : kModes) {
    for (const AliasCase& alias : kAliasCases) {
      const uint32_t flags = Pack8Flags(mode);
      TestFunction test([alias, flags](HIRBuilder& b) {
        StoreVR(b, alias.dest,
                b.Pack(LoadVR(b, alias.src1), LoadVR(b, alias.src2), flags));
        b.Return();
      });
      const vec128_t& expected_src2 = alias.src2 == alias.src1 ? src1 : src2;
      const vec128_t expected = ReferencePack8In16(src1, expected_src2, mode);
      test.Run(
          [src1, src2](PPCContext* ctx) {
            ctx->v[4] = src1;
            ctx->v[5] = src2;
          },
          [alias, expected](PPCContext* ctx) {
            REQUIRE(ctx->v[alias.dest] == expected);
          });
      ++run_count;
    }
  }
  std::printf("PACK_8_IN_16 unsigned architectural aliases: %zu runs\n",
              run_count);
  REQUIRE(run_count == kModes.size() * kAliasCases.size());
  REQUIRE(run_count == 8);
}

TEST_CASE("PACK_8_IN_16_UNSIGNED_SOURCE_LIVE_PRESSURE", "[instr]") {
  constexpr size_t kSurvivorCount = 12;
  const uint32_t modulo_flags = Pack8Flags(Pack8Mode::kUnsignedModulo);
  const uint32_t saturate_flags = Pack8Flags(Pack8Mode::kUnsignedSaturate);
  TestFunction test([=](HIRBuilder& b) {
    auto* src1 = LoadVR(b, 4);
    auto* src2 = LoadVR(b, 5);
    std::array<Value*, kSurvivorCount> survivors;
    for (size_t i = 0; i < survivors.size(); ++i) {
      survivors[i] = LoadVR(b, 6 + static_cast<int>(i));
    }
    auto* modulo = b.Pack(src1, src2, modulo_flags);
    auto* saturated = b.Pack(src1, src2, saturate_flags);
    StoreVR(b, 3, modulo);
    StoreVR(b, 18, saturated);
    StoreVR(b, 19, src1);
    StoreVR(b, 20, src2);
    for (size_t i = 0; i < survivors.size(); ++i) {
      StoreVR(b, 21 + static_cast<int>(i), survivors[i]);
    }
    b.Return();
  });

  const vec128_t src1 =
      vec128s(0x0000, 0x00FF, 0x0100, 0x0101, 0x1234, 0xABCD, 0xFF00, 0xFFFF);
  const vec128_t src2 =
      vec128s(0xFFFF, 0x7FFF, 0x01FF, 0x0100, 0x00FE, 0x0080, 0x0001, 0x0000);
  std::array<vec128_t, kSurvivorCount> survivors;
  for (size_t i = 0; i < survivors.size(); ++i) {
    survivors[i] = vec128i(0x01020304u + static_cast<uint32_t>(i),
                           0x11223344u + static_cast<uint32_t>(i),
                           0x89ABCDEFu - static_cast<uint32_t>(i),
                           0xF0E1D2C3u - static_cast<uint32_t>(i));
  }
  const vec128_t expected_modulo =
      ReferencePack8In16(src1, src2, Pack8Mode::kUnsignedModulo);
  const vec128_t expected_saturated =
      ReferencePack8In16(src1, src2, Pack8Mode::kUnsignedSaturate);

  size_t comparison_count = 0;
  test.Run(
      [src1, src2, survivors](PPCContext* ctx) {
        ctx->v[4] = src1;
        ctx->v[5] = src2;
        for (size_t i = 0; i < survivors.size(); ++i) {
          ctx->v[6 + i] = survivors[i];
        }
      },
      [&](PPCContext* ctx) {
        REQUIRE(ctx->v[3] == expected_modulo);
        REQUIRE(ctx->v[18] == expected_saturated);
        REQUIRE(ctx->v[19] == src1);
        REQUIRE(ctx->v[20] == src2);
        comparison_count += 4;
        for (size_t i = 0; i < survivors.size(); ++i) {
          REQUIRE(ctx->v[21 + i] == survivors[i]);
          ++comparison_count;
        }
      });
  std::printf(
      "PACK_8_IN_16 pressure: 14 live inputs, %zu post-pack comparisons\n",
      comparison_count);
  REQUIRE(comparison_count == 4 + kSurvivorCount);
  REQUIRE(comparison_count == 16);
}
