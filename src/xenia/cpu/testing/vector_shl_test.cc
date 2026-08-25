/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

TEST_CASE("VECTOR_SHL_I8", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorShl(LoadVR(b, 4), LoadVR(b, 5), INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(0x7E, 0x7E, 0x7E, 0x7F, 0x80, 0xFF, 0x01, 0x12,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
        ctx->v[5] =
            vec128b(0, 1, 2, 8, 4, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(0x7E, 0xFC, 0xF8, 0x7F, 0x00, 0xF0, 0x40,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00));
      });
}

TEST_CASE("VECTOR_SHL_I8_CONSTANT", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.VectorShl(LoadVR(b, 4),
                    b.LoadConstantVec128(vec128b(0, 1, 2, 8, 4, 4, 6, 7, 8, 9,
                                                 10, 11, 12, 13, 14, 15)),
                    INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(0x7E, 0x7E, 0x7E, 0x7F, 0x80, 0xFF, 0x01, 0x12,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(0x7E, 0xFC, 0xF8, 0x7F, 0x00, 0xF0, 0x40,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00));
      });
}

// This targets the "all_same" optimization of the Int8 specialization of
// VECTOR_SHL_V128
TEST_CASE("VECTOR_SHL_I8_SAME_CONSTANT", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.VectorShl(LoadVR(b, 4), b.LoadConstantVec128(vec128b(5)), INT8_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(0x7E, 0x7E, 0x7E, 0x7F, 0x80, 0xFF, 0x01, 0x12,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128b(0xC0, 0xC0, 0xC0, 0xE0, 0x00, 0xE0, 0x20,
                                  0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00));
      });
}

// Targets the "all_same" detection bug in EmitInt16's constant path.
// The loop condition `n < 8 - n` only checks u16[0..4], missing u16[5..7].
// vec128s params map to u16[] as: u16[0]=x1, u16[1]=x0, u16[2]=y1,
// u16[3]=y0, u16[4]=z1, u16[5]=z0, u16[6]=w1, u16[7]=w0.
// So params (1,1,1,1,5,1,5,5) → u16[0..4]=1, u16[5..7]=5.
// Buggy code sees all_same=true and shifts everything by 1.
TEST_CASE("VECTOR_SHL_I16_CONSTANT_PARTIAL_SAME", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.VectorShl(LoadVR(b, 4),
                        b.LoadConstantVec128(vec128s(1, 1, 1, 1, 5, 1, 5, 5)),
                        INT16_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                            0xFFFF, 0xFFFF);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // h0-h3,h5: shift 1 → 0xFFFE; h4,h6,h7: shift 5 → 0xFFE0
        REQUIRE(result == vec128s(0xFFFE, 0xFFFE, 0xFFFE, 0xFFFE, 0xFFE0,
                                  0xFFFE, 0xFFE0, 0xFFE0));
      });
}

TEST_CASE("VECTOR_SHL_I16", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorShl(LoadVR(b, 4), LoadVR(b, 5), INT16_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0x7FFE, 0x7FFE, 0x7FFE, 0x7FFF, 0x8000, 0xFFFF,
                            0x0001, 0x1234);
        ctx->v[5] = vec128s(0, 1, 8, 15, 15, 8, 1, 16);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128s(0x7FFE, 0xFFFC, 0xFE00, 0x8000, 0x0000,
                                  0xFF00, 0x0002, 0x1234));
      });
}

TEST_CASE("VECTOR_SHL_I16_CONSTANT", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.VectorShl(LoadVR(b, 4),
                    b.LoadConstantVec128(vec128s(0, 1, 8, 15, 15, 8, 1, 16)),
                    INT16_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128s(0x7FFE, 0x7FFE, 0x7FFE, 0x7FFF, 0x8000, 0xFFFF,
                            0x0001, 0x1234);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result == vec128s(0x7FFE, 0xFFFC, 0xFE00, 0x8000, 0x0000,
                                  0xFF00, 0x0002, 0x1234));
      });
}

// Targets the "all_same" detection bug in EmitInt32's constant path.
// The loop condition `n < 4 - n` only checks u32[0..2], missing u32[3].
// vec128i(1,1,1,10) → u32[0..2]=1, u32[3]=10.
// Buggy code sees all_same=true and shifts everything by 1.
TEST_CASE("VECTOR_SHL_I32_CONSTANT_PARTIAL_SAME", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.VectorShl(LoadVR(b, 4), b.LoadConstantVec128(vec128i(1, 1, 1, 10)),
                    INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // d0-d2: shift 1 → 0xFFFFFFFE; d3: shift 10 → 0xFFFFFC00
        REQUIRE(result ==
                vec128i(0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFFFE, 0xFFFFFC00));
      });
}

TEST_CASE("VECTOR_SHL_I32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorShl(LoadVR(b, 4), LoadVR(b, 5), INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFF);
        ctx->v[5] = vec128i(0, 1, 16, 31);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x7FFFFFFE, 0xFFFFFFFC, 0xFFFE0000, 0x80000000));
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x80000000, 0xFFFFFFFF, 0x00000001, 0x12345678);
        ctx->v[5] = vec128i(31, 16, 1, 32);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        REQUIRE(result ==
                vec128i(0x00000000, 0xFFFF0000, 0x00000002, 0x12345678));
      });
}

TEST_CASE("VECTOR_SHL_I32_CONSTANT", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(
        b, 3,
        b.VectorShl(LoadVR(b, 4), b.LoadConstantVec128(vec128i(0, 1, 16, 31)),
                    INT32_TYPE));
    StoreVR(
        b, 4,
        b.VectorShl(LoadVR(b, 5), b.LoadConstantVec128(vec128i(31, 16, 1, 32)),
                    INT32_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128i(0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFE, 0x7FFFFFFF);
        ctx->v[5] = vec128i(0x80000000, 0xFFFFFFFF, 0x00000001, 0x12345678);
      },
      [](PPCContext* ctx) {
        auto result1 = ctx->v[3];
        REQUIRE(result1 ==
                vec128i(0x7FFFFFFE, 0xFFFFFFFC, 0xFFFE0000, 0x80000000));
        auto result2 = ctx->v[4];
        REQUIRE(result2 ==
                vec128i(0x00000000, 0xFFFF0000, 0x00000002, 0x12345678));
      });
}

TEST_CASE("VECTOR_SHL_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t value =
      vec128b(0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA,
              0x98, 0x76, 0x54, 0x32, 0x10);
  // Counts past the element width exercise the masking.
  const vec128_t counts =
      vec128b(0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 127, 255);
  for (TypeName part : {INT8_TYPE, INT16_TYPE, INT32_TYPE}) {
    RequireVectorFoldMatchesBackend(
        {value, counts}, [part](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.VectorShl(ops[0], ops[1], part);
        });
  }
}
