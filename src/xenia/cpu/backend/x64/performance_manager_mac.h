/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_PERFORMANCE_MANAGER_MAC_H_
#define XENIA_CPU_BACKEND_X64_PERFORMANCE_MANAGER_MAC_H_

#include <memory>
#include <string>
#include <vector>

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

class X64PerformanceMonitor;
class X64Backend;

// Manager class for accessing JIT performance monitoring functionality
class PerformanceManager {
 public:
  static PerformanceManager& Instance();
  
  // Initialize performance monitoring system
  bool Initialize(X64Backend* backend);
  void Shutdown();
  
  // Check if performance monitoring is available
  bool IsAvailable() const { return performance_monitor_ != nullptr; }
  
  // Get performance monitor instance
  X64PerformanceMonitor* GetMonitor() const { return performance_monitor_; }
  
  // Generate and print performance reports
  void PrintReport();
  void PrintHotFunctions(uint32_t limit = 10);
  void PrintCompilationStats();
  void PrintCacheStats();
  
  // Export performance data
  bool ExportReport(const std::string& filename);
  bool ExportHotFunctions(const std::string& filename, uint32_t limit = 100);
  
  // Reset performance metrics
  void ResetMetrics();
  
  // Configuration
  void EnableVTuneIntegration(bool enable);
  void SetHotFunctionThreshold(uint32_t threshold);
  void SetDetailedMetrics(bool enable);
  
 private:
  PerformanceManager() = default;
  ~PerformanceManager() = default;
  
  X64PerformanceMonitor* performance_monitor_ = nullptr;
  X64Backend* backend_ = nullptr;
  
  // Internal helper methods
  std::string GenerateReport();
  std::vector<std::string> FormatHotFunctionList(uint32_t limit);
};

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_PERFORMANCE_MANAGER_MAC_H_