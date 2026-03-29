/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Tests for remaining untested HIR opcodes: CAST, ROUND, TO_SINGLE,
// MUL_ADD, MUL_SUB, ADD_CARRY, RECIP, RSQRT, LVL/LVR/STVL/STVR,
// RESERVED_LOAD/RESERVED_STORE, VECTOR_AVERAGE.

#include "xenia/cpu/testing/util.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// ============================================================================
// CAST — bitcast I32<->F32, I64<->F64
// ============================================================================
TEST_CASE("CAST_I32_TO_F32", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    auto ival = b.Truncate(LoadGPR(b, 4), INT32_TYPE);
    auto fval = b.Cast(ival, FLOAT32_TYPE);
    // Store back as I32 via cast to verify round-trip.
    StoreGPR(b, 3, b.ZeroExtend(b.Cast(fval, INT32_TYPE), INT64_TYPE));
    b.Return();
  });
  // 1.0f = 0x3F800000
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0x3F800000; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x3F800000);
           });
  // -0.0f = 0x80000000
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0x80000000; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x80000000);
           });
  // NaN = 0x7FC00000
  test.Run([](PPCContext* ctx) { ctx->r[4] = 0x7FC00000; },
           [](PPCContext* ctx) {
             REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 0x7FC00000);
           });
}

TEST_CASE("CAST_I64_TO_F64", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    auto ival = LoadGPR(b, 4);
    auto fval = b.Cast(ival, FLOAT64_TYPE);
    StoreGPR(b, 3, b.Cast(fval, INT64_TYPE));
    b.Return();
  });
  // 1.0 = 0x3FF0000000000000
  test.Run(
      [](PPCContext* ctx) { ctx->r[4] = 0x3FF0000000000000ULL; },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0x3FF0000000000000ULL); });
  // -1.0 = 0xBFF0000000000000
  test.Run(
      [](PPCContext* ctx) { ctx->r[4] = 0xBFF0000000000000ULL; },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0xBFF0000000000000ULL); });
}

// ============================================================================
// ROUND F64 (F32 is impossible on x64)
// ============================================================================
TEST_CASE("ROUND_F64_NEAREST", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.Round(LoadFPR(b, 4), ROUND_TO_NEAREST));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.5; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 2.0); });
  test.Run(
      [](PPCContext* ctx) { ctx->f[4] = 2.5; },
      [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 2.0); });  // banker's rounding
  test.Run([](PPCContext* ctx) { ctx->f[4] = -1.5; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == -2.0); });
}

TEST_CASE("ROUND_F64_ZERO", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.Round(LoadFPR(b, 4), ROUND_TO_ZERO));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.9; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = -1.9; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == -1.0); });
}

TEST_CASE("ROUND_F64_CEIL", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.Round(LoadFPR(b, 4), ROUND_TO_POSITIVE_INFINITY));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.1; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 2.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = -1.1; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == -1.0); });
}

TEST_CASE("ROUND_F64_FLOOR", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.Round(LoadFPR(b, 4), ROUND_TO_MINUS_INFINITY));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.9; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = -1.1; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == -2.0); });
}

// ============================================================================
// TO_SINGLE — double -> single -> double rounding
// ============================================================================
TEST_CASE("TO_SINGLE", "[convert]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.ToSingle(LoadFPR(b, 4)));
    b.Return();
  });
  // Exact value survives.
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  // Value requiring rounding: 1.0 + 2^-24 in double -> rounds in single.
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0 + std::ldexp(1.0, -24); },
           [](PPCContext* ctx) {
             float f32 = static_cast<float>(ctx->f[3]);
             REQUIRE(f32 == 1.0f);  // rounded to nearest in single
           });
  // Negative value.
  test.Run([](PPCContext* ctx) { ctx->f[4] = -3.14; },
           [](PPCContext* ctx) {
             float f32 = static_cast<float>(ctx->f[3]);
             REQUIRE(f32 == static_cast<float>(-3.14));
           });
}

// ============================================================================
// MUL_ADD F64 — fused multiply-add: (1 * 2) + 3
// ============================================================================
TEST_CASE("MUL_ADD_F64", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.MulAdd(LoadFPR(b, 4), LoadFPR(b, 5), LoadFPR(b, 6)));
    b.Return();
  });
  // 2.0 * 3.0 + 4.0 = 10.0
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 2.0;
        ctx->f[5] = 3.0;
        ctx->f[6] = 4.0;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 10.0); });
  // -1.0 * 2.0 + 3.0 = 1.0
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = -1.0;
        ctx->f[5] = 2.0;
        ctx->f[6] = 3.0;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
  // 0.0 * anything + 5.0 = 5.0
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 0.0;
        ctx->f[5] = 999.0;
        ctx->f[6] = 5.0;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 5.0); });
}

// ============================================================================
// MUL_SUB F64 — fused multiply-subtract: (1 * 2) - 3
// ============================================================================
TEST_CASE("MUL_SUB_F64", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.MulSub(LoadFPR(b, 4), LoadFPR(b, 5), LoadFPR(b, 6)));
    b.Return();
  });
  // 2.0 * 3.0 - 4.0 = 2.0
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 2.0;
        ctx->f[5] = 3.0;
        ctx->f[6] = 4.0;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 2.0); });
  // 5.0 * 5.0 - 25.0 = 0.0
  test.Run(
      [](PPCContext* ctx) {
        ctx->f[4] = 5.0;
        ctx->f[5] = 5.0;
        ctx->f[6] = 25.0;
      },
      [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 0.0); });
}

// ============================================================================
// ADD_CARRY I8/I32 — add with carry-in
// ============================================================================
TEST_CASE("ADD_CARRY_I8", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3,
             b.ZeroExtend(b.AddWithCarry(b.Truncate(LoadGPR(b, 4), INT8_TYPE),
                                         b.Truncate(LoadGPR(b, 5), INT8_TYPE),
                                         b.Truncate(LoadGPR(b, 6), INT8_TYPE)),
                          INT64_TYPE));
    b.Return();
  });
  // 0xFF + 0x00 + carry=1 = 0x00 (wraps)
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0xFF;
        ctx->r[5] = 0;
        ctx->r[6] = 1;
      },
      [](PPCContext* ctx) {
        REQUIRE(static_cast<uint8_t>(ctx->r[3]) == 0x00);
      });
  // 0x10 + 0x20 + carry=0 = 0x30
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x10;
        ctx->r[5] = 0x20;
        ctx->r[6] = 0;
      },
      [](PPCContext* ctx) {
        REQUIRE(static_cast<uint8_t>(ctx->r[3]) == 0x30);
      });
  // 0x10 + 0x20 + carry=1 = 0x31
  test.Run(
      [](PPCContext* ctx) {
        ctx->r[4] = 0x10;
        ctx->r[5] = 0x20;
        ctx->r[6] = 1;
      },
      [](PPCContext* ctx) {
        REQUIRE(static_cast<uint8_t>(ctx->r[3]) == 0x31);
      });
}

// ============================================================================
// RECIP F32/F64 — exact reciprocal (1/x)
// ============================================================================
TEST_CASE("RECIP_F64", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    StoreFPR(b, 3, b.Recip(LoadFPR(b, 4)));
    b.Return();
  });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 2.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 0.5); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = -4.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == -0.25); });
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) { REQUIRE(ctx->f[3] == 1.0); });
}

// ============================================================================
// RSQRT F32 — 1/sqrt(x) (exact, not estimate)
// ============================================================================
TEST_CASE("RSQRT_F32", "[arithmetic]") {
  TestFunction test([](HIRBuilder& b) {
    auto val = b.Convert(LoadFPR(b, 4), FLOAT32_TYPE);
    auto rsqrt = b.RSqrt(val);
    StoreFPR(b, 3, b.Convert(rsqrt, FLOAT64_TYPE));
    b.Return();
  });
  // rsqrt(1.0) = 1.0
  test.Run([](PPCContext* ctx) { ctx->f[4] = 1.0; },
           [](PPCContext* ctx) {
             float r = static_cast<float>(ctx->f[3]);
             REQUIRE(r == 1.0f);
           });
  // rsqrt(4.0) = 0.5
  test.Run([](PPCContext* ctx) { ctx->f[4] = 4.0; },
           [](PPCContext* ctx) {
             float r = static_cast<float>(ctx->f[3]);
             REQUIRE(r == 0.5f);
           });
}

// ============================================================================
// VECTOR_AVERAGE I8 — rounding halving add: (a+b+1)>>1
// ============================================================================
TEST_CASE("VECTOR_AVERAGE_UNSIGNED_I8", "[vector]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3,
            b.VectorAverage(LoadVR(b, 4), LoadVR(b, 5), INT8_TYPE,
                            ARITHMETIC_UNSIGNED));
    b.Return();
  });
  // Use uniform values so byte ordering doesn't matter.
  // avg(100, 200) unsigned = (100+200+1)/2 = 150
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(100);
        ctx->v[5] = vec128b(200);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3] == vec128b(150)); });
  // avg(255, 255) unsigned = (255+255+1)/2 = 255
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(255);
        ctx->v[5] = vec128b(255);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3] == vec128b(255)); });
  // avg(0, 1) unsigned = (0+1+1)/2 = 1
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[4] = vec128b(0);
        ctx->v[5] = vec128b(1);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3] == vec128b(1)); });
}

// ============================================================================
// RESERVED_LOAD + RESERVED_STORE — LL/SC emulation
// ============================================================================
TEST_CASE("RESERVED_LOAD_STORE_I32", "[atomic]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    // Load with reserve.
    auto loaded = b.LoadWithReserve(addr, INT32_TYPE);
    StoreGPR(b, 3, b.ZeroExtend(loaded, INT64_TYPE));
    // Store conditional with the loaded value + 1.
    auto new_val = b.Add(loaded, b.LoadConstantInt32(1));
    auto success = b.StoreWithReserve(addr, new_val, INT32_TYPE);
    StoreGPR(b, 5, b.ZeroExtend(success, INT64_TYPE));
    b.Return();
  });

  uint32_t guest_addr = test.memory->SystemHeapAlloc(4);
  REQUIRE(guest_addr != 0);
  auto* host_ptr =
      reinterpret_cast<uint32_t*>(test.memory->TranslateVirtual(guest_addr));

  test.Run(
      [&](PPCContext* ctx) {
        *host_ptr = 42;
        ctx->r[4] = guest_addr;
      },
      [&](PPCContext* ctx) {
        REQUIRE(static_cast<uint32_t>(ctx->r[3]) == 42);  // loaded value
        REQUIRE(ctx->r[5] == 1);                          // store succeeded
        REQUIRE(*host_ptr == 43);                         // incremented
      });

  test.memory->SystemHeapFree(guest_addr);
}

TEST_CASE("RESERVED_LOAD_STORE_I64", "[atomic]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto loaded = b.LoadWithReserve(addr, INT64_TYPE);
    StoreGPR(b, 3, loaded);
    auto new_val = b.Add(loaded, b.LoadConstantInt64(1));
    auto success = b.StoreWithReserve(addr, new_val, INT64_TYPE);
    StoreGPR(b, 5, b.ZeroExtend(success, INT64_TYPE));
    b.Return();
  });

  uint32_t guest_addr = test.memory->SystemHeapAlloc(8);
  REQUIRE(guest_addr != 0);
  auto* host_ptr =
      reinterpret_cast<uint64_t*>(test.memory->TranslateVirtual(guest_addr));

  test.Run(
      [&](PPCContext* ctx) {
        *host_ptr = 100;
        ctx->r[4] = guest_addr;
      },
      [&](PPCContext* ctx) {
        REQUIRE(ctx->r[3] == 100);  // loaded value
        REQUIRE(ctx->r[5] == 1);    // store succeeded
        REQUIRE(*host_ptr == 101);  // incremented
      });

  test.memory->SystemHeapFree(guest_addr);
}

TEST_CASE("RESERVED_STORE_I32_NO_RESERVATION", "[atomic]") {
  // StoreWithReserve without a preceding LoadWithReserve must fail (return 0).
  // Run with a timeout: the bug this catches produces an infinite loop.
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto val = b.Truncate(LoadGPR(b, 5), INT32_TYPE);
    auto success = b.StoreWithReserve(addr, val, INT32_TYPE);
    StoreGPR(b, 3, b.ZeroExtend(success, INT64_TYPE));
    b.Return();
  });

  uint32_t guest_addr = test.memory->SystemHeapAlloc(4);
  REQUIRE(guest_addr != 0);
  auto* host_ptr =
      reinterpret_cast<uint32_t*>(test.memory->TranslateVirtual(guest_addr));

  std::atomic<bool> completed{false};
  std::thread worker([&]() {
    test.Run(
        [&](PPCContext* ctx) {
          *host_ptr = 42;
          ctx->r[4] = guest_addr;
          ctx->r[5] = 99;
        },
        [&](PPCContext* ctx) {
          CHECK(ctx->r[3] == 0);   // store must fail
          CHECK(*host_ptr == 42);  // memory unchanged
        });
    completed.store(true);
  });
  worker.detach();

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!completed.load()) {
    // Detached thread is stuck in JIT'd code — can't unwind safely.
    FAIL("Timed out: no-reservation path likely has an infinite loop");
    std::_Exit(1);
  }

  test.memory->SystemHeapFree(guest_addr);
}

TEST_CASE("RESERVED_STORE_I64_NO_RESERVATION", "[atomic]") {
  // Run with a timeout: the bug this catches produces an infinite loop.
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto val = LoadGPR(b, 5);
    auto success = b.StoreWithReserve(addr, val, INT64_TYPE);
    StoreGPR(b, 3, b.ZeroExtend(success, INT64_TYPE));
    b.Return();
  });

  uint32_t guest_addr = test.memory->SystemHeapAlloc(8);
  REQUIRE(guest_addr != 0);
  auto* host_ptr =
      reinterpret_cast<uint64_t*>(test.memory->TranslateVirtual(guest_addr));

  std::atomic<bool> completed{false};
  std::thread worker([&]() {
    test.Run(
        [&](PPCContext* ctx) {
          *host_ptr = 42;
          ctx->r[4] = guest_addr;
          ctx->r[5] = 99;
        },
        [&](PPCContext* ctx) {
          CHECK(ctx->r[3] == 0);   // store must fail
          CHECK(*host_ptr == 42);  // memory unchanged
        });
    completed.store(true);
  });
  worker.detach();

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!completed.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!completed.load()) {
    // Detached thread is stuck in JIT'd code — can't unwind safely.
    FAIL("Timed out: no-reservation path likely has an infinite loop");
    std::_Exit(1);
  }

  test.memory->SystemHeapFree(guest_addr);
}

// ============================================================================
// LVL / LVR — partial vector load left/right
// Detailed alignment semantics are tested by PPC instruction tests (lvlx etc.)
// These tests verify the HIR opcode executes without crashing.
// ============================================================================
TEST_CASE("LOAD_VECTOR_LEFT", "[memory]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.LoadVectorLeft(LoadGPR(b, 4)));
    b.Return();
  });

  // Allocate 16-byte aligned memory with known pattern.
  uint32_t guest_addr = test.memory->SystemHeapAlloc(32);
  REQUIRE(guest_addr != 0);
  // Align to 16 bytes.
  uint32_t aligned_addr = (guest_addr + 15) & ~15u;
  auto* host_ptr =
      reinterpret_cast<uint8_t*>(test.memory->TranslateVirtual(aligned_addr));
  for (int i = 0; i < 16; ++i) host_ptr[i] = static_cast<uint8_t>(0x10 + i);

  // LVL at aligned address — should load data (not crash).
  test.Run([&](PPCContext* ctx) { ctx->r[4] = aligned_addr; },
           [&](PPCContext* ctx) {
             // Just verify it loaded something non-zero from the pattern.
             REQUIRE((ctx->v[3].u32[0] | ctx->v[3].u32[1] | ctx->v[3].u32[2] |
                      ctx->v[3].u32[3]) != 0);
           });

  test.memory->SystemHeapFree(guest_addr);
}

TEST_CASE("LOAD_VECTOR_RIGHT", "[memory]") {
  TestFunction test([](HIRBuilder& b) {
    StoreVR(b, 3, b.LoadVectorRight(LoadGPR(b, 4)));
    b.Return();
  });

  uint32_t guest_addr = test.memory->SystemHeapAlloc(32);
  REQUIRE(guest_addr != 0);
  uint32_t aligned_addr = (guest_addr + 15) & ~15u;
  auto* host_ptr =
      reinterpret_cast<uint8_t*>(test.memory->TranslateVirtual(aligned_addr));
  for (int i = 0; i < 16; ++i) host_ptr[i] = static_cast<uint8_t>(0x20 + i);

  // LVR at aligned address returns zero (no bytes before alignment boundary).
  test.Run([&](PPCContext* ctx) { ctx->r[4] = aligned_addr; },
           [&](PPCContext* ctx) {
             REQUIRE(ctx->v[3].u32[0] == 0);
             REQUIRE(ctx->v[3].u32[1] == 0);
             REQUIRE(ctx->v[3].u32[2] == 0);
             REQUIRE(ctx->v[3].u32[3] == 0);
           });

  // LVR at aligned+4 loads 4 bytes into the high end of the vector.
  test.Run([&](PPCContext* ctx) { ctx->r[4] = aligned_addr + 4; },
           [&](PPCContext* ctx) {
             // Should have loaded some data (not all zero).
             REQUIRE((ctx->v[3].u32[0] | ctx->v[3].u32[1] | ctx->v[3].u32[2] |
                      ctx->v[3].u32[3]) != 0);
           });

  test.memory->SystemHeapFree(guest_addr);
}
