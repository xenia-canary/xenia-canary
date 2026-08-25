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

TEST_CASE("SWIZZLE_V128", "[instr]") {
  TestFunction([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Swizzle(LoadVR(b, 4), INT32_TYPE, MakeSwizzleMask(0, 1, 2, 3)));
    b.Return();
  })
      .Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 1, 2, 3); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(0, 1, 2, 3));
           });
  TestFunction([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Swizzle(LoadVR(b, 4), INT32_TYPE, MakeSwizzleMask(3, 2, 1, 0)));
    b.Return();
  })
      .Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 1, 2, 3); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(3, 2, 1, 0));
           });
  TestFunction([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.Swizzle(LoadVR(b, 4), INT32_TYPE, MakeSwizzleMask(1, 1, 2, 2)));
    b.Return();
  })
      .Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0, 1, 2, 3); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128i(1, 1, 2, 2));
           });
}

TEST_CASE("SWIZZLE_V128_FOLD_MATCHES_BACKEND", "[instr]") {
  const vec128_t vec = vec128i(0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F);
  const uint32_t masks[] = {
      SWIZZLE_XYZW_TO_XYZW,        SWIZZLE_XYZW_TO_YZWX,
      SWIZZLE_XYZW_TO_ZWXY,        SWIZZLE_XYZW_TO_WXYZ,
      MakeSwizzleMask(3, 2, 1, 0), MakeSwizzleMask(0, 0, 0, 0),
      MakeSwizzleMask(2, 2, 3, 3),
  };
  for (uint32_t mask : masks) {
    RequireVectorFoldMatchesBackend(
        {vec}, [mask](HIRBuilder& b, const std::vector<Value*>& ops) {
          return b.Swizzle(ops[0], INT32_TYPE, mask);
        });
  }
}
