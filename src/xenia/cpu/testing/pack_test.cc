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

constexpr size_t kFloat16PackTopPrefixCount = 1u << 19;
constexpr size_t kFloat16PackGridInvocationCount = kFloat16PackTopPrefixCount;
constexpr size_t kFloat16PackTailCount = 1u << 13;
constexpr std::array<uint32_t, 10> kFloat16PackRiskPrefixes = {
    0x387FE000u, 0xB87FE000u,  // Immediately below minimum normal.
    0x38800000u, 0xB8800000u,  // Minimum normal.
    0x3F800000u, 0xBF800000u,  // Representative normalized values.
    0x47FFC000u, 0xC7FFC000u,  // Last full prefix below saturation.
    0x47FFE000u, 0xC7FFE000u,  // Saturation threshold.
};
constexpr size_t kFloat16PackRiskInvocationCount =
    (kFloat16PackRiskPrefixes.size() / 2) * kFloat16PackTailCount;
constexpr size_t kFloat16PackSampleCount =
    kFloat16PackGridInvocationCount + kFloat16PackRiskInvocationCount;

void Float16PackSamplePair(size_t index, uint32_t& x_bits, uint32_t& y_bits) {
  if (index < kFloat16PackGridInvocationCount) {
    // Every sign/exponent/top-10-mantissa prefix appears once with a zero tail
    // in X and once with an all-ones tail in Y. Reversing the Y prefix also
    // makes lane swaps observable.
    x_bits = static_cast<uint32_t>(index << 13);
    y_bits = static_cast<uint32_t>(
        ((kFloat16PackTopPrefixCount - 1 - index) << 13) | 0x1FFFu);
    return;
  }

  const size_t risk_index = index - kFloat16PackGridInvocationCount;
  const size_t prefix_pair = risk_index / kFloat16PackTailCount;
  const uint32_t tail =
      static_cast<uint32_t>(risk_index % kFloat16PackTailCount);
  x_bits = kFloat16PackRiskPrefixes[prefix_pair * 2] | tail;
  y_bits = kFloat16PackRiskPrefixes[prefix_pair * 2 + 1] | tail;
}

// Independent bit-field reference for vpkd3d128 FLOAT16_2. Xenos half has an
// extended finite range, flushes values below its normalized range, saturates
// above it, and truncates rather than rounding to nearest.
uint16_t ReferenceFloatToXenosHalfTrunc(uint32_t float_bits) {
  const uint32_t sign = float_bits >> 31;
  const uint32_t exponent = (float_bits >> 23) & 0xFFu;
  const uint32_t fraction = float_bits & 0x7FFFFFu;

  uint32_t half_magnitude;
  if (exponent < 113u) {
    half_magnitude = 0;
  } else if (exponent > 143u || (exponent == 143u && fraction >= 0x7FE000u)) {
    half_magnitude = 0x7FFFu;
  } else {
    const uint32_t half_exponent = exponent - 112u;
    const uint32_t half_fraction = fraction >> 13;
    half_magnitude = (half_exponent << 10) | half_fraction;
  }
  return static_cast<uint16_t>((sign << 15) | half_magnitude);
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
  // Exact truncation discriminator: round-to-nearest-even would produce
  // 0x3C01/0xBC01 instead.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x3F801001, 0xBF801001, 0, 0);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3] == vec128i(0, 0, 0, 0x3C00BC00));
      });
}

TEST_CASE("PACK_FLOAT16_2_SAMPLED_EXACT_BITS", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Pack(LoadVR(b, 4), PACK_TYPE_FLOAT16_2));
    b.Return();
  });

  size_t invocation_count = 0;
  size_t lane_comparison_count = 0;
  size_t mismatch_count = 0;
  size_t padding_mismatch_count = 0;
  size_t first_mismatch_index = 0;
  uint32_t first_mismatch_lane = 0;
  uint16_t first_expected = 0;
  uint16_t first_actual = 0;

  test.RunRepeated(
      kFloat16PackSampleCount,
      [](PPCContext* ctx, size_t iteration) {
        uint32_t x_bits;
        uint32_t y_bits;
        Float16PackSamplePair(iteration, x_bits, y_bits);
        vec128_t source = vec128b(0);
        source.u32[0] = x_bits;
        source.u32[1] = y_bits;
        source.u32[2] = 0xDEADBEEFu;
        source.u32[3] = 0xA5A55A5Au;
        ctx->v[3] = vec128b(0xCD);
        ctx->v[4] = source;
      },
      [&](PPCContext* ctx, size_t iteration) {
        uint32_t x_bits;
        uint32_t y_bits;
        Float16PackSamplePair(iteration, x_bits, y_bits);
        const uint16_t expected_x = ReferenceFloatToXenosHalfTrunc(x_bits);
        const uint16_t expected_y = ReferenceFloatToXenosHalfTrunc(y_bits);
        const vec128_t actual = ctx->v[3];

        const auto record_mismatch = [&](uint32_t lane, uint16_t expected,
                                         uint16_t observed) {
          if (expected == observed) {
            return;
          }
          if (mismatch_count == 0) {
            first_mismatch_index = iteration;
            first_mismatch_lane = lane;
            first_expected = expected;
            first_actual = observed;
          }
          ++mismatch_count;
        };
        record_mismatch(0, expected_x, actual.u16[7]);
        record_mismatch(1, expected_y, actual.u16[6]);

        for (size_t i = 0; i < 6; ++i) {
          if (actual.u16[i] != 0) {
            ++padding_mismatch_count;
          }
        }
        ++invocation_count;
        lane_comparison_count += 2;
      });

  std::printf("PACK_FLOAT16_2 sampled: %zu invocations, %zu lane comparisons\n",
              invocation_count, lane_comparison_count);
  REQUIRE(invocation_count == kFloat16PackSampleCount);
  REQUIRE(invocation_count == 565248);
  REQUIRE(lane_comparison_count == kFloat16PackSampleCount * 2);
  REQUIRE(lane_comparison_count == 1130496);
  CAPTURE(first_mismatch_index, first_mismatch_lane, first_expected,
          first_actual);
  REQUIRE(mismatch_count == 0);
  REQUIRE(padding_mismatch_count == 0);
}

TEST_CASE("PACK_FLOAT16_2_CONSTANT_SOURCE", "[instr]") {
  struct ConstantCase {
    uint32_t x;
    uint32_t y;
  };
  constexpr std::array<ConstantCase, 10> kCases = {{
      {0x00000000u, 0x80000000u},
      {0x387FFFFFu, 0xB87FFFFFu},
      {0x38800000u, 0xB8800000u},
      {0x3F801001u, 0xBF801001u},
      {0x42AAAFFFu, 0xC4CCCFFFu},
      {0x47FFDFFFu, 0xC7FFDFFFu},
      {0x47FFE000u, 0xC7FFE000u},
      {0x7F800000u, 0xFF800000u},
      {0x7FC00001u, 0xFFC00001u},
      {0x00800000u, 0x80800000u},
  }};

  std::array<vec128_t, kCases.size()> sources;
  std::array<vec128_t, kCases.size()> expected;
  for (size_t i = 0; i < kCases.size(); ++i) {
    const ConstantCase& test_case = kCases[i];
    vec128_t source = vec128b(0);
    source.u32[0] = test_case.x;
    source.u32[1] = test_case.y;
    source.u32[2] = 0xDEADBEEFu;
    source.u32[3] = 0xA5A55A5Au;
    sources[i] = source;

    expected[i] = vec128b(0);
    expected[i].u16[7] = ReferenceFloatToXenosHalfTrunc(test_case.x);
    expected[i].u16[6] = ReferenceFloatToXenosHalfTrunc(test_case.y);
  }

  TestFunction test([sources](HIRBuilder& b) {
    for (size_t i = 0; i < sources.size(); ++i) {
      StoreVR(b, 3 + static_cast<int>(i),
              b.Pack(b.LoadConstantVec128(sources[i]), PACK_TYPE_FLOAT16_2));
    }
    b.Return();
  });
  size_t comparison_count = 0;
  test.Run([](PPCContext*) {},
           [&](PPCContext* ctx) {
             for (size_t i = 0; i < expected.size(); ++i) {
               REQUIRE(ctx->v[3 + i] == expected[i]);
               ++comparison_count;
             }
             REQUIRE(ctx->v[6] == vec128i(0, 0, 0, 0x3C00BC00));
           });

  std::printf("PACK_FLOAT16_2 constant source: %zu comparisons\n",
              comparison_count);
  REQUIRE(comparison_count == kCases.size());
}

TEST_CASE("PACK_FLOAT16_2_SOURCE_LIVE_PRESSURE", "[instr]") {
  constexpr size_t kSurvivorCount = 12;
  TestFunction test([](HIRBuilder& b) {
    auto source = LoadVR(b, 4);
    std::array<Value*, kSurvivorCount> survivors;
    for (size_t i = 0; i < survivors.size(); ++i) {
      survivors[i] = LoadVR(b, 5 + static_cast<int>(i));
    }
    auto packed = b.Pack(source, PACK_TYPE_FLOAT16_2);
    StoreVR(b, 3, packed);
    StoreVR(b, 20, source);
    for (size_t i = 0; i < survivors.size(); ++i) {
      StoreVR(b, 21 + static_cast<int>(i), survivors[i]);
    }
    b.Return();
  });

  vec128_t source = vec128b(0);
  source.u32[0] = 0x42AAAFFFu;
  source.u32[1] = 0xC4CCCFFFu;
  source.u32[2] = 0xDEADBEEFu;
  source.u32[3] = 0xA5A55A5Au;
  std::array<vec128_t, kSurvivorCount> survivors;
  for (size_t i = 0; i < survivors.size(); ++i) {
    survivors[i] = vec128i(0x01020304u + static_cast<uint32_t>(i),
                           0x11223344u + static_cast<uint32_t>(i),
                           0x89ABCDEFu - static_cast<uint32_t>(i),
                           0xF0E1D2C3u - static_cast<uint32_t>(i));
  }
  vec128_t expected = vec128b(0);
  expected.u16[7] = ReferenceFloatToXenosHalfTrunc(source.u32[0]);
  expected.u16[6] = ReferenceFloatToXenosHalfTrunc(source.u32[1]);

  test.Run(
      [source, survivors](PPCContext* ctx) {
        ctx->v[4] = source;
        for (size_t i = 0; i < survivors.size(); ++i) {
          ctx->v[5 + i] = survivors[i];
        }
      },
      [source, survivors, expected](PPCContext* ctx) {
        REQUIRE(ctx->v[3] == expected);
        REQUIRE(ctx->v[20] == source);
        for (size_t i = 0; i < survivors.size(); ++i) {
          REQUIRE(ctx->v[21 + i] == survivors[i]);
        }
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
