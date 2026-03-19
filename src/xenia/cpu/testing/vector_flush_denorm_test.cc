/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
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

TEST_CASE("VECTOR_DENORMFLUSH_F32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.VectorDenormFlush(LoadVR(b, 4)));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0x7fffff); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128f(0.0f));
           });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128i(0x807fffff); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128f(-0.0f));
           });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128f(FLT_MIN); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128f(FLT_MIN));
           });
  test.Run([](PPCContext* ctx) { ctx->v[4] = vec128f(-FLT_MIN); },
           [](PPCContext* ctx) {
             auto result = ctx->v[3];
             REQUIRE(result == vec128f(-FLT_MIN));
           });
}
