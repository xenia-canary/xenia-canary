/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_THUNK_OPTIMIZER_MAC_H_
#define XENIA_CPU_BACKEND_X64_THUNK_OPTIMIZER_MAC_H_

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

// Forward declarations
class X64Backend;
class X64PerformanceMonitor;

// Thunk optimization levels
enum class ThunkOptimizationLevel {
  kMinimal,     // Fastest compilation, minimal optimization
  kBalanced,    // Balance between compilation speed and runtime performance
  kAggressive    // Maximum runtime performance, slower compilation
};

// Function dispatch strategies
enum class DispatchStrategy {
  kDirect,       // Direct function calls (fastest)
  kIndirect,     // Indirect through table (memory overhead)
  kHybrid,       // Hot functions direct, cold indirect
  kAdaptive      // Dynamic selection based on patterns
};

// Register preservation strategies
enum class RegisterPreservationStrategy {
  kFull,         // Preserve all registers (safest, slowest)
  kMinimal,       // Preserve only volatile registers (faster)
  kSmart,         // Analyze and preserve only what's needed
  kContextual     // Context-aware preservation
};

// Thunk performance metrics
struct ThunkMetrics {
  std::atomic<uint64_t> host_to_guest_calls{0};
  std::atomic<uint64_t> guest_to_host_calls{0};
  std::atomic<uint64_t> direct_calls{0};
  std::atomic<uint64_t> indirect_calls{0};
  std::atomic<uint64_t> register_save_cycles{0};
  std::atomic<uint64_t> register_restore_cycles{0};
  std::atomic<uint64_t> dispatch_overhead_cycles{0};
  
  // Latency measurements in nanoseconds
  std::atomic<uint64_t> avg_host_to_guest_latency_ns{0};
  std::atomic<uint64_t> avg_guest_to_host_latency_ns{0};
  std::atomic<uint64_t> min_host_to_guest_latency_ns{UINT64_MAX};
  std::atomic<uint64_t> min_guest_to_host_latency_ns{UINT64_MAX};
};

// Function hotness tracking for thunk optimization
struct ThunkHotnessData {
  uint32_t guest_address;
  std::atomic<uint64_t> call_count{0};
  std::atomic<uint64_t> total_latency_ns{0};
  std::atomic<bool> is_hot{false};
  ThunkOptimizationLevel current_optimization{ThunkOptimizationLevel::kMinimal};
  
  uint64_t GetAverageLatency() const {
    uint64_t calls = call_count.load();
    return calls > 0 ? total_latency_ns.load() / calls : 0;
  }
};

class ThunkOptimizer {
 public:
  explicit ThunkOptimizer(X64Backend* backend, X64PerformanceMonitor* perf_monitor);
  ~ThunkOptimizer();
  
  // Initialization
  bool Initialize();
  void Shutdown();
  
  // Configuration
  void SetOptimizationLevel(ThunkOptimizationLevel level);
  void SetDispatchStrategy(DispatchStrategy strategy);
  void SetRegisterPreservationStrategy(RegisterPreservationStrategy strategy);
  void SetHotThreshold(uint32_t threshold);
  
  // Main optimization interface
  void* OptimizeHostToGuestThunk(uint32_t guest_address, void* host_function);
  void* OptimizeGuestToHostThunk(uint32_t guest_address, void* host_function);
  void* OptimizeFunctionDispatch(uint32_t guest_address, const std::vector<uint32_t>& candidates);
  
  // Hotness management
  void UpdateFunctionHotness(uint32_t guest_address, uint64_t latency_ns);
  void RecompileHotFunctions();
  
  // Performance monitoring
  const ThunkMetrics& GetMetrics() const { return metrics_; }
  void ResetMetrics();
  std::vector<ThunkHotnessData*> GetHotFunctions(uint32_t limit = 100);
  
 private:
  // Internal data structures
  struct OptimizedThunk {
    void* thunk_address;
    ThunkOptimizationLevel optimization_level;
    DispatchStrategy dispatch_strategy;
    RegisterPreservationStrategy preservation_strategy;
    uint64_t creation_timestamp;
    uint32_t guest_address;
  };
  
  // Hot function cache
  struct HotFunctionEntry {
    uint32_t guest_address;
    void* optimized_thunk;
    uint32_t last_access_time;
    uint32_t access_count;
    ThunkOptimizationLevel optimization_level;
  };
  
  // Core optimization functions
  void* CreateMinimalThunk(uint32_t guest_address, void* host_function);
  void* CreateBalancedThunk(uint32_t guest_address, void* host_function);
  void* CreateAggressiveThunk(uint32_t guest_address, void* host_function);
  void* CreateDirectDispatchThunk(uint32_t guest_address);
  void* CreateHybridDispatchThunk(const std::vector<uint32_t>& hot_functions);
  
  // Register optimization functions
  void AnalyzeRegisterUsage(uint32_t guest_address, std::vector<bool>& used_registers);
  void CreateMinimalRegisterSave(void*& save_code, size_t& save_size);
  void CreateSmartRegisterSave(uint32_t guest_address, void*& save_code, size_t& save_size);
  
  // Platform-specific optimizations
  void ApplyCPUOptimizations(void* code, size_t code_size);
  void ApplyCacheOptimizations(void* code, size_t code_size);
  void ApplyBranchPredictionOptimizations(void* code, size_t code_size);
  
  // Management functions
  void CleanupOldThunks();
  void UpdateDispatchTable();
  bool ShouldRecompile(uint32_t guest_address, const ThunkHotnessData& hotness);
  
  // Assembly generation helpers
  void GenerateProlog(void*& code, RegisterPreservationStrategy strategy);
  void GenerateEpilog(void*& code, RegisterPreservationStrategy strategy);
  void GenerateDispatchJump(void*& code, uint32_t target_address, DispatchStrategy strategy);
  
  // Performance tracking
  void RecordLatency(uint32_t guest_address, uint64_t start_time, uint64_t end_time);
  void UpdateMetrics();
  
  // Timing utilities
  static uint64_t GetCurrentTimestampNs();
  
  // Data members
  X64Backend* backend_;
  X64PerformanceMonitor* performance_monitor_;
  
  // Configuration
  ThunkOptimizationLevel optimization_level_{ThunkOptimizationLevel::kBalanced};
  DispatchStrategy dispatch_strategy_{DispatchStrategy::kAdaptive};
  RegisterPreservationStrategy preservation_strategy_{RegisterPreservationStrategy::kSmart};
  uint32_t hot_threshold_{100}; // calls to become hot
  
  // Thunk storage
  std::unordered_map<uint32_t, std::unique_ptr<OptimizedThunk>> optimized_thunks_;
  std::vector<HotFunctionEntry> hot_function_cache_;
  
  // Performance tracking
  std::unordered_map<uint32_t, std::unique_ptr<ThunkHotnessData>> hotness_data_;
  ThunkMetrics metrics_;
  
  // Timing and state
  std::atomic<bool> initialized_{false};
  uint64_t start_time_counter_;
  uint32_t last_cleanup_time_;
  
  // CPU feature cache for optimization decisions
  bool has_avx2_{false};
  bool has_bmi1_{false};
  bool has_movbe_{false};
  bool is_apple_silicon_{false};
};

// Inline assembly helpers for low-level thunk optimization
namespace thunk_asm {
  
// Fast register preservation using XMM registers
inline void FastRegisterSave(void* stack_ptr) {
#ifdef _WIN32
  __asm {
    mov eax, [stack_ptr]
    movups xmm0, [eax]
    movups xmm1, [eax + 16]
    movups xmm2, [eax + 32]
    movups xmm3, [eax + 48]
    movups xmm4, [eax + 64]
    movups xmm5, [eax + 80]
    movups xmm6, [eax + 96]
    movups xmm7, [eax + 112]
  }
#endif
}

inline void FastRegisterRestore(void* stack_ptr) {
#ifdef _WIN32
  __asm {
    mov eax, [stack_ptr]
    movups [eax], xmm0
    movups [eax + 16], xmm1
    movups [eax + 32], xmm2
    movups [eax + 48], xmm3
    movups [eax + 64], xmm4
    movups [eax + 80], xmm5
    movups [eax + 96], xmm6
    movups [eax + 112], xmm7
  }
#endif
}

// High-precision timing for latency measurement
inline uint64_t ReadTSC() {
#ifdef _WIN32
  return __rdtsc();
#else
  uint32_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return (static_cast<uint64_t>(high) << 32) | low;
#endif
}

// Optimized dispatch with branch prediction hints
inline void* PredictiveDispatch(uint32_t guest_address, void** jump_table) {
  // Use branch prediction hints for likely targets
#ifdef _WIN32
  void* result;
  __asm {
    mov eax, guest_address
    mov edx, [jump_table]  // Jump table base
    mov edx, [edx + eax*8]  // Indirect jump
    mov result, edx
  }
  return result;
#else
  return jump_table[guest_address];
#endif
}

}  // namespace thunk_asm

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_THUNK_OPTIMIZER_MAC_H_