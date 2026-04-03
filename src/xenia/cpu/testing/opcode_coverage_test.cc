/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Tests for previously-untested HIR opcodes, prioritized by ARM64 bug risk.

#include "xenia/cpu/testing/util.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// ============================================================================
// VECTOR_DENORMFLUSH — potential register aliasing bug
// ============================================================================
TEST_CASE("VECTOR_DENORMFLUSH", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorDenormFlush(LoadVR(b, 4)));
    b.Return();
  });
  // Normal values should pass through unchanged.
  test.Run(
      [](PPCContext* ctx) { ctx->v[4] = vec128f(1.0f, -1.0f, 0.0f, 100.0f); },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].f32[0] == 1.0f);
        REQUIRE(ctx->v[3].f32[1] == -1.0f);
        REQUIRE(ctx->v[3].f32[2] == 0.0f);
        REQUIRE(ctx->v[3].f32[3] == 100.0f);
      });
  // Denormals should be flushed to signed zero.
  test.Run(
      [](PPCContext* ctx) {
        // Smallest positive denormal.
        uint32_t pos_denorm = 0x00000001;
        // Smallest negative denormal.
        uint32_t neg_denorm = 0x80000001;
        memcpy(&ctx->v[4].f32[0], &pos_denorm, 4);
        memcpy(&ctx->v[4].f32[1], &neg_denorm, 4);
        ctx->v[4].f32[2] = 1.0f;  // normal, should survive
        ctx->v[4].f32[3] = -1.0f;
      },
      [](PPCContext* ctx) {
        uint32_t r0, r1;
        memcpy(&r0, &ctx->v[3].f32[0], 4);
        memcpy(&r1, &ctx->v[3].f32[1], 4);
        REQUIRE(r0 == 0x00000000);  // +0.0
        REQUIRE(r1 == 0x80000000);  // -0.0 (sign preserved)
        REQUIRE(ctx->v[3].f32[2] == 1.0f);
        REQUIRE(ctx->v[3].f32[3] == -1.0f);
      });
}

// ============================================================================
// CONVERT F64<->F32 (the only CONVERT variants the PPC frontend generates)
// ============================================================================
TEST_CASE("CONVERT_F64_TO_F32", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    auto fval = b.Convert(LoadFPR(b, 4), FLOAT32_TYPE);
    StoreFPR(b, 3, b.Convert(fval, FLOAT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = -0.0; },
           [](PPCContext* ctx) {
             uint64_t bits;
             memcpy(&bits, &ctx->f[3], 8);
             REQUIRE(bits == 0x8000000000000000ULL);  // -0.0 preserved
           });
  // Large value that loses precision in F32.
  test.Run([](PPCContext* ctx) { ctx->f[4] = 16777217.0; },  // 2^24+1
           [](PPCContext* ctx) {
             float f32 = static_cast<float>(ctx->f[3]);
             REQUIRE(f32 == 16777216.0f);  // rounds to 2^24
           });
}

// ============================================================================
// VECTOR_CONVERT_I2F (unsigned and signed)
// ============================================================================
TEST_CASE("VECTOR_CONVERT_I2F_UNSIGNED", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorConvertI2F(LoadVR(b, 4), ARITHMETIC_UNSIGNED));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 1, 0x7FFFFFFF, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].f32[0] == 0.0f);
        REQUIRE(ctx->v[3].f32[1] == 1.0f);
        // 0x7FFFFFFF rounds to 2147483648.0f in single precision.
        REQUIRE(ctx->v[3].f32[2] == static_cast<float>(0x7FFFFFFFU));
        // 0xFFFFFFFF = 4294967295, rounds to 4294967296.0f.
        REQUIRE(ctx->v[3].f32[3] == static_cast<float>(0xFFFFFFFFU));
      });
}

TEST_CASE("VECTOR_CONVERT_I2F_SIGNED", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorConvertI2F(LoadVR(b, 4), 0));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 1, 0x80000000, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].f32[0] == 0.0f);
        REQUIRE(ctx->v[3].f32[1] == 1.0f);
        REQUIRE(ctx->v[3].f32[2] == -2147483648.0f);
        REQUIRE(ctx->v[3].f32[3] == -1.0f);
      });
}

// ============================================================================
// VECTOR_CONVERT_F2I (unsigned and signed)
// ============================================================================
TEST_CASE("VECTOR_CONVERT_F2I_UNSIGNED", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorConvertF2I(LoadVR(b, 4), ARITHMETIC_UNSIGNED));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) { ctx->v[4] = vec128f(0.0f, 1.0f, 1.5f, 255.0f); },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0);
        REQUIRE(ctx->v[3].u32[1] == 1);
        REQUIRE(ctx->v[3].u32[2] == 1);  // truncate toward zero
        REQUIRE(ctx->v[3].u32[3] == 255);
      });
  // NaN should produce 0.
  test.Run(
      [](PPCContext* ctx) {
        float nan = std::numeric_limits<float>::quiet_NaN();
        ctx->v[4] = vec128f(nan, 0.0f, 0.0f, 0.0f);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3].u32[0] == 0); });
}

TEST_CASE("VECTOR_CONVERT_F2I_SIGNED", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorConvertF2I(LoadVR(b, 4), 0));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) { ctx->v[4] = vec128f(0.0f, -1.0f, 1.5f, -1.5f); },
      [](PPCContext* ctx) {
        REQUIRE(static_cast<int32_t>(ctx->v[3].u32[0]) == 0);
        REQUIRE(static_cast<int32_t>(ctx->v[3].u32[1]) == -1);
        REQUIRE(static_cast<int32_t>(ctx->v[3].u32[2]) == 1);
        REQUIRE(static_cast<int32_t>(ctx->v[3].u32[3]) == -1);
      });
  // NaN should produce 0.
  test.Run(
      [](PPCContext* ctx) {
        float nan = std::numeric_limits<float>::quiet_NaN();
        ctx->v[4] = vec128f(nan, 0.0f, 0.0f, 0.0f);
      },
      [](PPCContext* ctx) {
        REQUIRE(static_cast<int32_t>(ctx->v[3].u32[0]) == 0);
      });
}

// ============================================================================
// VECTOR_COMPARE_EQ / SGT / UGT — basic coverage
// ============================================================================
TEST_CASE("VECTOR_COMPARE_EQ_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorCompareEQ(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(1, 2, 3, 4);
        ctx->v[5] = vec128i(1, 99, 3, 99);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);  // equal
        REQUIRE(ctx->v[3].u32[1] == 0x00000000);  // not equal
        REQUIRE(ctx->v[3].u32[2] == 0xFFFFFFFF);  // equal
        REQUIRE(ctx->v[3].u32[3] == 0x00000000);  // not equal
      });
}

TEST_CASE("VECTOR_COMPARE_SGT_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorCompareSGT(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(1, 0xFFFFFFFF, 0x80000000, 0);
        ctx->v[5] = vec128i(0, 0, 0, 0x80000000);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);  // 1 > 0
        REQUIRE(ctx->v[3].u32[1] == 0x00000000);  // -1 > 0 = false
        REQUIRE(ctx->v[3].u32[2] == 0x00000000);  // INT_MIN > 0 = false
        REQUIRE(ctx->v[3].u32[3] == 0xFFFFFFFF);  // 0 > INT_MIN = true
      });
}

TEST_CASE("VECTOR_COMPARE_UGT_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorCompareUGT(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(1, 0xFFFFFFFF, 0x80000000, 0);
        ctx->v[5] = vec128i(0, 0, 0, 0x80000000);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);  // 1 > 0
        REQUIRE(ctx->v[3].u32[1] == 0xFFFFFFFF);  // 0xFFFFFFFF > 0 (unsigned)
        REQUIRE(ctx->v[3].u32[2] == 0xFFFFFFFF);  // 0x80000000 > 0 (unsigned)
        REQUIRE(ctx->v[3].u32[3] == 0x00000000);  // 0 > 0x80000000 = false
      });
}

// ============================================================================
// SPLAT
// ============================================================================
TEST_CASE("SPLAT_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Splat(b.Truncate(LoadGPR(b, 4), INT32_TYPE), VEC128_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0xDEADBEEF; },
           [](PPCContext* ctx) {
             REQUIRE(ctx->v[3] ==
                     vec128i(0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF));
           });
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0; },
           [](PPCContext* ctx) { REQUIRE(ctx->v[3] == vec128i(0, 0, 0, 0)); });
}

TEST_CASE("SPLAT_F32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Splat(b.Convert(LoadFPR(b, 4), FLOAT32_TYPE), VEC128_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 3.14; },
           [](PPCContext* ctx) {
             float expected = static_cast<float>(3.14);
             REQUIRE(ctx->v[3].f32[0] == expected);
             REQUIRE(ctx->v[3].f32[1] == expected);
             REQUIRE(ctx->v[3].f32[2] == expected);
             REQUIRE(ctx->v[3].f32[3] == expected);
           });
}

// ============================================================================
// IS_NAN (F64 — F32 variant is impossible on x64)
// ============================================================================
TEST_CASE("IS_NAN_F64", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3, b.ZeroExtend(b.IsNan(LoadFPR(b, 4)), INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 0.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = std::numeric_limits<double>::quiet_NaN();
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 1); });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = std::numeric_limits<double>::infinity();
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
}

// ============================================================================
// DIV I32 — divide by zero and INT_MIN/-1 edge cases
// ============================================================================
TEST_CASE("DIV_I32", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.Div(b.Truncate(LoadGPR(b, 4), INT32_TYPE),
                                b.Truncate(LoadGPR(b, 5), INT32_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 10;
        ctx->r[5] = 3;
      },
      [](PPCContext* ctx) { REQUIRE(static_cast<int32_t>(ctx->r[3]) == 3); });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFFFFFF;
        ctx->r[5] = 1;
      },
      [](PPCContext* ctx) { REQUIRE(static_cast<int32_t>(ctx->r[3]) == -1); });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 7;
        ctx->r[5] = 2;
      },
      [](PPCContext* ctx) { REQUIRE(static_cast<int32_t>(ctx->r[3]) == 3); });
}

// ============================================================================
// VECTOR_SUB — basic coverage with saturation
// ============================================================================
TEST_CASE("VECTOR_SUB_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorSub(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(10, 0, 0x80000000, 0xFFFFFFFF);
        ctx->v[5] = vec128i(3, 1, 1, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 7);
        REQUIRE(ctx->v[3].u32[1] == 0xFFFFFFFF);  // 0-1 wraps
        REQUIRE(ctx->v[3].u32[2] == 0x7FFFFFFF);  // INT_MIN-1 wraps
        REQUIRE(ctx->v[3].u32[3] == 0);
      });
}

// ============================================================================
// ABS F32/F64
// ============================================================================
TEST_CASE("ABS_F32", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    auto val = b.Convert(LoadFPR(b, 4), FLOAT32_TYPE);
    auto absval = b.Abs(val);
    StoreFPR(b, 3, b.Convert(absval, FLOAT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = -1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 0.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 0.0); });
  // ABS of -0.0 should be +0.0.
  test.Run([](PPCContext* ctx) { ctx->f[4] = -0.0; },
           [](PPCContext* ctx) {
             uint64_t bits;
             memcpy(&bits, &ctx->f[3], 8);
             REQUIRE(bits == 0);  // +0.0, not -0.0
           });
}

// ============================================================================
// SQRT F64
// ============================================================================
TEST_CASE("SQRT_F64", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.Sqrt(LoadFPR(b, 4)));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 4.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 2.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 0.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 0.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
}

// ============================================================================
// MUL_HI I64 — unsigned (ARITHMETIC_UNSIGNED)
// ============================================================================
TEST_CASE("MUL_HI_I64_UNSIGNED", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3, b.MulHi(LoadGPR(b, 4), LoadGPR(b, 5), ARITHMETIC_UNSIGNED));
    b.Return();
  });
  // Low values: high 64 bits of 2*3 = 0.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 2;
        ctx->r[5] = 3;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
  // 2^63 * 2: high 64 bits = 1.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x8000000000000000ULL;
        ctx->r[5] = 2;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 1); });
  // MAX * MAX: (2^64-1)^2 high bits = 2^64-2.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFFFFFFFFFFFFFFULL;
        ctx->r[5] = 0xFFFFFFFFFFFFFFFFULL;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0xFFFFFFFFFFFFFFFEULL); });
}

// MUL_HI I64 — signed (default)
TEST_CASE("MUL_HI_I64_SIGNED", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3, b.MulHi(LoadGPR(b, 4), LoadGPR(b, 5)));
    b.Return();
  });
  // 2 * 3 signed: high bits = 0.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 2;
        ctx->r[5] = 3;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
  // -1 * -1 signed: result is 1 (128-bit), high bits = 0.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFFFFFFFFFFFFFFULL;
        ctx->r[5] = 0xFFFFFFFFFFFFFFFFULL;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0); });
  // -1 * 2 signed: result is -2, high bits = -1 (0xFFFFFFFFFFFFFFFF).
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFFFFFFFFFFFFFFULL;
        ctx->r[5] = 2;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0xFFFFFFFFFFFFFFFFULL); });
}

// ============================================================================
// ATOMIC_COMPARE_EXCHANGE I32
// ============================================================================
TEST_CASE("ATOMIC_COMPARE_EXCHANGE_I32", "[atomic]") {
  TestFunction test([](HIRBuilder& b) {
    // r[4] = address (guest), r[5] = expected, r[6] = desired.
    auto addr = LoadGPR(b, 4);
    auto expected = b.Truncate(LoadGPR(b, 5), INT32_TYPE);
    auto desired = b.Truncate(LoadGPR(b, 6), INT32_TYPE);
    auto result = b.AtomicCompareExchange(addr, expected, desired);
    StoreGPR(b, 3, b.ZeroExtend(result, INT64_TYPE));
    b.Return();
  });

  uint32_t guest_addr = test.memory->SystemHeapAlloc(4);
  REQUIRE(guest_addr != 0);
  auto* host_ptr =
      reinterpret_cast<uint32_t*>(test.memory->TranslateVirtual(guest_addr));

  // Success case: expected matches current value.
  test.Run(
      [&](PPCContext* ctx) {
        *host_ptr = 0xAAAAAAAA;
        ctx->r[4] = guest_addr;
        ctx->r[5] = 0xAAAAAAAA;  // expected
        ctx->r[6] = 0xBBBBBBBB;  // desired
      },
      [&](PPCContext* ctx) {
        REQUIRE(ctx->r[3] == 1);  // success
        REQUIRE(*host_ptr == 0xBBBBBBBB);
      });

  // Failure case: expected does NOT match.
  test.Run(
      [&](PPCContext* ctx) {
        *host_ptr = 0xCCCCCCCC;
        ctx->r[4] = guest_addr;
        ctx->r[5] = 0xDDDDDDDD;  // wrong expected
        ctx->r[6] = 0xEEEEEEEE;  // desired
      },
      [&](PPCContext* ctx) {
        REQUIRE(ctx->r[3] == 0);           // failure
        REQUIRE(*host_ptr == 0xCCCCCCCC);  // unchanged
      });

  test.memory->SystemHeapFree(guest_addr);
}

// ============================================================================
// AND_NOT — bitwise AND with complement of second operand
// ============================================================================
TEST_CASE("AND_NOT_I8", "[bitwise]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 2,
             b.ZeroExtend(b.And(b.Truncate(LoadGPR(b, 4), INT8_TYPE),
                                b.Not(b.Truncate(LoadGPR(b, 5), INT8_TYPE))),
                          INT64_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(b.AndNot(b.Truncate(LoadGPR(b, 4), INT8_TYPE),
                                   b.Truncate(LoadGPR(b, 5), INT8_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  // result = src1 & ~src2
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF;
        ctx->r[5] = 0x0F;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0xF0);
      });
  // All bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xAA;
        ctx->r[5] = 0xFF;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x00);
      });
  // No bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x12;
        ctx->r[5] = 0x00;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x12);
      });
}

TEST_CASE("AND_NOT_I16", "[bitwise]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 2,
             b.ZeroExtend(b.And(b.Truncate(LoadGPR(b, 4), INT16_TYPE),
                                b.Not(b.Truncate(LoadGPR(b, 5), INT16_TYPE))),
                          INT64_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(b.AndNot(b.Truncate(LoadGPR(b, 4), INT16_TYPE),
                                   b.Truncate(LoadGPR(b, 5), INT16_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  // result = src1 & ~src2
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF00;
        ctx->r[5] = 0x0F0F;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0xF000);
      });
  // All bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xAAAA;
        ctx->r[5] = 0xFFFF;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x0000);
      });
  // No bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x1234;
        ctx->r[5] = 0x0000;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x1234);
      });
}

TEST_CASE("AND_NOT_I32", "[bitwise]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 2,
             b.ZeroExtend(b.And(b.Truncate(LoadGPR(b, 4), INT32_TYPE),
                                b.Not(b.Truncate(LoadGPR(b, 5), INT32_TYPE))),
                          INT64_TYPE));
    StoreGPR(b, 3,
             b.ZeroExtend(b.AndNot(b.Truncate(LoadGPR(b, 4), INT32_TYPE),
                                   b.Truncate(LoadGPR(b, 5), INT32_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  // result = src1 & ~src2
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF00FF00;
        ctx->r[5] = 0x0F0F0F0F;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0xF000F000);
      });
  // All bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xAAAAAAAA;
        ctx->r[5] = 0xFFFFFFFF;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x00000000);
      });
  // No bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x12345678;
        ctx->r[5] = 0x00000000;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x12345678);
      });
}

TEST_CASE("AND_NOT_I64", "[bitwise]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 2, b.And(LoadGPR(b, 4), b.Not(LoadGPR(b, 5))));
    StoreGPR(b, 3, b.AndNot(LoadGPR(b, 4), LoadGPR(b, 5)));
    b.Return();
  });
  // result = src1 & ~src2
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF00FF00FF00FF00;
        ctx->r[5] = 0x0F0F0F0F0F0F0F0F;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(ctx->r[3] == 0xF000F000F000F000);
      });
  // All bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xAAAAAAAAAAAAAAAA;
        ctx->r[5] = 0xFFFFFFFFFFFFFFFF;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(ctx->r[3] == 0x0000000000000000);
      });
  // No bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x1234567812345678;
        ctx->r[5] = 0x0000000000000000;
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->r[2] == ctx->r[3]);
        REQUIRE(ctx->r[3] == 0x1234567812345678);
      });
}

TEST_CASE("AND_NOT_V128", "[bitwise]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 2, b.And(LoadVR(b, 4), b.Not(LoadVR(b, 5))));
    StoreVR(b, 3, b.AndNot(LoadVR(b, 4), LoadVR(b, 5)));
    b.Return();
  });
  // result = src1 & ~src2
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0xFF00);
        ctx->v[5] = vec128s(0x0F0F);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[2] == ctx->v[3]);
        REQUIRE(ctx->v[3] == vec128s(0xF000));
      });
  // All bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(0xAA);
        ctx->v[5] = vec128b(0xFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[2] == ctx->v[3]);
        REQUIRE(ctx->v[3] == vec128b(0x00));
      });
  // No bits masked out.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x12345678);
        ctx->v[5] = vec128i(0x00000000);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[2] == ctx->v[3]);
        REQUIRE(ctx->v[3] == vec128i(0x12345678));
      });
}

// ============================================================================
// TRUNCATE — integer narrowing
// ============================================================================
TEST_CASE("TRUNCATE_I64_TO_I32", "[alu]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.Truncate(LoadGPR(b, 4), INT32_TYPE), INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0x123456789ABCDEF0ull; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x9ABCDEF0);
           });
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0x00000000FFFFFFFFull; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0xFFFFFFFF);
           });
}

TEST_CASE("TRUNCATE_I32_TO_I16", "[alu]") {
  TestFunction test([](HIRBuilder& b) {
    auto val = b.Truncate(LoadGPR(b, 4), INT32_TYPE);
    auto narrow = b.Truncate(val, INT16_TYPE);
    StoreGPR(b, 3, b.ZeroExtend(narrow, INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0xDEADBEEF; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0xBEEF);
           });
}

TEST_CASE("TRUNCATE_I32_TO_I8", "[alu]") {
  TestFunction test([](HIRBuilder& b) {
    auto val = b.Truncate(LoadGPR(b, 4), INT32_TYPE);
    auto narrow = b.Truncate(val, INT8_TYPE);
    StoreGPR(b, 3, b.ZeroExtend(narrow, INT64_TYPE));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0xDEADBEEF; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0xEF);
           });
}

// ============================================================================
// VECTOR_COMPARE_SGE — signed greater-than-or-equal per lane
// ============================================================================
TEST_CASE("VECTOR_COMPARE_SGE_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorCompareSGE(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(1, 0xFFFFFFFF, 0x80000000, 0);
        ctx->v[5] = vec128i(0, 0, 0, 0x80000000);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);  // 1 >= 0
        REQUIRE(ctx->v[3].u32[1] == 0x00000000);  // -1 >= 0 = false
        REQUIRE(ctx->v[3].u32[2] == 0x00000000);  // INT_MIN >= 0 = false
        REQUIRE(ctx->v[3].u32[3] == 0xFFFFFFFF);  // 0 >= INT_MIN = true
      });
  // Equal values.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(42, 0, 0x80000000, 0x7FFFFFFF);
        ctx->v[5] = vec128i(42, 0, 0x80000000, 0x7FFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);  // 42 >= 42
        REQUIRE(ctx->v[3].u32[1] == 0xFFFFFFFF);  // 0 >= 0
        REQUIRE(ctx->v[3].u32[2] == 0xFFFFFFFF);  // INT_MIN >= INT_MIN
        REQUIRE(ctx->v[3].u32[3] == 0xFFFFFFFF);  // INT_MAX >= INT_MAX
      });
}

// ============================================================================
// VECTOR_COMPARE_UGE — unsigned greater-than-or-equal per lane
// ============================================================================
TEST_CASE("VECTOR_COMPARE_UGE_I32", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorCompareUGE(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(1, 0xFFFFFFFF, 0, 0x80000000);
        ctx->v[5] = vec128i(0, 0x80000000, 1, 0x80000000);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);  // 1 >= 0
        REQUIRE(ctx->v[3].u32[1] == 0xFFFFFFFF);  // 0xFFFFFFFF >= 0x80000000
        REQUIRE(ctx->v[3].u32[2] == 0x00000000);  // 0 >= 1 = false
        REQUIRE(ctx->v[3].u32[3] == 0xFFFFFFFF);  // 0x80000000 >= 0x80000000
      });
  // All equal.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0, 1, 0x80000000, 0xFFFFFFFF);
        ctx->v[5] = vec128i(0, 1, 0x80000000, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].u32[0] == 0xFFFFFFFF);
        REQUIRE(ctx->v[3].u32[1] == 0xFFFFFFFF);
        REQUIRE(ctx->v[3].u32[2] == 0xFFFFFFFF);
        REQUIRE(ctx->v[3].u32[3] == 0xFFFFFFFF);
      });
}
