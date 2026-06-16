/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cfloat>
#include <cmath>

using namespace xe::cpu::hir;
using namespace xe::cpu;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// Helper for floating-point comparison with epsilon
static bool ApproxEqual(double a, double b, double epsilon = 1e-6) {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  if (std::isinf(a) && std::isinf(b)) {
    return (a > 0) == (b > 0);
  }
  return std::abs(a - b) <= epsilon * std::max(std::abs(a), std::abs(b));
}

TEST_CASE("ADD_F32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3,
             b.Convert(b.Add(b.Convert(LoadFPR(b, 4), FLOAT32_TYPE),
                             b.Convert(LoadFPR(b, 5), FLOAT32_TYPE)),
                       FLOAT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 0.0;
        ctx->f[5] = 0.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 0.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 5.0;
        ctx->f[5] = 7.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 12.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = FLT_MAX / 2.0;
        ctx->f[5] = FLT_MAX / 2.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == FLT_MAX);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -100.0;
        ctx->f[5] = -150.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == -250.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = FLT_MIN;
        ctx->f[5] = 0.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == FLT_MIN);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = FLT_MAX;
        ctx->f[5] = 0.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == FLT_MAX);
      });
}

TEST_CASE("SUB_F32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3,
             b.Convert(b.Sub(b.Convert(LoadFPR(b, 4), FLOAT32_TYPE),
                             b.Convert(LoadFPR(b, 5), FLOAT32_TYPE)),
                       FLOAT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 0.0;
        ctx->f[5] = 0.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 0.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 10.0;
        ctx->f[5] = 3.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 7.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = FLT_MAX;
        ctx->f[5] = FLT_MAX / 2.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == FLT_MAX / 2.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -100.0;
        ctx->f[5] = -150.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 50.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 5.5;
        ctx->f[5] = 2.5;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 3.0);
      });
}

TEST_CASE("MUL_F32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3,
             b.Convert(b.Mul(b.Convert(LoadFPR(b, 4), FLOAT32_TYPE),
                             b.Convert(LoadFPR(b, 5), FLOAT32_TYPE)),
                       FLOAT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 0.0;
        ctx->f[5] = 0.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 0.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 2.0;
        ctx->f[5] = 3.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 6.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -5.0;
        ctx->f[5] = 3.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == -15.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -5.0;
        ctx->f[5] = -3.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 15.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 1.5;
        ctx->f[5] = 2.5;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 3.75);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 1000000.0f;
        ctx->f[5] = 1000000.0f;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(ApproxEqual(result, 1000000000000.0));
      });
}

TEST_CASE("DIV_F32", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3,
             b.Convert(b.Div(b.Convert(LoadFPR(b, 4), FLOAT32_TYPE),
                             b.Convert(LoadFPR(b, 5), FLOAT32_TYPE)),
                       FLOAT64_TYPE));
    b.Return();
  });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 0.0;
        ctx->f[5] = 1.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 0.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 6.0;
        ctx->f[5] = 2.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 3.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -15.0;
        ctx->f[5] = 3.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == -5.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -15.0;
        ctx->f[5] = -3.0;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 5.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 7.5;
        ctx->f[5] = 2.5;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 3.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 1000000.0f;
        ctx->f[5] = 1000.0f;
      },
      [](PPCContext* ctx) {
        auto result = ctx->f[3];
        REQUIRE(result == 1000.0);
      });
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 1.0;
        ctx->f[5] = 3.0;
      },
      [](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        REQUIRE(result == 1.0f / 3.0f);
      });
}
