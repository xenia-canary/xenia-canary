/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_X64_PERFORMANCE_MONITOR_MAC_H_
#define XENIA_CPU_BACKEND_X64_X64_PERFORMANCE_MONITOR_MAC_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/cvar.h"

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

// Performance metrics structure for JIT operations
struct JITCompilationMetrics {
  std::atomic<uint64_t> total_functions_compiled{0};
  std::atomic<uint64_t> total_compilation_time_ns{0};
  std::atomic<uint64_t> total_code_size_bytes{0};
  std::atomic<uint64_t> total_instructions_generated{0};
  std::atomic<uint64_t> cache_hits{0};
  std::atomic<uint64_t> cache_misses{0};
  std::atomic<uint64_t> optimization_passes_total{0};
  std::atomic<uint64_t> optimization_passes_successful{0};
};

// Function hotness tracking
struct FunctionHotnessData {
  std::atomic<uint64_t> execution_count{0};
  std::atomic<uint64_t> total_execution_time_ns{0};
  std::atomic<uint64_t> last_execution_timestamp_ns{0};
  uint32_t guest_address;
  size_t code_size;
  uint64_t compilation_time_ns;
  
  FunctionHotnessData(uint32_t addr, size_t size, uint64_t comp_time)
      : guest_address(addr), code_size(size), compilation_time_ns(comp_time) {}
};

// Code cache utilization metrics
struct CodeCacheMetrics {
  std::atomic<size_t> total_allocated_bytes{0};
  std::atomic<size_t> total_used_bytes{0};
  std::atomic<uint32_t> total_functions{0};
  std::atomic<uint32_t> active_functions{0};
  std::atomic<uint64_t> fragmentation_score{0};
  std::atomic<uint32_t> cache_full_events{0};
  
  double GetUtilizationPercentage() const {
    size_t allocated = total_allocated_bytes.load();
    size_t used = total_used_bytes.load();
    return allocated > 0 ? (static_cast<double>(used) / allocated) * 100.0 : 0.0;
  }
};

// Performance monitoring configuration
struct PerformanceMonitorConfig {
  bool enable_compilation_metrics = true;
  bool enable_hotness_tracking = true;
  bool enable_cache_metrics = true;
  bool enable_vtune_integration = false;
  uint32_t hot_function_threshold = 1000; // execution count threshold
  uint32_t metrics_update_interval_ms = 1000;
  size_t max_tracked_functions = 10000;
};

// Cache invalidation strategies
enum class CacheInvalidationStrategy {
  kImmediate,    // Immediately invalidate affected functions
  kLazy,         // Mark functions for lazy invalidation
  kSelective,    // Only invalidate functions with low hotness
  kAdaptive      // Choose strategy based on current cache pressure
};

// VTune integration support
struct VTuneIntegration {
  bool initialized = false;
  void* jit_domain = nullptr;
  std::mutex vtune_mutex;
  std::unordered_map<uint32_t, uint32_t> guest_to_method_id;
  uint32_t next_method_id = 0;
  
  bool Initialize();
  void Shutdown();
  void NotifyCodeGenerated(uint32_t guest_addr, void* host_addr, size_t size);
  void NotifyFunctionCompiled(uint32_t guest_addr, const char* name);
  void NotifyFunctionEntered(uint32_t guest_addr);
  void NotifyFunctionExited(uint32_t guest_addr);
  void NotifyOptimizationStart(const char* pass_name);
  void NotifyOptimizationEnd(const char* pass_name);
  uint32_t GetMethodId(uint32_t guest_addr);
};

class X64PerformanceMonitor {
 public:
  explicit X64PerformanceMonitor(const PerformanceMonitorConfig& config = {});
  ~X64PerformanceMonitor();

  // Initialization and cleanup
  bool Initialize();
  void Shutdown();

  // Configuration accessors
  PerformanceMonitorConfig& GetConfig() { return config_; }
  const PerformanceMonitorConfig& GetConfig() const { return config_; }

  // JIT compilation metrics tracking
  void BeginFunctionCompilation(uint32_t guest_address);
  void EndFunctionCompilation(uint32_t guest_address, size_t code_size, 
                              uint32_t instructions_generated);
  
  // Function execution tracking (for hotness)
  void BeginFunctionExecution(uint32_t guest_address);
  void EndFunctionExecution(uint32_t guest_address);
  
  // Code cache metrics tracking
  void UpdateCodeCacheMetrics(size_t total_allocated, size_t total_used, 
                             uint32_t total_funcs, uint32_t active_funcs);
  void NotifyCacheHit();
  void NotifyCacheMiss();
  void NotifyCacheFull();
  
  // Optimization pass tracking
  void BeginOptimizationPass(const char* pass_name);
  void EndOptimizationPass(const char* pass_name, bool success);
  
  // Metrics retrieval
  const JITCompilationMetrics& GetCompilationMetrics() const { 
    return compilation_metrics_; 
  }
  const CodeCacheMetrics& GetCodeCacheMetrics() const { 
    return cache_metrics_; 
  }
  
  // Hot functions analysis
  std::vector<FunctionHotnessData*> GetHotFunctions(uint32_t limit = 100);
  std::vector<FunctionHotnessData*> GetAllTrackedFunctions();
  FunctionHotnessData* GetFunctionData(uint32_t guest_address);
  
  // Performance reporting
  void PrintMetricsReport();
  std::string GenerateMetricsReport();
  void ResetMetrics();
  
  // VTune integration
  void EnableVTuneIntegration(bool enable) { 
    config_.enable_vtune_integration = enable; 
  }
  void NotifyCodeGenerated(uint32_t guest_addr, void* host_addr, size_t size);
  void NotifyFunctionCompiled(uint32_t guest_addr, const char* name);
  void NotifyFunctionEntered(uint32_t guest_addr);
  void NotifyFunctionExited(uint32_t guest_addr);
  void NotifyOptimizationStart(const char* pass_name);
  void NotifyOptimizationEnd(const char* pass_name);
  
  // Code cache management
  void InvalidateCodeCache(uint32_t guest_address, size_t size, 
                           CacheInvalidationStrategy strategy);
  void HandleCachePressure();
  bool ShouldEvictFunction(const FunctionHotnessData* func_data) const;
  void UpdateHardwareCounters();
  
  // Compilation analysis
  struct CompilationAnalysis {
    uint32_t hot_functions_compiled = 0;
    uint32_t cold_functions_compiled = 0;
    uint64_t total_compilation_time_ns = 0;
    uint64_t total_execution_time_ns = 0;
    uint32_t hot_functions = 0;
    double compilation_overhead_percentage = 0.0;
    double cache_hit_rate = 0.0;
  };
  CompilationAnalysis AnalyzeCompilationVsExecution();
  
  // Optimization effectiveness
  std::vector<std::pair<std::string, double>> GetOptimizationEffectiveness();

 private:
  // Internal tracking structures
  struct CompilationInfo {
    std::chrono::high_resolution_clock::time_point start_time;
    uint32_t guest_address;
  };
  
  struct ExecutionInfo {
    std::chrono::high_resolution_clock::time_point start_time;
    uint32_t guest_address;
  };
  
  struct OptimizationInfo {
    std::chrono::high_resolution_clock::time_point start_time;
    std::string pass_name;
  };

  // Thread-local storage for tracking
  thread_local static std::unique_ptr<CompilationInfo> current_compilation_;
  thread_local static std::unique_ptr<ExecutionInfo> current_execution_;
  thread_local static std::unique_ptr<OptimizationInfo> current_optimization_;
  
  // Internal methods
  void CleanupOldFunctionData();
  uint64_t GetCurrentTimestampNs() const;
  std::string GetFunctionName(uint32_t guest_address) const;

  // Data members
  PerformanceMonitorConfig config_;
  JITCompilationMetrics compilation_metrics_;
  CodeCacheMetrics cache_metrics_;
  
  // Function hotness tracking
  std::unordered_map<uint32_t, std::unique_ptr<FunctionHotnessData>> function_data_;
  mutable std::mutex function_data_mutex_;
  
  // Optimization tracking
  std::unordered_map<std::string, uint64_t> optimization_times_;
  std::unordered_map<std::string, uint64_t> optimization_success_counts_;
  mutable std::mutex optimization_mutex_;
  
  // VTune integration
  VTuneIntegration vtune_;
  
  // Performance monitoring state
  std::atomic<bool> initialized_{false};
  uint64_t last_cleanup_time_ns_ = 0;
  std::atomic<double> cache_pressure_threshold_{0.85}; // 85% threshold
  
  // Hardware performance counters
  struct HardwareCounters {
    std::atomic<uint64_t> cpu_cycles{0};
    std::atomic<uint64_t> instructions_retired{0};
    std::atomic<uint64_t> cache_references{0};
    std::atomic<uint64_t> cache_misses{0};
  };
  HardwareCounters hw_counters_;
  uint64_t last_hw_counter_update_ = 0;
  
  // CVars for configuration

};

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_X64_PERFORMANCE_MONITOR_MAC_H_