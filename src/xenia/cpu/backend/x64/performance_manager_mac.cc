/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/performance_manager_mac.h"

#include <fstream>
#include <iomanip>

#include "xenia/base/logging.h"
#if XE_PLATFORM_MACOS
#include "xenia/cpu/backend/x64/x64_backend_mac.h"
#else
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif
#include "xenia/cpu/backend/x64/x64_performance_monitor_mac.h"

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

PerformanceManager& PerformanceManager::Instance() {
  static PerformanceManager instance;
  return instance;
}

bool PerformanceManager::Initialize(X64Backend* backend) {
  if (!backend) {
    return false;
  }
  
  backend_ = backend;
  performance_monitor_ = backend->GetPerformanceMonitor();
  
  if (performance_monitor_) {
    XELOGI("Performance Manager initialized successfully");
    return true;
  }
  
  XELOGW("Performance monitor not available in backend");
  return false;
}

void PerformanceManager::Shutdown() {
  performance_monitor_ = nullptr;
  backend_ = nullptr;
}

void PerformanceManager::PrintReport() {
  if (!IsAvailable()) {
    XELOGW("Performance monitoring not available");
    return;
  }
  
  performance_monitor_->PrintMetricsReport();
}

void PerformanceManager::PrintHotFunctions(uint32_t limit) {
  if (!IsAvailable()) {
    XELOGW("Performance monitoring not available");
    return;
  }
  
  auto hot_functions = performance_monitor_->GetHotFunctions(limit);
  if (hot_functions.empty()) {
    XELOGI("No hot functions found (threshold: {})", 
              performance_monitor_->GetConfig().hot_function_threshold);
    return;
  }
  
  XELOGI("=== Top {} Hot Functions ===", limit);
  for (size_t i = 0; i < hot_functions.size(); ++i) {
    auto* func = hot_functions[i];
    uint64_t exec_count = func->execution_count.load();
    uint64_t total_time = func->total_execution_time_ns.load();
    double avg_time_us = exec_count > 0 ? 
        static_cast<double>(total_time) / (exec_count * 1000.0) : 0.0;
    
    XELOGI("{}. 0x{:08X} - Exec: {}, Avg: {:.3f}us, Size: {}B", 
              i + 1, func->guest_address, exec_count, avg_time_us, func->code_size);
  }
}

void PerformanceManager::PrintCompilationStats() {
  if (!IsAvailable()) {
    XELOGW("Performance monitoring not available");
    return;
  }
  
  const auto& metrics = performance_monitor_->GetCompilationMetrics();
  
  XELOGI("=== JIT Compilation Statistics ===");
  XELOGI("Functions Compiled: {}", metrics.total_functions_compiled.load());
  
  uint64_t total_time = metrics.total_compilation_time_ns.load();
  if (total_time > 0 && metrics.total_functions_compiled.load() > 0) {
    double avg_time_ms = static_cast<double>(total_time) / 
                         (metrics.total_functions_compiled.load() * 1000000.0);
    XELOGI("Avg Compilation Time: {:.3f} ms", avg_time_ms);
  }
  
  uint64_t total_size = metrics.total_code_size_bytes.load();
  XELOGI("Total Code Generated: {:.2f} KB", static_cast<double>(total_size) / 1024.0);
  
  uint64_t total_instructions = metrics.total_instructions_generated.load();
  if (total_instructions > 0 && metrics.total_functions_compiled.load() > 0) {
    uint64_t avg_instructions = total_instructions / metrics.total_functions_compiled.load();
    XELOGI("Avg Instructions per Function: {}", avg_instructions);
  }
  
  uint64_t hits = metrics.cache_hits.load();
  uint64_t misses = metrics.cache_misses.load();
  uint64_t total_lookups = hits + misses;
  if (total_lookups > 0) {
    double hit_rate = (static_cast<double>(hits) / total_lookups) * 100.0;
    XELOGI("Cache Hit Rate: {:.2f}% ({}/{})", hit_rate, hits, total_lookups);
  }
}

void PerformanceManager::PrintCacheStats() {
  if (!IsAvailable()) {
    XELOGW("Performance monitoring not available");
    return;
  }
  
  const auto& metrics = performance_monitor_->GetCodeCacheMetrics();
  
  XELOGI("=== Code Cache Statistics ===");
  XELOGI("Utilization: {:.2f}%", metrics.GetUtilizationPercentage());
  XELOGI("Active Functions: {} / {}", 
            metrics.active_functions.load(), metrics.total_functions.load());
  XELOGI("Cache Full Events: {}", metrics.cache_full_events.load());
  
  uint64_t fragmentation = metrics.fragmentation_score.load();
  if (fragmentation > 0) {
    XELOGI("Fragmentation Score: {:.2f}", static_cast<double>(fragmentation) / 1000.0);
  }
}

bool PerformanceManager::ExportReport(const std::string& filename) {
  if (!IsAvailable()) {
    return false;
  }
  
  std::ofstream file(filename);
  if (!file.is_open()) {
    XELOGE("Failed to open file for performance report export: {}", filename);
    return false;
  }
  
  std::string report = GenerateReport();
  file << report;
  file.close();
  
  XELOGI("Performance report exported to: {}", filename);
  return true;
}

bool PerformanceManager::ExportHotFunctions(const std::string& filename, uint32_t limit) {
  if (!IsAvailable()) {
    return false;
  }
  
  std::ofstream file(filename);
  if (!file.is_open()) {
    XELOGE("Failed to open file for hot functions export: {}", filename);
    return false;
  }
  
  auto hot_functions = performance_monitor_->GetHotFunctions(limit);
  
  file << "address,execution_count,total_time_ns,avg_time_us,code_size,compilation_time_ns\n";
  for (const auto* func : hot_functions) {
    uint64_t exec_count = func->execution_count.load();
    uint64_t total_time = func->total_execution_time_ns.load();
    double avg_time_us = exec_count > 0 ? 
        static_cast<double>(total_time) / (exec_count * 1000.0) : 0.0;
    
    file << "0x" << std::hex << func->guest_address << std::dec << ","
         << exec_count << ","
         << total_time << ","
         << std::fixed << std::setprecision(3) << avg_time_us << ","
         << func->code_size << ","
         << func->compilation_time_ns << "\n";
  }
  
  file.close();
  
  XELOGI("Hot functions exported to: {}", filename);
  return true;
}

void PerformanceManager::ResetMetrics() {
  if (!IsAvailable()) {
    XELOGW("Performance monitoring not available");
    return;
  }
  
  performance_monitor_->ResetMetrics();
  XELOGI("Performance metrics reset");
}

void PerformanceManager::EnableVTuneIntegration(bool enable) {
  if (!IsAvailable()) {
    return;
  }
  
  performance_monitor_->EnableVTuneIntegration(enable);
  XELOGI("VTune integration: {}", enable ? "enabled" : "disabled");
}

void PerformanceManager::SetHotFunctionThreshold(uint32_t threshold) {
  if (!IsAvailable()) {
    return;
  }
  
  auto& config = performance_monitor_->GetConfig();
  config.hot_function_threshold = threshold;
  XELOGI("Hot function threshold set to: {}", threshold);
}

void PerformanceManager::SetDetailedMetrics(bool enable) {
  if (!IsAvailable()) {
    return;
  }
  
  auto& config = performance_monitor_->GetConfig();
  config.enable_cache_metrics = enable;
  XELOGI("Detailed metrics: {}", enable ? "enabled" : "disabled");
}

std::string PerformanceManager::GenerateReport() {
  if (!IsAvailable()) {
    return "Performance monitoring not available\n";
  }
  
  return performance_monitor_->GenerateMetricsReport();
}

std::vector<std::string> PerformanceManager::FormatHotFunctionList(uint32_t limit) {
  std::vector<std::string> formatted_list;
  
  if (!IsAvailable()) {
    formatted_list.push_back("Performance monitoring not available");
    return formatted_list;
  }
  
  auto hot_functions = performance_monitor_->GetHotFunctions(limit);
  
  for (const auto* func : hot_functions) {
    uint64_t exec_count = func->execution_count.load();
    uint64_t total_time = func->total_execution_time_ns.load();
    double avg_time_us = exec_count > 0 ? 
        static_cast<double>(total_time) / (exec_count * 1000.0) : 0.0;
    
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setfill('0') << std::setw(8) 
        << func->guest_address << std::dec << " - Exec: " << exec_count
        << ", Avg: " << std::fixed << std::setprecision(3) << avg_time_us 
        << "μs, Size: " << func->code_size << "B";
    
    formatted_list.push_back(oss.str());
  }
  
  return formatted_list;
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe