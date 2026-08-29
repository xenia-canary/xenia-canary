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

// Independent exact-bit reference for the default Xenos half conversion used
// by vupkd3d128. Half denormals flush to signed zero; exponent 31 remains part
// of the Xenos extended finite range rather than becoming IEEE infinity/NaN.
uint32_t ReferenceXenosHalfToFloatBits(uint16_t half_bits) {
  const uint32_t sign = uint32_t(half_bits & 0x8000u) << 16;
  const uint32_t exponent = (half_bits >> 10) & 0x1Fu;
  const uint32_t mantissa = half_bits & 0x3FFu;
  if (exponent == 0) {
    return sign;
  }
  return sign | ((exponent + 112u) << 23) | (mantissa << 13);
}

}  // namespace

TEST_CASE("UNPACK_D3DCOLOR", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_D3DCOLOR));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        uint32_t value = 0;
        ctx->v[4] = vec128i(0, 0, 0, value);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128f(1.0f, 1.0f, 1.0f, 1.0f));
      });
  test.Run(
      [](PPCContext* ctx) {
        uint32_t value = 0x80506070;
        ctx->v[4] = vec128i(0, 0, 0, value);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x3F800050, 0x3F800060, 0x3F800070, 0x3F800080));
      });
}

TEST_CASE("UNPACK_FLOAT16_2", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_FLOAT16_2));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(0, 0, 0, 0x3F800000));
           });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0x7FFFFFFF); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x47FFE000, 0xC7FFE000, 0x00000000, 0x3F800000));
           });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0x55556666); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x42AAA000, 0x44CCC000, 0x00000000, 0x3F800000));
           });
}

TEST_CASE("UNPACK_FLOAT16_2_EXHAUSTIVE_EXACT_BITS", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_FLOAT16_2));
    b.Return();
  });

  constexpr size_t kInvocationCount = 1u << 16;
  constexpr size_t kExpectedLaneConversions = kInvocationCount * 2;
  size_t invocation_count = 0;
  size_t lane_conversion_count = 0;
  size_t mismatch_count = 0;
  size_t fill_mismatch_count = 0;
  size_t first_mismatch_index = 0;
  uint32_t first_mismatch_lane = 0;
  uint32_t first_expected = 0;
  uint32_t first_actual = 0;

  test.RunRepeated(
      kInvocationCount,
      [](PPCContext* ctx, size_t iteration) {
        const uint16_t x = static_cast<uint16_t>(iteration);
        const uint16_t y = static_cast<uint16_t>(x ^ 0xFFFFu);
        vec128_t source = vec128b(0xA5);
        source.u16[7] = x;
        source.u16[6] = y;
        ctx->v[3] = vec128b(0xCD);
        ctx->v[4] = source;
      },
      [&](PPCContext* ctx, size_t iteration) {
        const uint16_t x = static_cast<uint16_t>(iteration);
        const uint16_t y = static_cast<uint16_t>(x ^ 0xFFFFu);
        const uint32_t expected_x = ReferenceXenosHalfToFloatBits(x);
        const uint32_t expected_y = ReferenceXenosHalfToFloatBits(y);
        const vec128_t actual = ctx->v[3];

        const auto record_mismatch = [&](uint32_t lane, uint32_t expected,
                                         uint32_t observed) {
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
        record_mismatch(0, expected_x, actual.u32[0]);
        record_mismatch(1, expected_y, actual.u32[1]);
        if (actual.u32[2] != 0 || actual.u32[3] != 0x3F800000u) {
          ++fill_mismatch_count;
        }
        ++invocation_count;
        lane_conversion_count += 2;
      });

  std::printf(
      "UNPACK_FLOAT16_2 exhaustive: %zu invocations, %zu lane conversions\n",
      invocation_count, lane_conversion_count);
  REQUIRE(invocation_count == kInvocationCount);
  REQUIRE(lane_conversion_count == kExpectedLaneConversions);
  CAPTURE(first_mismatch_index, first_mismatch_lane, first_expected,
          first_actual);
  REQUIRE(mismatch_count == 0);
  REQUIRE(fill_mismatch_count == 0);
}

TEST_CASE("UNPACK_FLOAT16_2_CONSTANT_SOURCE", "[instr]") {
  struct ConstantCase {
    uint16_t x;
    uint16_t y;
  };
  constexpr std::array<ConstantCase, 10> kCases = {{
      {0x0000u, 0x8000u},
      {0x0001u, 0x83FFu},
      {0x0400u, 0x8400u},
      {0x3C00u, 0xBC00u},
      {0x5555u, 0x6666u},
      {0x7BFFu, 0xFBFFu},
      {0x7C00u, 0xFC00u},
      {0x7FFFu, 0xFFFFu},
      {0x3555u, 0xB666u},
      {0x03FFu, 0x8001u},
  }};

  std::array<vec128_t, kCases.size()> sources;
  std::array<vec128_t, kCases.size()> expected;
  for (size_t i = 0; i < kCases.size(); ++i) {
    const ConstantCase& test_case = kCases[i];
    vec128_t source = vec128b(0xA5);
    source.u16[7] = test_case.x;
    source.u16[6] = test_case.y;
    sources[i] = source;

    expected[i] = vec128b(0);
    expected[i].u32[0] = ReferenceXenosHalfToFloatBits(test_case.x);
    expected[i].u32[1] = ReferenceXenosHalfToFloatBits(test_case.y);
    expected[i].u32[3] = 0x3F800000u;
  }

  TestFunction test([sources](HIRBuilder& b) {
    for (size_t i = 0; i < sources.size(); ++i) {
      StoreVR(b, 3 + static_cast<int>(i),
              b.Unpack(b.LoadConstantVec128(sources[i]), PACK_TYPE_FLOAT16_2));
    }
    b.Return();
  });
  size_t comparison_count = 0;
  test.Run(
      [](PPCContext*) {},
      [&](PPCContext* ctx) {
        for (size_t i = 0; i < expected.size(); ++i) {
          REQUIRE(ctx->v[3 + i] == expected[i]);
          ++comparison_count;
        }
        REQUIRE(ctx->v[7] == vec128i(0x42AAA000, 0x44CCC000, 0, 0x3F800000));
      });

  std::printf("UNPACK_FLOAT16_2 constant source: %zu comparisons\n",
              comparison_count);
  REQUIRE(comparison_count == kCases.size());
}

TEST_CASE("UNPACK_FLOAT16_2_SOURCE_LIVE_PRESSURE", "[instr]") {
  constexpr size_t kSurvivorCount = 12;
  TestFunction test([](HIRBuilder& b) {
    auto source = LoadVR(b, 4);
    std::array<Value*, kSurvivorCount> survivors;
    for (size_t i = 0; i < survivors.size(); ++i) {
      survivors[i] = LoadVR(b, 5 + static_cast<int>(i));
    }
    auto unpacked = b.Unpack(source, PACK_TYPE_FLOAT16_2);
    StoreVR(b, 3, unpacked);
    StoreVR(b, 20, source);
    for (size_t i = 0; i < survivors.size(); ++i) {
      StoreVR(b, 21 + static_cast<int>(i), survivors[i]);
    }
    b.Return();
  });

  vec128_t source = vec128b(0xA5);
  source.u16[7] = 0x5555u;
  source.u16[6] = 0xD666u;
  std::array<vec128_t, kSurvivorCount> survivors;
  for (size_t i = 0; i < survivors.size(); ++i) {
    survivors[i] = vec128i(0x01020304u + static_cast<uint32_t>(i),
                           0x11223344u + static_cast<uint32_t>(i),
                           0x89ABCDEFu - static_cast<uint32_t>(i),
                           0xF0E1D2C3u - static_cast<uint32_t>(i));
  }
  vec128_t expected = vec128b(0);
  expected.u32[0] = ReferenceXenosHalfToFloatBits(source.u16[7]);
  expected.u32[1] = ReferenceXenosHalfToFloatBits(source.u16[6]);
  expected.u32[3] = 0x3F800000u;

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

TEST_CASE("UNPACK_FLOAT16_4", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_FLOAT16_4));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(0));
           });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0, 0, 0, 0, 0x64D2, 0x6D8B, 0x4881, 0x4491);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x449A4000, 0x45B16000, 0x41102000, 0x40922000));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0, 0, 0, 0, 0x0001, 0x8001, 0x03FF, 0x83FF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3] ==
                vec128i(0x00000000, 0x80000000, 0x00000000, 0x80000000));
      });
}

TEST_CASE("UNPACK_SHORT_2", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_SHORT_2));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x40400000, 0x40400000, 0x00000000, 0x3F800000));
           });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x7004FD60, 0x8201C990, 0x00000000, 0x7FFF8001);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x40407FFF, 0x403F8001, 0x00000000, 0x3F800000));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 0, 0, (0x1234u << 16) | 0x5678u);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x40401234, 0x40405678, 0x00000000, 0x3F800000));
      });
}

TEST_CASE("UNPACK_UINT_2101010", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_UINT_2101010));
    b.Return();
  });
  // All-zero: XYZ=0 → magic 3.0, W=0 → 1.0
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x40400000, 0x40400000, 0x40400000, 0x3F800000));
           });
  // Positive values: x=100, y=200, z=3, w=2
  // packed = 100 | (200<<10) | (3<<20) | (2<<30) = 0x80332064
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0x80332064); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x40400064, 0x404000C8, 0x40400003, 0x3F800002));
           });
  // Negative x=-100 (10-bit: 0x39C), y=50, z=-1 (10-bit: 0x3FF), w=3
  // packed = 0x39C | (50<<10) | (0x3FF<<20) | (3<<30) = 0xFFF0CB9C
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0xFFF0CB9C); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             // x=-100: 0x40400000+0xFFFFFF9C = 0x403FFF9C
             // y=50:   0x40400000+50 = 0x40400032
             // z=-1:   0x40400000+0xFFFFFFFF = 0x403FFFFF
             // w=3:    0x3F800000+3 = 0x3F800003
             REQUIRE(result ==
                     vec128i(0x403FFF9C, 0x40400032, 0x403FFFFF, 0x3F800003));
           });
  // Overflow: x=-512 (10-bit: 0x200) → 0x403FFE00 → QNaN
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 0, 0, 0x00000200); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x7FC00000, 0x40400000, 0x40400000, 0x3F800000));
           });
}

TEST_CASE("UNPACK_ULONG_4202020", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_ULONG_4202020));
    b.Return();
  });
  // All-zero: XYZ=0 → magic 3.0, W=0 → 1.0
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x40400000, 0x40400000, 0x40400000, 0x3F800000));
           });
  // x=1000, y=2000, z=100, w=5
  // packed64 = 1000|(2000<<20)|(100<<40)|(5<<60) = 0x500064007D0003E8
  // u32[2]=0x50006400 (high), u32[3]=0x7D0003E8 (low)
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 0, 0x50006400, 0x7D0003E8);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // x=1000:  0x40400000+1000 = 0x404003E8
        // y=2000:  0x40400000+2000 = 0x404007D0
        // z=100:   0x40400000+100  = 0x40400064
        // w=5:     0x3F800000+5    = 0x3F800005
        REQUIRE(result ==
                vec128i(0x404003E8, 0x404007D0, 0x40400064, 0x3F800005));
      });
  // Negative x=-100, y=50, z=-1, w=10
  // packed64=0xAFFFFF00032FFF9C, u32[2]=0xAFFFFF00, u32[3]=0x032FFF9C
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 0, 0xAFFFFF00, 0x032FFF9C);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x403FFF9C, 0x40400032, 0x403FFFFF, 0x3F80000A));
      });
  // Overflow: x=-524288 (20-bit: 0x80000) → 0x40380000 → QNaN
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 0, 0x00000000, 0x00080000);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x7FC00000, 0x40400000, 0x40400000, 0x3F800000));
      });
}

// TEST_CASE("UNPACK_S8_IN_16_LO", "[instr]") {
//  TestFunction test([](HIRBuilder& b) {
//    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_S8_IN_16_LO));
//    b.Return();
//  });
//  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128b(0); },
//           [](PPCContext* ctx) {
//             auto result = ctx->v[3];
//             REQUIRE(result == vec128b(0));
//           });
//}
//
// TEST_CASE("UNPACK_S8_IN_16_HI", "[instr]") {
//  TestFunction test([](HIRBuilder& b) {
//    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_S8_IN_16_HI));
//    b.Return();
//  });
//  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128b(0); },
//           [](PPCContext* ctx) {
//             auto result = ctx->v[3];
//             REQUIRE(result == vec128b(0));
//           });
//}
//
// TEST_CASE("UNPACK_S16_IN_32_LO", "[instr]") {
//  TestFunction test([](HIRBuilder& b) {
//    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_S16_IN_32_LO));
//    b.Return();
//  });
//  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128b(0); },
//           [](PPCContext* ctx) {
//             auto result = ctx->v[3];
//             REQUIRE(result == vec128b(0));
//           });
//}
//
// TEST_CASE("UNPACK_S16_IN_32_HI", "[instr]") {
//  TestFunction test([](HIRBuilder& b) {
//    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_S16_IN_32_HI));
//    b.Return();
//  });
//  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128b(0); },
//           [](PPCContext* ctx) {
//             auto result = ctx->v[3];
//             REQUIRE(result == vec128b(0));
//           });
//}
