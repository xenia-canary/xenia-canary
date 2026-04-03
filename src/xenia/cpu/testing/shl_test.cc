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
using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

TEST_CASE("SHL_I8", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.Shl(b.Truncate(LoadGPR(b, 4), INT8_TYPE),
                                b.Truncate(LoadGPR(b, 5), INT8_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x0F;
        ctx->r[5] = 4;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint8_t>(ctx->r[3]);
        REQUIRE(result == 0xF0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF;
        ctx->r[5] = 0;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint8_t>(ctx->r[3]);
        REQUIRE(result == 0xFF);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF;
        ctx->r[5] = 1;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint8_t>(ctx->r[3]);
        REQUIRE(result == 0xFE);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x80;
        ctx->r[5] = 8;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint8_t>(ctx->r[3]);
        REQUIRE(result == 0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7F;
        ctx->r[5] = 7;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint8_t>(ctx->r[3]);
        REQUIRE(result == 0x80);
      });
}

TEST_CASE("SHL_I16", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.Shl(b.Truncate(LoadGPR(b, 4), INT16_TYPE),
                                b.Truncate(LoadGPR(b, 5), INT8_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x00FF;
        ctx->r[5] = 8;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint16_t>(ctx->r[3]);
        REQUIRE(result == 0xFF00);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFF;
        ctx->r[5] = 0;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint16_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFF);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7FFF;
        ctx->r[5] = 1;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint16_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFE);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x8000;
        ctx->r[5] = 16;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint16_t>(ctx->r[3]);
        REQUIRE(result == 0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7FFF;
        ctx->r[5] = 15;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint16_t>(ctx->r[3]);
        REQUIRE(result == 0x8000);
      });
}

TEST_CASE("SHL_I32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.Shl(b.Truncate(LoadGPR(b, 4), INT32_TYPE),
                                b.Truncate(LoadGPR(b, 5), INT8_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x0000FFFF;
        ctx->r[5] = 16;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint32_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFF0000);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFFFFFF;
        ctx->r[5] = 0;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint32_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFFFFFF);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7FFFFFFF;
        ctx->r[5] = 1;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint32_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFFFFFE);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x80000000;
        ctx->r[5] = 32;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint32_t>(ctx->r[3]);
        REQUIRE(result == 0x80000000);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7FFFFFFF;
        ctx->r[5] = 31;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint32_t>(ctx->r[3]);
        REQUIRE(result == 0x80000000);
      });
}

TEST_CASE("SHL_I64", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.Shl(b.Truncate(LoadGPR(b, 4), INT64_TYPE),
                   b.Truncate(LoadGPR(b, 5), INT8_TYPE)));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x00000000FFFFFFFFull;
        ctx->r[5] = 32;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint64_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFFFFFF00000000ull);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFFFFFFFFFFFFFFFFull;
        ctx->r[5] = 0;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint64_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFFFFFFFFFFFFFFull);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7FFFFFFFFFFFFFFFull;
        ctx->r[5] = 1;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint64_t>(ctx->r[3]);
        REQUIRE(result == 0xFFFFFFFFFFFFFFFEull);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x8000000000000000ull;
        ctx->r[5] = 64;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint64_t>(ctx->r[3]);
        REQUIRE(result == 0x8000000000000000ull);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x7FFFFFFFFFFFFFFFull;
        ctx->r[5] = 63;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<uint64_t>(ctx->r[3]);
        REQUIRE(result == 0x8000000000000000ull);
      });
}

TEST_CASE("SHL_V128", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Shl(LoadVR(b, 4), b.Truncate(LoadGPR(b, 1), INT8_TYPE)));
    b.Return();
  });
  // Shift by 0 — identity.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[1] = 0;
        ctx->v[4] = vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3] ==
                vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF));
      });
  // Shift by 1 — MSB of each word carries into LSB of the previous word.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[1] = 1;
        ctx->v[4] = vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3] ==
                vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE));
      });
  // Shift by 4 — verify carry propagation across word boundaries.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[1] = 4;
        ctx->v[4] = vec128i(0x01234567, 0x89ABCDEF, 0x01234567, 0x89ABCDEF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3] ==
                vec128i(0x12345678, 0x9ABCDEF0, 0x12345678, 0x9ABCDEF0));
      });
  // Shift by 8 — masked to 0, identity.
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[1] = 8;
        ctx->v[4] = vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3] ==
                vec128i(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF));
      });
}
