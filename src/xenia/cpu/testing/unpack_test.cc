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

TEST_CASE("UNPACK_SHORT_4", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.Unpack(LoadVR(b, 4), PACK_TYPE_SHORT_4));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result ==
                     vec128i(0x40400000, 0x40400000, 0x40400000, 0x40400000));
           });
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] =
            vec128i(0xCDCDCDCD, 0xCDCDCDCD, 0x512E2A47, 0x568EE95B);
      },
      [](PPCContext* ctx) {
        auto result = ctx->v[3];
        // Regression: A64 once swapped these two 64-bit result halves.
        REQUIRE(result ==
                vec128i(0x4040512E, 0x40402A47, 0x4040568E, 0x403FE95B));
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
