/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cfloat>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

TEST_CASE("EXTRACT_INT8", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(
        b, 3,
        b.ZeroExtend(b.Extract(LoadVR(b, 4),
                               b.Truncate(LoadGPR(b, 4), INT8_TYPE), INT8_TYPE),
                     INT64_TYPE));
    b.Return();
  });
  for (int i = 0; i < 16; ++i) {
    test.Run(
        [i](PPCContext* ctx) {
          ctx->r[4] = i;
          ctx->v[4] =
              vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        },
        [i](PPCContext* ctx) {
          auto result = ctx->r[3];
          REQUIRE(result == i);
        });
  }
}

TEST_CASE("EXTRACT_INT8_CONSTANT", "[instr]") {
  for (int i = 0; i < 16; ++i) {
    TestFunction([i](HIRBuilder& b) {
      StoreGPR(b, 3,
               b.ZeroExtend(
                   b.Extract(LoadVR(b, 4), b.LoadConstantInt8(i), INT8_TYPE),
                   INT64_TYPE));
      b.Return();
    })
        .Run(
            [i](PPCContext* ctx) {
              ctx->r[4] = i;
              ctx->v[4] =
                  vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
            },
            [i](PPCContext* ctx) {
              auto result = ctx->r[3];
              REQUIRE(result == i);
            });
  }
}

TEST_CASE("EXTRACT_INT16", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(
                 b.Extract(LoadVR(b, 4), b.Truncate(LoadGPR(b, 4), INT8_TYPE),
                           INT16_TYPE),
                 INT64_TYPE));
    b.Return();
  });
  for (int i = 0; i < 8; ++i) {
    test.Run(
        [i](PPCContext* ctx) {
          ctx->r[4] = i;
          ctx->v[4] = vec128s(0x0000, 0x1001, 0x2002, 0x3003, 0x4004, 0x5005,
                              0x6006, 0x7007);
        },
        [i](PPCContext* ctx) {
          auto result = ctx->r[3];
          REQUIRE(result == (i | (i << 12)));
        });
  }
}

TEST_CASE("EXTRACT_INT16_CONSTANT", "[instr]") {
  for (int i = 0; i < 8; ++i) {
    TestFunction([i](HIRBuilder& b) {
      StoreGPR(b, 3,
               b.ZeroExtend(
                   b.Extract(LoadVR(b, 4), b.LoadConstantInt8(i), INT16_TYPE),
                   INT64_TYPE));
      b.Return();
    })
        .Run(
            [i](PPCContext* ctx) {
              ctx->r[4] = i;
              ctx->v[4] = vec128s(0, 1, 2, 3, 4, 5, 6, 7);
            },
            [i](PPCContext* ctx) {
              auto result = ctx->r[3];
              REQUIRE(result == i);
            });
  }
}

TEST_CASE("EXTRACT_INT32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(
                 b.Extract(LoadVR(b, 4), b.Truncate(LoadGPR(b, 4), INT8_TYPE),
                           INT32_TYPE),
                 INT64_TYPE));
    b.Return();
  });
  for (int i = 0; i < 4; ++i) {
    test.Run(
        [i](PPCContext* ctx) {
          ctx->r[4] = i;
          ctx->v[4] = vec128i(0, 1, 2, 3);
        },
        [i](PPCContext* ctx) {
          auto result = ctx->r[3];
          REQUIRE(result == i);
        });
  }
}

TEST_CASE("EXTRACT_INT32_CONSTANT", "[instr]") {
  for (int i = 0; i < 4; ++i) {
    TestFunction([i](HIRBuilder& b) {
      StoreGPR(b, 3,
               b.ZeroExtend(
                   b.Extract(LoadVR(b, 4), b.LoadConstantInt8(i), INT32_TYPE),
                   INT64_TYPE));
      b.Return();
    })
        .Run(
            [i](PPCContext* ctx) {
              ctx->r[4] = i;
              ctx->v[4] = vec128i(0, 1, 2, 3);
            },
            [i](PPCContext* ctx) {
              auto result = ctx->r[3];
              REQUIRE(result == i);
            });
  }
}

// The folded and emitted forms must pick the same lane.
TEST_CASE("EXTRACT_INT8_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t vec =
      vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  for (int i = 0; i < 16; ++i) {
    RequireScalarFoldMatchesBackend(
        {vec}, [i](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Extract(ops[0], b.LoadConstantUint8(uint8_t(i)), INT8_TYPE);
        });
  }
}

TEST_CASE("EXTRACT_INT16_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t vec =
      vec128s(0x0000, 0x1001, 0x2002, 0x3003, 0x4004, 0x5005, 0x6006, 0x7007);
  for (int i = 0; i < 8; ++i) {
    RequireScalarFoldMatchesBackend(
        {vec}, [i](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Extract(ops[0], b.LoadConstantUint8(uint8_t(i)), INT16_TYPE);
        });
  }
}

TEST_CASE("EXTRACT_INT32_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t vec = vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  for (int i = 0; i < 4; ++i) {
    RequireScalarFoldMatchesBackend(
        {vec}, [i](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Extract(ops[0], b.LoadConstantUint8(uint8_t(i)), INT32_TYPE);
        });
  }
}

// stvebx and stvehx extract with a runtime index. The fold cannot fire so the
// vector reaches the emitter as a constant.
TEST_CASE("EXTRACT_INT8_CONSTANT_VECTOR_MATCHES_REGISTER", "[instr]") {
  const vec128_t vec =
      vec128b(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
  for (int i = 0; i < 16; ++i) {
    RequireScalarFoldMatchesBackend(
        {vec},
        [](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Extract(ops[0], b.Truncate(LoadGPR(b, 5), INT8_TYPE),
                           INT8_TYPE);
        },
        {uint64_t(i)});
  }
}

TEST_CASE("EXTRACT_INT16_CONSTANT_VECTOR_MATCHES_REGISTER", "[instr]") {
  const vec128_t vec =
      vec128s(0x0000, 0x1001, 0x2002, 0x3003, 0x4004, 0x5005, 0x6006, 0x7007);
  for (int i = 0; i < 8; ++i) {
    RequireScalarFoldMatchesBackend(
        {vec},
        [](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Extract(ops[0], b.Truncate(LoadGPR(b, 5), INT8_TYPE),
                           INT16_TYPE);
        },
        {uint64_t(i)});
  }
}

TEST_CASE("EXTRACT_INT32_CONSTANT_VECTOR_MATCHES_REGISTER", "[instr]") {
  const vec128_t vec = vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  for (int i = 0; i < 4; ++i) {
    RequireScalarFoldMatchesBackend(
        {vec},
        [](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Extract(ops[0], b.Truncate(LoadGPR(b, 5), INT8_TYPE),
                           INT32_TYPE);
        },
        {uint64_t(i)});
  }
}
