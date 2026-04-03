/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <cstring>

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// =============================================================================
// STORE_V128 — constant source
// =============================================================================
TEST_CASE("STORE_V128_CONSTANT", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto value = b.LoadConstantVec128(
        vec128i(0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x9ABCDEF0));
    b.Store(addr, value);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(16, 16);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 16);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        vec128_t result;
        std::memcpy(&result, host, 16);
        REQUIRE(result.u32[0] == 0xDEADBEEF);
        REQUIRE(result.u32[1] == 0xCAFEBABE);
        REQUIRE(result.u32[2] == 0x12345678);
        REQUIRE(result.u32[3] == 0x9ABCDEF0);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("STORE_V128_CONSTANT_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto value = b.LoadConstantVec128(
        vec128i(0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x9ABCDEF0));
    b.Store(addr, value, LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(16, 16);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 16);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        vec128_t result;
        std::memcpy(&result, host, 16);
        REQUIRE(result.u32[0] == 0xEFBEADDE);
        REQUIRE(result.u32[1] == 0xBEBAFECA);
        REQUIRE(result.u32[2] == 0x78563412);
        REQUIRE(result.u32[3] == 0xF0DEBC9A);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

// =============================================================================
// STORE integer byte-swap with constant source
// =============================================================================
TEST_CASE("STORE_I16_CONSTANT_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Store(addr, b.LoadConstantInt16(0x1234), LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(4, 4);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 4);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint16_t result;
        std::memcpy(&result, host, 2);
        REQUIRE(result == 0x3412);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("STORE_I32_CONSTANT_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Store(addr, b.LoadConstantInt32(0x12345678), LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(4, 4);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 4);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint32_t result;
        std::memcpy(&result, host, 4);
        REQUIRE(result == 0x78563412);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("STORE_I64_CONSTANT_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Store(addr, b.LoadConstantInt64(0x123456789ABCDEF0LL),
            LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(8, 8);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 8);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint64_t result;
        std::memcpy(&result, host, 8);
        REQUIRE(result == 0xF0DEBC9A78563412ULL);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

// =============================================================================
// STORE_F32 / STORE_F64 byte-swap (entirely unimplemented on x64)
// =============================================================================
TEST_CASE("STORE_F32_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto val = b.Convert(LoadFPR(b, 5), FLOAT32_TYPE);
    b.Store(addr, val, LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(4, 4);
        ctx->r[4] = addr;
        ctx->f[5] = 1.0;  // 1.0f = 0x3F800000
        std::memset(test.memory->TranslateVirtual(addr), 0, 4);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint32_t result;
        std::memcpy(&result, host, 4);
        // 0x3F800000 byte-reversed = 0x0000803F
        REQUIRE(result == 0x0000803F);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("STORE_F32_CONSTANT_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Store(addr, b.LoadConstantFloat32(1.0f), LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(4, 4);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 4);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint32_t result;
        std::memcpy(&result, host, 4);
        REQUIRE(result == 0x0000803F);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("STORE_F64_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto val = LoadFPR(b, 5);
    b.Store(addr, val, LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(8, 8);
        ctx->r[4] = addr;
        ctx->f[5] = 1.0;  // 1.0 = 0x3FF0000000000000
        std::memset(test.memory->TranslateVirtual(addr), 0, 8);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint64_t result;
        std::memcpy(&result, host, 8);
        // 0x3FF0000000000000 byte-reversed = 0x000000000000F03F
        REQUIRE(result == 0x000000000000F03FULL);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("STORE_F64_CONSTANT_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Store(addr, b.LoadConstantFloat64(1.0), LOAD_STORE_BYTE_SWAP);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(8, 8);
        ctx->r[4] = addr;
        std::memset(test.memory->TranslateVirtual(addr), 0, 8);
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        uint64_t result;
        std::memcpy(&result, host, 8);
        REQUIRE(result == 0x000000000000F03FULL);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

// =============================================================================
// MEMSET — constant size
// =============================================================================
TEST_CASE("MEMSET_32_ZERO", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Memset(addr, b.LoadZeroInt8(), b.LoadConstantInt64(32));
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(32, 32);
        ctx->r[4] = addr;
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        const uint8_t result[32] = {};
        REQUIRE(std::memcmp(result, host, 32) == 0);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("MEMSET_128_ZERO", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    b.Memset(addr, b.LoadZeroInt8(), b.LoadConstantInt64(128));
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(128, 128);
        ctx->r[4] = addr;
      },
      [&test](PPCContext* ctx) {
        auto* host =
            test.memory->TranslateVirtual(static_cast<uint32_t>(ctx->r[4]));
        const uint8_t result[128] = {};
        REQUIRE(std::memcmp(result, host, 128) == 0);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

// =============================================================================
// LOAD_F32 / LOAD_F64 byte-swap (entirely unimplemented on x64)
// =============================================================================
TEST_CASE("LOAD_F32_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto val = b.Load(addr, FLOAT32_TYPE, LOAD_STORE_BYTE_SWAP);
    StoreFPR(b, 3, b.Convert(val, FLOAT64_TYPE));
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(4, 4);
        ctx->r[4] = addr;
        // Write 1.0f (0x3F800000) byte-swapped into memory: 0x0000803F.
        uint32_t swapped = 0x0000803F;
        std::memcpy(test.memory->TranslateVirtual(addr), &swapped, 4);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        REQUIRE(result == 1.0f);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}

TEST_CASE("LOAD_F64_BYTE_SWAP", "[instr]") {
  TestFunction test([](HIRBuilder& b) {
    auto addr = LoadGPR(b, 4);
    auto val = b.Load(addr, FLOAT64_TYPE, LOAD_STORE_BYTE_SWAP);
    StoreFPR(b, 3, val);
    b.Return();
  });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = test.memory->SystemHeapAlloc(8, 8);
        ctx->r[4] = addr;
        // Write 1.0 (0x3FF0000000000000) byte-swapped into memory.
        uint64_t swapped = 0x000000000000F03FULL;
        std::memcpy(test.memory->TranslateVirtual(addr), &swapped, 8);
      },
      [&test](PPCContext* ctx) {
        REQUIRE(ctx->f[3] == 1.0);
        test.memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
      });
}
