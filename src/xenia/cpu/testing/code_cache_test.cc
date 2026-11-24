/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/code_cache_base.h"

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::backend {
namespace {

class TestCodeCache : public CodeCacheBase<TestCodeCache> {
 public:
  void AddRange(uint32_t start, uint32_t end, GuestFunction* function) {
    generated_code_map_.emplace_back((uint64_t(start) << 32) | end, function);
  }
  void* LookupUnwindInfo(uint64_t) override { return nullptr; }
};

class TestFunction : public GuestFunction {
 public:
  TestFunction() : GuestFunction(nullptr, 0) {}
  uint8_t* machine_code() const override { return nullptr; }
  size_t machine_code_length() const override { return 0; }
  bool CallImpl(ThreadState*, uint32_t) override { return false; }
};

TEST_CASE("Code cache resolves half-open host PC ranges", "[cpu]") {
  TestCodeCache cache;
  TestFunction first;
  TestFunction second;
  TestFunction third;
  const auto base = cache.execute_base_address();
  CHECK(cache.LookupFunction(base) == nullptr);
  cache.AddRange(16, 32, &first);
  cache.AddRange(32, 64, &second);
  cache.AddRange(80, 96, &third);
  CHECK(cache.LookupFunction(base + 15) == nullptr);
  CHECK(cache.LookupFunction(base + 16) == &first);
  CHECK(cache.LookupFunction(base + 31) == &first);
  CHECK(cache.LookupFunction(base + 32) == &second);
  CHECK(cache.LookupFunction(base + 63) == &second);
  CHECK(cache.LookupFunction(base + 64) == nullptr);
  CHECK(cache.LookupFunction(base + 79) == nullptr);
  CHECK(cache.LookupFunction(base + 80) == &third);
  CHECK(cache.LookupFunction(base + 95) == &third);
  CHECK(cache.LookupFunction(base + 96) == nullptr);
  CHECK(cache.LookupFunction(base + cache.total_size()) == nullptr);
}

}  // namespace
}  // namespace xe::cpu::backend
