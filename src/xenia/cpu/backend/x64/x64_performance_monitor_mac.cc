/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_performance_monitor_mac.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#if XE_PLATFORM_WIN32
#include <windows.h>
#endif

#ifdef ENABLE_VTUNE
#include "../../../third_party/DirectXShaderCompiler/lib/ExecutionEngine/IntelJITEvents/jitprofiling.h"
#include "../../../third_party/DirectXShaderCompiler/lib/ExecutionEngine/IntelJITEvents/ittnotify.h"
#endif

#include "xenia/base/logging.h"
#if XE_PLATFORM_MACOS
#include "xenia/base/math_mac.h"
#else
#include "xenia/base/math.h"
#endif
#include "xenia/base/platform.h"
#if XE_PLATFORM_MACOS
#include "xenia/cpu/backend/x64/x64_backend_mac.h"
#else
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

// Thread-local storage definitions
thread_local std::unique_ptr<X64PerformanceMonitor::CompilationInfo> 
    X64PerformanceMonitor::current_compilation_ = nullptr;
thread_local std::unique_ptr<X64PerformanceMonitor::ExecutionInfo> 
    X64PerformanceMonitor::current_execution_ = nullptr;
thread_local std::unique_ptr<X64PerformanceMonitor::OptimizationInfo> 
    X64PerformanceMonitor::current_optimization_ = nullptr;

// VTune integration implementation
bool VTuneIntegration::Initialize() {
#ifdef ENABLE_VTUNE
  // Initialize VTune JIT profiling
  iJIT_DLL_Initialize();
  
  // Set notification mode
  iJIT_DLL_SetEventMode(iJIT_BE_NOTIFY_ON_LOAD | iJIT_BE_NOTIFY_ON_UNLOAD);
  
  // Create VTune domain for profiling
#ifdef __ITTNOTIFY_H__
  if (itt_domain_create && itt_thread_set_name) {
    jit_domain = itt_domain_create("Xenia.JIT");
    if (jit_domain) {
      itt_thread_set_name("Xenia JIT Compiler");
      initialized = true;
      XELOGI("VTune integration initialized for JIT profiling");
      return true;
    }
  }
#else
  // Fallback if ITT headers not available
  initialized = true;
  XELOGI("VTune JIT profiling initialized (basic mode)");
  return true;
#endif
#endif
  return false;
}

void VTuneIntegration::Shutdown() {
  if (initialized && jit_domain) {
#if XE_PLATFORM_WIN32 && defined(ITTNOTIFY_API_AVAILABLE)
    // VTune cleanup is automatic
#endif
    jit_domain = nullptr;
    initialized = false;
  }
}

void VTuneIntegration::NotifyCodeGenerated(uint32_t guest_addr, void* host_addr, 
                                            size_t size) {
#ifdef ENABLE_VTUNE
  if (!initialized) return;
  
  // Create method notification for VTune
  iJIT_Method_Load_V2 method_load = {0};
  method_load.method_id = GetMethodId(guest_addr);
  method_load.method_load_address = reinterpret_cast<void*>(host_addr);
  method_load.method_size = static_cast<unsigned int>(size);
  
  // Create method name
  char method_name[128];
  snprintf(method_name, sizeof(method_name), "xenia_guest_0x%08X", guest_addr);
  method_load.method_name = method_name;
  
  // Set class/file info (optional but helpful for debugging)
  static char class_name[] = "Xenia.Guest";
  static char file_name[] = "xenia_jit";
  method_load.class_file_name = class_name;
  method_load.source_file_name = file_name;
  
  // Notify VTune of method load
  iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED_V2, &method_load);
  
#ifdef __ITTNOTIFY_H__
  if (jit_domain && itt_function_begin) {
    // Also notify ITT for more detailed tracing
    __itt_id method_id = __itt_id_make(reinterpret_cast<void*>(guest_addr), 0);
    itt_function_begin(jit_domain, method_id, nullptr);
  }
#endif
#endif
}

void VTuneIntegration::NotifyFunctionCompiled(uint32_t guest_addr, 
                                               const char* name) {
#ifdef ENABLE_VTUNE
  if (!initialized) return;
  
  // Additional notification for function compilation completion
#ifdef __ITTNOTIFY_H__
  if (jit_domain && itt_function_end) {
    __itt_id method_id = __itt_id_make(reinterpret_cast<void*>(guest_addr), 0);
    itt_function_end(jit_domain, method_id);
  }
#endif
#endif
}

void VTuneIntegration::NotifyFunctionEntered(uint32_t guest_addr) {
#ifdef ENABLE_VTUNE
  if (!initialized) return;
  
  // Send method entry notification for call graph profiling
  iJIT_Method_NIDS method_entry = {0};
  method_entry.method_id = GetMethodId(guest_addr);
  method_entry.method_name = const_cast<char*>("xenia_guest_function");
  
  iJIT_NotifyEvent(iJVM_EVENT_TYPE_ENTER_NIDS, &method_entry);
#endif
}

void VTuneIntegration::NotifyFunctionExited(uint32_t guest_addr) {
#ifdef ENABLE_VTUNE
  if (!initialized) return;
  
  // Send method exit notification for call graph profiling
  iJIT_Method_NIDS method_exit = {0};
  method_exit.method_id = GetMethodId(guest_addr);
  method_exit.method_name = const_cast<char*>("xenia_guest_function");
  
  iJIT_NotifyEvent(iJVM_EVENT_TYPE_LEAVE_NIDS, &method_exit);
#endif
}

void VTuneIntegration::NotifyOptimizationStart(const char* pass_name) {
#ifdef ENABLE_VTUNE
  if (!initialized) return;
  
#ifdef __ITTNOTIFY_H__
  if (jit_domain && itt_task_begin) {
    itt_task_begin(jit_domain, __itt_null, __itt_null, pass_name);
  }
#endif
#endif
}

void VTuneIntegration::NotifyOptimizationEnd(const char* pass_name) {
#ifdef ENABLE_VTUNE
  if (!initialized) return;
  
#ifdef __ITTNOTIFY_H__
  if (jit_domain && itt_task_end) {
    itt_task_end(jit_domain);
  }
#endif
#endif
}

uint32_t VTuneIntegration::GetMethodId(uint32_t guest_addr) {
  std::lock_guard<std::mutex> lock(vtune_mutex);
  
  auto it = guest_to_method_id.find(guest_addr);
  if (it != guest_to_method_id.end()) {
    return it->second;
  }
  
  uint32_t method_id = next_method_id++;
  guest_to_method_id[guest_addr] = method_id;
  return method_id;
}

// X64PerformanceMonitor implementation
X64PerformanceMonitor::X64PerformanceMonitor(
    const PerformanceMonitorConfig& config) 
    : config_(config) {
}

X64PerformanceMonitor::~X64PerformanceMonitor() {
  Shutdown();
}

bool X64PerformanceMonitor::Initialize() {
  if (initialized_.load()) {
    return true;
  }

  // Initialize VTune if requested
  if (config_.enable_vtune_integration) {
    if (!vtune_.Initialize()) {
      XELOGW("Failed to initialize VTune integration");
      config_.enable_vtune_integration = false;
    }
  }

  // Apply CVars to config
  config_.hot_function_threshold = ::cvars::perf_monitor_hot_threshold;
  config_.enable_vtune_integration = ::cvars::perf_monitor_enable_vtune;
  config_.enable_cache_metrics = ::cvars::perf_monitor_detailed_metrics;

  initialized_.store(true);
  last_cleanup_time_ns_ = GetCurrentTimestampNs();
  
  XELOGI("X64 Performance Monitor initialized");
  return true;
}

void X64PerformanceMonitor::Shutdown() {
  if (!initialized_.load()) {
    return;
  }

  vtune_.Shutdown();
  
  {
    std::lock_guard<std::mutex> lock(function_data_mutex_);
    function_data_.clear();
  }
  
  {
    std::lock_guard<std::mutex> lock(optimization_mutex_);
    optimization_times_.clear();
    optimization_success_counts_.clear();
  }

  initialized_.store(false);
  XELOGI("X64 Performance Monitor shutdown");
}

uint64_t X64PerformanceMonitor::GetCurrentTimestampNs() const {
  auto now = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             now.time_since_epoch()).count();
}

void X64PerformanceMonitor::BeginFunctionCompilation(uint32_t guest_address) {
  if (!initialized_.load() || !config_.enable_compilation_metrics) {
    return;
  }

  current_compilation_ = std::make_unique<CompilationInfo>();
  current_compilation_->start_time = std::chrono::high_resolution_clock::now();
  current_compilation_->guest_address = guest_address;
}

void X64PerformanceMonitor::EndFunctionCompilation(uint32_t guest_address, 
                                                  size_t code_size,
                                                  uint32_t instructions_generated) {
  if (!initialized_.load() || !config_.enable_compilation_metrics || 
      !current_compilation_) {
    return;
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end_time - current_compilation_->start_time).count();

  // Update compilation metrics
  compilation_metrics_.total_functions_compiled.fetch_add(1);
  compilation_metrics_.total_compilation_time_ns.fetch_add(duration_ns);
  compilation_metrics_.total_code_size_bytes.fetch_add(code_size);
  compilation_metrics_.total_instructions_generated.fetch_add(instructions_generated);

  // Create function hotness data
  if (config_.enable_hotness_tracking) {
    std::lock_guard<std::mutex> lock(function_data_mutex_);
    
    auto it = function_data_.find(guest_address);
    if (it == function_data_.end()) {
      // Limit the number of tracked functions
      if (function_data_.size() < config_.max_tracked_functions) {
        auto func_data = std::make_unique<FunctionHotnessData>(
            guest_address, code_size, duration_ns);
        function_data_[guest_address] = std::move(func_data);
      }
    } else {
      it->second->code_size = code_size;
      it->second->compilation_time_ns = duration_ns;
    }
  }

  // Notify VTune if enabled
  if (config_.enable_vtune_integration) {
    // VTune notification will be handled when code is placed in cache
  }

  current_compilation_.reset();
}

void X64PerformanceMonitor::BeginFunctionExecution(uint32_t guest_address) {
  if (!initialized_.load() || !config_.enable_hotness_tracking) {
    return;
  }

  current_execution_ = std::make_unique<ExecutionInfo>();
  current_execution_->start_time = std::chrono::high_resolution_clock::now();
  current_execution_->guest_address = guest_address;
}

void X64PerformanceMonitor::EndFunctionExecution(uint32_t guest_address) {
  if (!initialized_.load() || !config_.enable_hotness_tracking || 
      !current_execution_) {
    return;
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end_time - current_execution_->start_time).count();

  std::lock_guard<std::mutex> lock(function_data_mutex_);
  
  auto it = function_data_.find(guest_address);
  if (it != function_data_.end()) {
    it->second->execution_count.fetch_add(1);
    it->second->total_execution_time_ns.fetch_add(duration_ns);
    it->second->last_execution_timestamp_ns.store(GetCurrentTimestampNs());
  }

  current_execution_.reset();
}

void X64PerformanceMonitor::UpdateCodeCacheMetrics(size_t total_allocated, 
                                                   size_t total_used,
                                                   uint32_t total_funcs,
                                                   uint32_t active_funcs) {
  if (!initialized_.load() || !config_.enable_cache_metrics) {
    return;
  }

  cache_metrics_.total_allocated_bytes.store(total_allocated);
  cache_metrics_.total_used_bytes.store(total_used);
  cache_metrics_.total_functions.store(total_funcs);
  cache_metrics_.active_functions.store(active_funcs);

  // Calculate fragmentation score (simplified)
  if (total_allocated > 0) {
    double utilization = static_cast<double>(total_used) / total_allocated;
    uint64_t fragmentation = static_cast<uint64_t>((1.0 - utilization) * 1000000);
    cache_metrics_.fragmentation_score.store(fragmentation);
  }
}

void X64PerformanceMonitor::NotifyCacheHit() {
  if (initialized_.load()) {
    compilation_metrics_.cache_hits.fetch_add(1);
  }
}

void X64PerformanceMonitor::NotifyCacheMiss() {
  if (initialized_.load()) {
    compilation_metrics_.cache_misses.fetch_add(1);
  }
}

void X64PerformanceMonitor::NotifyCacheFull() {
  if (initialized_.load()) {
    cache_metrics_.cache_full_events.fetch_add(1);
    XELOGW("JIT code cache full event detected");
  }
}

void X64PerformanceMonitor::BeginOptimizationPass(const char* pass_name) {
  if (!initialized_.load() || !config_.enable_compilation_metrics) {
    return;
  }

  current_optimization_ = std::make_unique<OptimizationInfo>();
  current_optimization_->start_time = std::chrono::high_resolution_clock::now();
  current_optimization_->pass_name = pass_name ? pass_name : "unknown";
}

void X64PerformanceMonitor::EndOptimizationPass(const char* pass_name, bool success) {
  if (!initialized_.load() || !config_.enable_compilation_metrics || 
      !current_optimization_) {
    return;
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end_time - current_optimization_->start_time).count();

  std::lock_guard<std::mutex> lock(optimization_mutex_);
  
  std::string name = pass_name ? pass_name : current_optimization_->pass_name;
  optimization_times_[name] += duration_ns;
  
  if (success) {
    optimization_success_counts_[name]++;
  }
  
  compilation_metrics_.optimization_passes_total.fetch_add(1);
  if (success) {
    compilation_metrics_.optimization_passes_successful.fetch_add(1);
  }

  current_optimization_.reset();
}

std::vector<FunctionHotnessData*> X64PerformanceMonitor::GetHotFunctions(
    uint32_t limit) {
  std::vector<FunctionHotnessData*> hot_functions;
  
  if (!initialized_.load()) {
    return hot_functions;
  }

  std::lock_guard<std::mutex> lock(function_data_mutex_);
  
  for (const auto& pair : function_data_) {
    if (pair.second->execution_count.load() >= config_.hot_function_threshold) {
      hot_functions.push_back(pair.second.get());
    }
  }
  
  // Sort by execution count (descending)
  std::sort(hot_functions.begin(), hot_functions.end(),
            [](const FunctionHotnessData* a, const FunctionHotnessData* b) {
              return a->execution_count.load() > b->execution_count.load();
            });
  
  if (hot_functions.size() > limit) {
    hot_functions.resize(limit);
  }
  
  return hot_functions;
}

std::vector<FunctionHotnessData*> X64PerformanceMonitor::GetAllTrackedFunctions() {
  std::vector<FunctionHotnessData*> all_functions;
  
  if (!initialized_.load()) {
    return all_functions;
  }

  std::lock_guard<std::mutex> lock(function_data_mutex_);
  
  all_functions.reserve(function_data_.size());
  for (const auto& pair : function_data_) {
    all_functions.push_back(pair.second.get());
  }
  
  return all_functions;
}

FunctionHotnessData* X64PerformanceMonitor::GetFunctionData(uint32_t guest_address) {
  if (!initialized_.load()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(function_data_mutex_);
  
  auto it = function_data_.find(guest_address);
  return (it != function_data_.end()) ? it->second.get() : nullptr;
}

void X64PerformanceMonitor::PrintMetricsReport() {
  std::string report = GenerateMetricsReport();
  XELOGI("JIT Performance Metrics Report:\n{}", report);
}

std::string X64PerformanceMonitor::GenerateMetricsReport() {
  std::ostringstream report;
  
  if (!initialized_.load()) {
    report << "Performance Monitor not initialized\n";
    return report.str();
  }

  // Compilation Metrics
  report << "=== JIT Compilation Metrics ===\n";
  report << "Total Functions Compiled: " << compilation_metrics_.total_functions_compiled.load() << "\n";
  
  uint64_t total_comp_time_ns = compilation_metrics_.total_compilation_time_ns.load();
  if (total_comp_time_ns > 0 && compilation_metrics_.total_functions_compiled.load() > 0) {
    double avg_comp_time_ms = static_cast<double>(total_comp_time_ns) / 
                               (compilation_metrics_.total_functions_compiled.load() * 1000000.0);
    report << "Average Compilation Time: " << std::fixed << std::setprecision(3) 
           << avg_comp_time_ms << " ms\n";
  }
  
  uint64_t total_code_size = compilation_metrics_.total_code_size_bytes.load();
  report << "Total Code Size: " << (total_code_size / 1024) << " KB\n";
  
  uint64_t total_instructions = compilation_metrics_.total_instructions_generated.load();
  if (total_instructions > 0 && compilation_metrics_.total_functions_compiled.load() > 0) {
    uint64_t avg_instructions = total_instructions / compilation_metrics_.total_functions_compiled.load();
    report << "Average Instructions per Function: " << avg_instructions << "\n";
  }
  
  // Cache Performance
  uint64_t hits = compilation_metrics_.cache_hits.load();
  uint64_t misses = compilation_metrics_.cache_misses.load();
  uint64_t total_lookups = hits + misses;
  if (total_lookups > 0) {
    double hit_rate = (static_cast<double>(hits) / total_lookups) * 100.0;
    report << "Cache Hit Rate: " << std::fixed << std::setprecision(2) << hit_rate << "%\n";
  }
  
  // Optimization Passes
  uint64_t total_passes = compilation_metrics_.optimization_passes_total.load();
  uint64_t successful_passes = compilation_metrics_.optimization_passes_successful.load();
  if (total_passes > 0) {
    double success_rate = (static_cast<double>(successful_passes) / total_passes) * 100.0;
    report << "Optimization Pass Success Rate: " << std::fixed << std::setprecision(2) 
           << success_rate << "% (" << successful_passes << "/" << total_passes << ")\n";
  }
  
  report << "\n=== Code Cache Metrics ===\n";
  report << "Cache Utilization: " << std::fixed << std::setprecision(2) 
         << cache_metrics_.GetUtilizationPercentage() << "%\n";
  report << "Active Functions: " << cache_metrics_.active_functions.load() 
         << " / " << cache_metrics_.total_functions.load() << "\n";
  report << "Cache Full Events: " << cache_metrics_.cache_full_events.load() << "\n";
  
  // Hot Functions
  if (config_.enable_hotness_tracking) {
    auto hot_functions = GetHotFunctions(10);
    if (!hot_functions.empty()) {
      report << "\n=== Top Hot Functions ===\n";
      for (size_t i = 0; i < hot_functions.size() && i < 10; ++i) {
        auto* func = hot_functions[i];
        uint64_t exec_count = func->execution_count.load();
        uint64_t total_time = func->total_execution_time_ns.load();
        double avg_time_us = exec_count > 0 ? 
            static_cast<double>(total_time) / (exec_count * 1000.0) : 0.0;
        
        report << i + 1 << ". 0x" << std::hex << func->guest_address << std::dec
               << " - Exec: " << exec_count 
               << ", Avg: " << std::fixed << std::setprecision(3) << avg_time_us << "μs"
               << ", Size: " << func->code_size << "B\n";
      }
    }
  }
  
  return report.str();
}

void X64PerformanceMonitor::ResetMetrics() {
  if (!initialized_.load()) {
    return;
  }

  compilation_metrics_.total_functions_compiled.store(0);
  compilation_metrics_.total_compilation_time_ns.store(0);
  compilation_metrics_.total_code_size_bytes.store(0);
  compilation_metrics_.total_instructions_generated.store(0);
  compilation_metrics_.cache_hits.store(0);
  compilation_metrics_.cache_misses.store(0);
  compilation_metrics_.optimization_passes_total.store(0);
  compilation_metrics_.optimization_passes_successful.store(0);
  
  cache_metrics_.total_allocated_bytes.store(0);
  cache_metrics_.total_used_bytes.store(0);
  cache_metrics_.total_functions.store(0);
  cache_metrics_.active_functions.store(0);
  cache_metrics_.fragmentation_score.store(0);
  cache_metrics_.cache_full_events.store(0);
  
  {
    std::lock_guard<std::mutex> lock(function_data_mutex_);
    function_data_.clear();
  }
  
  {
    std::lock_guard<std::mutex> lock(optimization_mutex_);
    optimization_times_.clear();
    optimization_success_counts_.clear();
  }
  
  XELOGI("Performance metrics reset");
}

void X64PerformanceMonitor::NotifyCodeGenerated(uint32_t guest_addr, void* host_addr, 
                                                size_t size) {
  if (config_.enable_vtune_integration) {
    vtune_.NotifyCodeGenerated(guest_addr, host_addr, size);
  }
}

void X64PerformanceMonitor::NotifyFunctionCompiled(uint32_t guest_addr, 
                                                    const char* name) {
  if (config_.enable_vtune_integration) {
    vtune_.NotifyFunctionCompiled(guest_addr, name);
  }
}

void X64PerformanceMonitor::NotifyFunctionEntered(uint32_t guest_addr) {
  if (config_.enable_vtune_integration) {
    vtune_.NotifyFunctionEntered(guest_addr);
  }
}

void X64PerformanceMonitor::NotifyFunctionExited(uint32_t guest_addr) {
  if (config_.enable_vtune_integration) {
    vtune_.NotifyFunctionExited(guest_addr);
  }
}

void X64PerformanceMonitor::NotifyOptimizationStart(const char* pass_name) {
  if (config_.enable_vtune_integration) {
    vtune_.NotifyOptimizationStart(pass_name);
  }
}

void X64PerformanceMonitor::NotifyOptimizationEnd(const char* pass_name) {
  if (config_.enable_vtune_integration) {
    vtune_.NotifyOptimizationEnd(pass_name);
  }
}

void X64PerformanceMonitor::CleanupOldFunctionData() {
  if (!config_.enable_hotness_tracking) {
    return;
  }

  uint64_t current_time = GetCurrentTimestampNs();
  const uint64_t cleanup_interval_ns = 60 * 1000000000ULL; // 60 seconds
  const uint64_t max_age_ns = 300 * 1000000000ULL; // 5 minutes

  if (current_time - last_cleanup_time_ns_ < cleanup_interval_ns) {
    return;
  }

  std::lock_guard<std::mutex> lock(function_data_mutex_);
  
  auto it = function_data_.begin();
  while (it != function_data_.end()) {
    uint64_t last_execution = it->second->last_execution_timestamp_ns.load();
    if (current_time - last_execution > max_age_ns) {
      it = function_data_.erase(it);
    } else {
      ++it;
    }
  }
  
  last_cleanup_time_ns_ = current_time;
}

std::string X64PerformanceMonitor::GetFunctionName(uint32_t guest_address) const {
  // This would need to be implemented to look up function names from symbols
  // For now, just return the address
  char name[64];
  snprintf(name, sizeof(name), "guest_0x%08X", guest_address);
  return std::string(name);
}

// Enhanced cache management implementation
void X64PerformanceMonitor::InvalidateCodeCache(uint32_t guest_address, size_t size, 
                                             CacheInvalidationStrategy strategy) {
  if (!initialized_.load()) return;
  
  // Log cache invalidation for performance analysis
  //uint64_t current_time = GetCurrentTimestampNs();
  
  switch (strategy) {
    case CacheInvalidationStrategy::kImmediate:
      // Immediately invalidate affected functions
      {
        std::lock_guard<std::mutex> lock(function_data_mutex_);
        uint32_t end_addr = guest_address + static_cast<uint32_t>(size);
        auto it = function_data_.begin();
        while (it != function_data_.end()) {
          if (it->first >= guest_address && it->first < end_addr) {
            it = function_data_.erase(it);
          } else {
            ++it;
          }
        }
      }
      break;
      
    case CacheInvalidationStrategy::kLazy:
      // Mark functions for lazy invalidation
      // Implementation would depend on code cache structure
      break;
      
    case CacheInvalidationStrategy::kSelective:
      // Only invalidate functions with low hotness
      {
        std::lock_guard<std::mutex> lock(function_data_mutex_);
        uint32_t end_addr = guest_address + static_cast<uint32_t>(size);
        auto it = function_data_.begin();
        while (it != function_data_.end()) {
          if (it->first >= guest_address && it->first < end_addr) {
            // Only invalidate if function is not hot
            if (it->second->execution_count.load() < config_.hot_function_threshold) {
              it = function_data_.erase(it);
            } else {
              ++it;
            }
          } else {
            ++it;
          }
        }
      }
      break;
      
    case CacheInvalidationStrategy::kAdaptive:
      // Choose strategy based on current cache pressure
      {
        double utilization = cache_metrics_.GetUtilizationPercentage() / 100.0;
        if (utilization > cache_pressure_threshold_.load()) {
          // High pressure: use selective invalidation
          InvalidateCodeCache(guest_address, size, CacheInvalidationStrategy::kSelective);
        } else {
          // Low pressure: use immediate invalidation
          InvalidateCodeCache(guest_address, size, CacheInvalidationStrategy::kImmediate);
        }
      }
      break;
  }
  
  XELOGI("Cache invalidation: addr=0x{:08X}, size={}, strategy={}", 
          guest_address, size, static_cast<int>(strategy));
}

void X64PerformanceMonitor::HandleCachePressure() {
  if (!initialized_.load()) return;
  
  double utilization = cache_metrics_.GetUtilizationPercentage() / 100.0;
  
  if (utilization > cache_pressure_threshold_.load()) {
    XELOGW("Cache pressure detected: {:.1f}% utilization", utilization * 100.0);
    
    // Get all functions sorted by hotness (least hot first)
    auto all_functions = GetAllTrackedFunctions();
    std::sort(all_functions.begin(), all_functions.end(),
              [](const FunctionHotnessData* a, const FunctionHotnessData* b) {
                return a->execution_count.load() < b->execution_count.load();
              });
    
    // Evict cold functions to free up space
    size_t target_eviction = static_cast<size_t>(all_functions.size() * 0.1); // Evict 10%
    size_t evicted = 0;
    
    std::lock_guard<std::mutex> lock(function_data_mutex_);
    for (auto* func : all_functions) {
      if (evicted >= target_eviction) break;
      
      auto it = function_data_.find(func->guest_address);
      if (it != function_data_.end() && 
          it->second->execution_count.load() < config_.hot_function_threshold / 2) {
        function_data_.erase(it);
        evicted++;
      }
    }
    
    XELOGI("Evicted {} functions to relieve cache pressure", evicted);
  }
}

bool X64PerformanceMonitor::ShouldEvictFunction(const FunctionHotnessData* func_data) const {
  if (!func_data) return false;
  
  // Don't evict hot functions
  if (func_data->execution_count.load() >= config_.hot_function_threshold) {
    return false;
  }
  
  // Consider function age and compilation cost
  uint64_t current_time = GetCurrentTimestampNs();
  uint64_t time_since_last_exec = current_time - func_data->last_execution_timestamp_ns.load();
  uint64_t max_age_ns = 300 * 1000000000ULL; // 5 minutes
  
  if (time_since_last_exec > max_age_ns) {
    return true;
  }
  
  // Consider compilation efficiency
  if (func_data->compilation_time_ns > 10000000ULL) { // >10ms compilation
    uint64_t exec_time = func_data->total_execution_time_ns.load();
    uint64_t exec_count = func_data->execution_count.load();
    
    if (exec_count > 0) {
      uint64_t avg_exec_time = exec_time / exec_count;
      // If compilation cost is much higher than average execution time, keep it
      if (func_data->compilation_time_ns > avg_exec_time * 100) {
        return false;
      }
    }
  }
  
  return true;
}

void X64PerformanceMonitor::UpdateHardwareCounters() {
  if (!initialized_.load()) return;
  
  uint64_t current_time = GetCurrentTimestampNs();
  const uint64_t update_interval_ns = 100 * 1000000ULL; // 100ms
  
  if (current_time - last_hw_counter_update_ < update_interval_ns) {
    return;
  }
  
#if XE_PLATFORM_WIN32
  // Windows hardware counter implementation using Performance Counters
  // This would need actual Windows PMC implementation
  hw_counters_.cpu_cycles.fetch_add(1000000); // Placeholder
  hw_counters_.instructions_retired.fetch_add(800000); // Placeholder
#endif
  
#if defined(__APPLE__) && defined(__arm64__)
  // macOS ARM64 hardware counter implementation
  // This would need ARM64 PMU implementation
  hw_counters_.cpu_cycles.fetch_add(1000000); // Placeholder
  hw_counters_.instructions_retired.fetch_add(800000); // Placeholder
#endif
  
  last_hw_counter_update_ = current_time;
}

X64PerformanceMonitor::CompilationAnalysis X64PerformanceMonitor::AnalyzeCompilationVsExecution() {
  CompilationAnalysis analysis;
  
  if (!initialized_.load()) return analysis;
  
  // Get compilation metrics
  uint64_t total_comp_time_ns = compilation_metrics_.total_compilation_time_ns.load();
  uint64_t total_functions = compilation_metrics_.total_functions_compiled.load();
  analysis.hot_functions_compiled = total_functions;
  analysis.total_compilation_time_ns = static_cast<double>(total_comp_time_ns) / 1000000.0;
  
  // Calculate total execution time from hotness data
  uint64_t total_exec_time_ns = 0;
  uint64_t hot_function_count = 0;
  
  {
    std::lock_guard<std::mutex> lock(function_data_mutex_);
    for (const auto& pair : function_data_) {
      uint64_t exec_time = pair.second->total_execution_time_ns.load();
      uint64_t exec_count = pair.second->execution_count.load();
      
      total_exec_time_ns += exec_time;
      if (exec_count >= config_.hot_function_threshold) {
        hot_function_count++;
      }
    }
  }
  
  analysis.total_execution_time_ns = static_cast<double>(total_exec_time_ns) / 1000000.0;
  analysis.hot_functions = hot_function_count;
  
  // Calculate overhead
  if (analysis.total_execution_time_ns > 0) {
    analysis.compilation_overhead_percentage = 
        (analysis.total_compilation_time_ns / analysis.total_execution_time_ns) * 100.0;
  }
  
  // Calculate cache hit rate
  uint64_t hits = compilation_metrics_.cache_hits.load();
  uint64_t misses = compilation_metrics_.cache_misses.load();
  uint64_t total_lookups = hits + misses;
  if (total_lookups > 0) {
    analysis.cache_hit_rate = (static_cast<double>(hits) / total_lookups) * 100.0;
  }
  
  return analysis;
}

std::vector<std::pair<std::string, double>> X64PerformanceMonitor::GetOptimizationEffectiveness() {
  std::vector<std::pair<std::string, double>> effectiveness;
  
  if (!initialized_.load()) return effectiveness;
  
  std::lock_guard<std::mutex> lock(optimization_mutex_);
  
  for (const auto& pair : optimization_times_) {
    const std::string& pass_name = pair.first;
    uint64_t total_time_ns = pair.second;
    
    auto success_it = optimization_success_counts_.find(pass_name);
    uint64_t success_count = (success_it != optimization_success_counts_.end()) ? 
                            success_it->second : 0;
    
    // Calculate effectiveness: success rate / average time
    uint64_t total_passes = compilation_metrics_.optimization_passes_total.load();
    if (total_passes > 0 && total_time_ns > 0) {
      double avg_time_ms = static_cast<double>(total_time_ns) / (total_passes * 1000000.0);
      double success_rate = static_cast<double>(success_count) / total_passes * 100.0;
      
      // Higher effectiveness = higher success rate and lower time
      double effectiveness_score = success_rate / (avg_time_ms + 1.0);
      effectiveness.emplace_back(pass_name, effectiveness_score);
    }
  }
  
  // Sort by effectiveness (descending)
  std::sort(effectiveness.begin(), effectiveness.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  
  return effectiveness;
}

// CVars for performance monitoring
DEFINE_int32(perf_monitor_hot_threshold, 1000,
             "Execution count threshold for hot function detection.", "x64");
DEFINE_bool(perf_monitor_enable_vtune, false,
            "Enable VTune integration for JIT profiling.", "x64");
DEFINE_bool(perf_monitor_detailed_metrics, false,
            "Enable detailed performance metrics collection.", "x64");

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe