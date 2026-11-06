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
                vec128i(0x00000000, 0x00000000, 0x64D26D8B, 0x48814491));
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
