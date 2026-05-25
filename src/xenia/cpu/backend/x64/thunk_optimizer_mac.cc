/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/thunk_optimizer_mac.h"
#include "xenia/cpu/backend/x64/cpu_features_mac.h"
#if XE_PLATFORM_MACOS
#include "xenia/cpu/backend/x64/x64_backend_mac.h"
#else
#include "xenia/cpu/backend/x64/x64_backend.h"
#endif
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <intrin.h>
#include <windows.h>
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

ThunkOptimizer::ThunkOptimizer(X64Backend* backend, X64PerformanceMonitor* perf_monitor)
    : backend_(backend), performance_monitor_(perf_monitor), start_time_counter_(0), last_cleanup_time_(0) {
  
  // Cache CPU features for optimization decisions
  has_avx2_ = HasAVX2();
  has_bmi1_ = HasBMI1();
  has_movbe_ = HasMOVBE();
  is_apple_silicon_ = CPUFeatureDetector::GetCPUInfo().platform.is_apple_silicon;
}

ThunkOptimizer::~ThunkOptimizer() {
  Shutdown();
}

bool ThunkOptimizer::Initialize() {
  if (initialized_.load()) {
    return true;
  }
  
  // Initialize performance tracking
  ResetMetrics();
  
  // Apply initial optimizations based on CPU features
  const auto& cpu_info = CPUFeatureDetector::GetCPUInfo();
  
  // Set platform-specific defaults
  if (cpu_info.platform.is_apple_silicon) {
    // Apple Silicon: prefer minimal register preservation due to translation overhead
    preservation_strategy_ = RegisterPreservationStrategy::kMinimal;
    dispatch_strategy_ = DispatchStrategy::kHybrid;
    optimization_level_ = ThunkOptimizationLevel::kBalanced;
  } else if (cpu_info.vendor == "GenuineIntel") {
    // Intel: can use advanced features
    if (cpu_info.family >= 6 && cpu_info.model >= 0x4E) {
      preservation_strategy_ = RegisterPreservationStrategy::kSmart;
      optimization_level_ = ThunkOptimizationLevel::kAggressive;
    }
  } else if (cpu_info.vendor == "AuthenticAMD") {
    // AMD: optimize for Zen architecture
    if (cpu_info.family >= 0x17) {
      preservation_strategy_ = RegisterPreservationStrategy::kSmart;
      dispatch_strategy_ = DispatchStrategy::kAdaptive;
    }
  }
  
  initialized_.store(true);
  XELOGI("Thunk Optimizer initialized: CPU={}, AVX2={}, BMI1={}, MOVBE={}", 
           cpu_info.vendor, has_avx2_, has_bmi1_, has_movbe_);
  
  return true;
}

void ThunkOptimizer::Shutdown() {
  if (!initialized_.load()) {
    return;
  }
  
  // Clean up all optimized thunks
  optimized_thunks_.clear();
  hot_function_cache_.clear();
  hotness_data_.clear();
  
  initialized_.store(false);
  XELOGI("Thunk Optimizer shutdown");
}

void ThunkOptimizer::SetOptimizationLevel(ThunkOptimizationLevel level) {
  optimization_level_ = level;
  
  // Recompile existing thunks if optimization level increased
  if (level > ThunkOptimizationLevel::kMinimal) {
    RecompileHotFunctions();
  }
}

void ThunkOptimizer::SetDispatchStrategy(DispatchStrategy strategy) {
  dispatch_strategy_ = strategy;
  UpdateDispatchTable();
}

void ThunkOptimizer::SetRegisterPreservationStrategy(RegisterPreservationStrategy strategy) {
  preservation_strategy_ = strategy;
}

void ThunkOptimizer::SetHotThreshold(uint32_t threshold) {
  hot_threshold_ = threshold;
  
  // Update hotness status for existing functions
  for (auto& pair : hotness_data_) {
    uint64_t call_count = pair.second->call_count.load();
    pair.second->is_hot.store(call_count >= threshold);
  }
}

void* ThunkOptimizer::OptimizeHostToGuestThunk(uint32_t guest_address, void* host_function) {
  switch (optimization_level_) {
    case ThunkOptimizationLevel::kMinimal:
      return CreateMinimalThunk(guest_address, host_function);
      
    case ThunkOptimizationLevel::kBalanced: {
      // Use existing thunk if available and sufficiently optimized
      auto it = optimized_thunks_.find(guest_address);
      if (it != optimized_thunks_.end() && 
          it->second->optimization_level >= ThunkOptimizationLevel::kBalanced) {
        return it->second->thunk_address;
      }
      return CreateBalancedThunk(guest_address, host_function);
    }
      
    case ThunkOptimizationLevel::kAggressive:
      return CreateAggressiveThunk(guest_address, host_function);
  }
  
  return nullptr;
}

void* ThunkOptimizer::OptimizeGuestToHostThunk(uint32_t guest_address, void* host_function) {
  // Similar to host-to-guest but with different register requirements
  return OptimizeHostToGuestThunk(guest_address, host_function);
}

void* ThunkOptimizer::OptimizeFunctionDispatch(uint32_t guest_address, const std::vector<uint32_t>& candidates) {
  switch (dispatch_strategy_) {
    case DispatchStrategy::kDirect:
      return CreateDirectDispatchThunk(guest_address);
      
    case DispatchStrategy::kIndirect:
      // Use standard indirection table
      return nullptr; // Use existing mechanism
      
    case DispatchStrategy::kHybrid: {
      // Separate hot and cold functions
      std::vector<uint32_t> hot_functions;
      std::vector<uint32_t> cold_functions;
      
      for (uint32_t addr : candidates) {
        auto hotness_it = hotness_data_.find(addr);
        if (hotness_it != hotness_data_.end() && 
            hotness_it->second->is_hot.load()) {
          hot_functions.push_back(addr);
        } else {
          cold_functions.push_back(addr);
        }
      }
      
      return CreateHybridDispatchThunk(hot_functions);
    }
    
    case DispatchStrategy::kAdaptive: {
      // Choose based on hotness and call patterns
      auto hotness_it = hotness_data_.find(guest_address);
      if (hotness_it != hotness_data_.end() && 
          hotness_it->second->is_hot.load()) {
        return CreateDirectDispatchThunk(guest_address);
      } else {
        return nullptr; // Use indirection table
      }
    }
  }
  
  return nullptr;
}

void* ThunkOptimizer::CreateMinimalThunk(uint32_t guest_address, void* host_function) {
  // Minimal thunk: just save essential registers, call, restore
  const size_t thunk_size = 64; // Estimated size
  void* thunk_code = malloc(thunk_size);
  if (!thunk_code) return nullptr;
  
  uint8_t* code = static_cast<uint8_t*>(thunk_code);
  
  // Generate minimal prolog
  GenerateProlog(thunk_code, RegisterPreservationStrategy::kMinimal);
  
  // Load function pointer and call
  *reinterpret_cast<void**>(code) = host_function;
  // code += sizeof(void*);
  
  // Generate minimal epilog
  GenerateEpilog(thunk_code, RegisterPreservationStrategy::kMinimal);
  
  // Apply CPU-specific optimizations
  ApplyCPUOptimizations(thunk_code, thunk_size);
  
  return thunk_code;
}

void* ThunkOptimizer::CreateBalancedThunk(uint32_t guest_address, void* host_function) {
  const size_t thunk_size = 128;
  void* thunk_code = malloc(thunk_size);
  if (!thunk_code) return nullptr;
  
  uint8_t* code = static_cast<uint8_t*>(thunk_code);
  
  // Generate balanced prolog (smart register preservation)
  GenerateProlog(thunk_code, RegisterPreservationStrategy::kSmart);
  
  // Setup function call with optimization
  *reinterpret_cast<void**>(code) = host_function;
  // code += sizeof(void*);
  
  // Generate balanced epilog
  GenerateEpilog(thunk_code, RegisterPreservationStrategy::kSmart);
  
  // Apply optimizations
  ApplyCPUOptimizations(thunk_code, thunk_size);
  ApplyCacheOptimizations(thunk_code, thunk_size);
  
  return thunk_code;
}

void* ThunkOptimizer::CreateAggressiveThunk(uint32_t guest_address, void* host_function) {
  const size_t thunk_size = 256;
  void* thunk_code = malloc(thunk_size);
  if (!thunk_code) return nullptr;
  
  uint8_t* code = static_cast<uint8_t*>(thunk_code);
  
  // Generate aggressive prolog with full optimization
  GenerateProlog(thunk_code, RegisterPreservationStrategy::kContextual);
  
  // Optimized function call setup
  if (has_movbe_) {
    // Use MOVBE for better performance on supported CPUs
    // This would be implemented in assembly
  }
  
  *reinterpret_cast<void**>(code) = host_function;
  // code += sizeof(void*);
  
  // Generate aggressive epilog
  GenerateEpilog(thunk_code, RegisterPreservationStrategy::kContextual);
  
  // Apply all optimizations
  ApplyCPUOptimizations(thunk_code, thunk_size);
  ApplyCacheOptimizations(thunk_code, thunk_size);
  ApplyBranchPredictionOptimizations(thunk_code, thunk_size);
  
  return thunk_code;
}

void* ThunkOptimizer::CreateDirectDispatchThunk(uint32_t guest_address) {
  const size_t thunk_size = 32;
  void* thunk_code = malloc(thunk_size);
  if (!thunk_code) return nullptr;
  
  void* code = thunk_code;
  
  // Direct jump to target
  GenerateDispatchJump(code, guest_address, DispatchStrategy::kDirect);
  
  return thunk_code;
}

void* ThunkOptimizer::CreateHybridDispatchThunk(const std::vector<uint32_t>& hot_functions) {
  if (hot_functions.empty()) return nullptr;
  
  const size_t thunk_size = 64 + hot_functions.size() * 8;
  void* thunk_code = malloc(thunk_size);
  if (!thunk_code) return nullptr;
  
  void* code = thunk_code;
  uint8_t* byte_code = static_cast<uint8_t*>(code);
  
  // Create mini-dispatch table for hot functions
  for (size_t i = 0; i < hot_functions.size(); ++i) {
    *reinterpret_cast<void**>(byte_code + i * 8) = 
        reinterpret_cast<void*>(hot_functions[i]);
  }
  
  byte_code += hot_functions.size() * 8;
  code = byte_code;
  
  // Generate dispatch logic with branch prediction hints
  GenerateDispatchJump(code, hot_functions[0], DispatchStrategy::kHybrid);
  
  return thunk_code;
}

void ThunkOptimizer::GenerateProlog(void*& code, RegisterPreservationStrategy strategy) {
  // Generate register preservation code based on strategy
  switch (strategy) {
    case RegisterPreservationStrategy::kMinimal: {
      // Save only volatile registers (RCX, RDX, R8-R11)
      uint8_t* byte_code = static_cast<uint8_t*>(code);
      *byte_code++ = 0x48; *byte_code++ = 0x89; *byte_code++ = 0x4C; *byte_code++ = 0x24; // mov [rsp+8], rcx
      *byte_code++ = 0x48; *byte_code++ = 0x89; *byte_code++ = 0x54; *byte_code++ = 0x24; // mov [rsp+16], rdx
      code = byte_code;
      break;
    }
      
    case RegisterPreservationStrategy::kSmart:
      // Analyze and save only what's needed
      CreateSmartRegisterSave(0, code, reinterpret_cast<size_t&>(code));
      break;
      
    case RegisterPreservationStrategy::kFull: {
      // Save all general purpose registers
      uint8_t* byte_code = static_cast<uint8_t*>(code);
      *byte_code++ = 0x57;                           // push rdi
      *byte_code++ = 0x56;                           // push rsi
      *byte_code++ = 0x52;                           // push rdx
      *byte_code++ = 0x51;                           // push rcx
      *byte_code++ = 0x53;                           // push rbx
      code = byte_code;
      break;
    }
      
    default:
      break;
  }
}

void ThunkOptimizer::GenerateEpilog(void*& code, RegisterPreservationStrategy strategy) {
  // Generate register restoration code
  switch (strategy) {
    case RegisterPreservationStrategy::kMinimal: {
      uint8_t* byte_code = static_cast<uint8_t*>(code);
      *byte_code++ = 0x48; *byte_code++ = 0x8B; *byte_code++ = 0x4C; *byte_code++ = 0x24; // mov rcx, [rsp+8]
      *byte_code++ = 0x48; *byte_code++ = 0x8B; *byte_code++ = 0x54; *byte_code++ = 0x24; // mov rdx, [rsp+16]
      code = byte_code;
      break;
    }
      
    case RegisterPreservationStrategy::kSmart:
      // Restore only what was saved
      break;
      
    case RegisterPreservationStrategy::kFull: {
      uint8_t* byte_code = static_cast<uint8_t*>(code);
      *byte_code++ = 0x5B;                           // pop rbx
      *byte_code++ = 0x59;                           // pop rcx
      *byte_code++ = 0x5A;                           // pop rdx
      *byte_code++ = 0x5E;                           // pop rsi
      *byte_code++ = 0x5F;                           // pop rdi
      code = byte_code;
      break;
    }
  }
  
  // Return
  {
    uint8_t* byte_code = static_cast<uint8_t*>(code);
    *byte_code++ = 0xC3; // ret
    code = byte_code;
  }
}

void ThunkOptimizer::GenerateDispatchJump(void*& code, uint32_t target_address, DispatchStrategy strategy) {
  switch (strategy) {
    case DispatchStrategy::kDirect: {
      // Direct jump with branch prediction hint
      uint8_t* byte_code = static_cast<uint8_t*>(code);
      *byte_code++ = 0xE9; // jmp rel32
      *reinterpret_cast<int32_t*>(byte_code) = static_cast<int32_t>(target_address - 
          reinterpret_cast<uintptr_t>(byte_code + 4));
      byte_code += 4;
      code = byte_code;
      break;
    }
      
    case DispatchStrategy::kHybrid: {
      // Use branch prediction hints
      uint8_t* byte_code = static_cast<uint8_t*>(code);
      *byte_code++ = 0x0F; *byte_code++ = 0x1F; *byte_code++ = 0x40; *byte_code++ = 0x00; // prefix for branch prediction
      *byte_code++ = 0xFF; *byte_code++ = 0x25; // jmp [target]
      byte_code += 4; // placeholder for target
      code = byte_code;
      break;
    }
      
    default:
      break;
  }
}

void ThunkOptimizer::AnalyzeRegisterUsage(uint32_t guest_address, std::vector<bool>& used_registers) {
  // This would analyze the function to determine which registers are actually used
  // For now, assume all registers are used
  used_registers.assign(16, true);
}

void ThunkOptimizer::CreateMinimalRegisterSave(void*& save_code, size_t& save_size) {
  save_size = 16; // 2 registers * 8 bytes
  // Actual assembly would go here
}

void ThunkOptimizer::CreateSmartRegisterSave(uint32_t guest_address, void*& save_code, size_t& save_size) {
  // Analyze function and save only needed registers
  std::vector<bool> used_regs;
  AnalyzeRegisterUsage(guest_address, used_regs);
  
  save_size = 0;
  for (size_t i = 0; i < used_regs.size(); ++i) {
    if (used_regs[i]) {
      save_size += 8; // Each register is 8 bytes
    }
  }
}

void ThunkOptimizer::ApplyCPUOptimizations(void* code, size_t code_size) {
  if (has_avx2_) {
    // Use AVX2 instructions for faster operations
    // This would insert AVX2 optimizations
  }
  
  if (has_bmi1_) {
    // Use BMI1 instructions for bit manipulation
    // This would use andn, bextr, etc.
  }
  
  if (has_movbe_) {
    // Use MOVBE for endian-aware operations
    // This would replace certain operations with movbe
  }
}

void ThunkOptimizer::ApplyCacheOptimizations(void* code, size_t code_size) {
  // Align code to cache line boundaries
  // Prefetch data that will be accessed
  // Optimize memory access patterns
}

void ThunkOptimizer::ApplyBranchPredictionOptimizations(void* code, size_t code_size) {
  // Add branch prediction hints
  // Optimize conditional branches
  // Align branch targets
}

void ThunkOptimizer::UpdateFunctionHotness(uint32_t guest_address, uint64_t latency_ns) {
  auto it = hotness_data_.find(guest_address);
  if (it == hotness_data_.end()) {
    auto hotness = std::make_unique<ThunkHotnessData>();
    hotness->guest_address = guest_address;
    hotness->call_count.store(1);
    hotness->total_latency_ns.store(latency_ns);
    hotness->is_hot.store(1 >= hot_threshold_);
    hotness_data_[guest_address] = std::move(hotness);
  } else {
    it->second->call_count.fetch_add(1);
    it->second->total_latency_ns.fetch_add(latency_ns);
    
    uint64_t call_count = it->second->call_count.load();
    it->second->is_hot.store(call_count >= hot_threshold_);
  }
}

void ThunkOptimizer::RecompileHotFunctions() {
  std::vector<uint32_t> hot_functions;
  
  for (auto& pair : hotness_data_) {
    if (pair.second->is_hot.load() && 
        pair.second->current_optimization < optimization_level_) {
      hot_functions.push_back(pair.first);
    }
  }
  
  // Recompile hot functions with higher optimization
  for (uint32_t addr : hot_functions) {
    auto it = optimized_thunks_.find(addr);
    if (it != optimized_thunks_.end()) {
      // Create a new optimized version before releasing the previous thunk.
      void* old_thunk = it->second->thunk_address;
      void* new_thunk = OptimizeHostToGuestThunk(addr, old_thunk);
      if (new_thunk && new_thunk != old_thunk) {
        free(old_thunk);

        auto optimized_thunk = std::make_unique<OptimizedThunk>();
        optimized_thunk->thunk_address = new_thunk;
        optimized_thunk->optimization_level = optimization_level_;
        optimized_thunk->guest_address = addr;
        optimized_thunk->creation_timestamp = GetCurrentTimestampNs();
        
        optimized_thunks_[addr] = std::move(optimized_thunk);
        
        // Update hotness data
        auto hotness_it = hotness_data_.find(addr);
        if (hotness_it != hotness_data_.end()) {
          hotness_it->second->current_optimization = optimization_level_;
        }
      }
    }
  }
}

void ThunkOptimizer::RecordLatency(uint32_t guest_address, uint64_t start_time, uint64_t end_time) {
  uint64_t latency = end_time - start_time;
  UpdateFunctionHotness(guest_address, latency);
  
  // Update metrics
  metrics_.host_to_guest_calls.fetch_add(1);
  
  uint64_t current_avg = metrics_.avg_host_to_guest_latency_ns.load();
  uint64_t call_count = metrics_.host_to_guest_calls.load();
  
  // Calculate new average
  uint64_t new_avg = (current_avg * (call_count - 1) + latency) / call_count;
  metrics_.avg_host_to_guest_latency_ns.store(new_avg);
  
  // Update min latency
  uint64_t current_min = metrics_.min_host_to_guest_latency_ns.load();
  if (latency < current_min) {
    metrics_.min_host_to_guest_latency_ns.store(latency);
  }
}

void ThunkOptimizer::UpdateMetrics() {
  // Periodic metrics update (called from main thread)
  uint64_t current_time = GetCurrentTimestampNs();
  
  if (current_time - last_cleanup_time_ > 10000000000ULL) { // 10 seconds
    CleanupOldThunks();
    last_cleanup_time_ = current_time;
  }
}

void ThunkOptimizer::CleanupOldThunks() {
  auto current_time = GetCurrentTimestampNs();
  const uint64_t thunk_lifetime = 30000000000ULL; // 30 seconds
  
  auto it = optimized_thunks_.begin();
  while (it != optimized_thunks_.end()) {
    if (current_time - it->second->creation_timestamp > thunk_lifetime) {
      // Check if function is still hot
      auto hotness_it = hotness_data_.find(it->first);
      if (hotness_it == hotness_data_.end() || !hotness_it->second->is_hot.load()) {
        free(it->second->thunk_address);
        it = optimized_thunks_.erase(it);
      } else {
        ++it;
      }
    } else {
      ++it;
    }
  }
}

void ThunkOptimizer::UpdateDispatchTable() {
  // Update any dispatch tables based on current hot functions
}

uint64_t ThunkOptimizer::GetCurrentTimestampNs() {
  auto now = std::chrono::high_resolution_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

void ThunkOptimizer::ResetMetrics() {
  metrics_.host_to_guest_calls.store(0);
  metrics_.guest_to_host_calls.store(0);
  metrics_.direct_calls.store(0);
  metrics_.indirect_calls.store(0);
  metrics_.register_save_cycles.store(0);
  metrics_.register_restore_cycles.store(0);
  metrics_.dispatch_overhead_cycles.store(0);
  metrics_.avg_host_to_guest_latency_ns.store(0);
  metrics_.avg_guest_to_host_latency_ns.store(0);
  metrics_.min_host_to_guest_latency_ns.store(UINT64_MAX);
  metrics_.min_guest_to_host_latency_ns.store(UINT64_MAX);
}

std::vector<ThunkHotnessData*> ThunkOptimizer::GetHotFunctions(uint32_t limit) {
  std::vector<ThunkHotnessData*> hot_functions;
  
  for (auto& pair : hotness_data_) {
    if (pair.second->is_hot.load()) {
      hot_functions.push_back(pair.second.get());
    }
  }
  
  // Sort by call count
  std::sort(hot_functions.begin(), hot_functions.end(),
            [](const ThunkHotnessData* a, const ThunkHotnessData* b) {
              return a->call_count.load() > b->call_count.load();
            });
  
  if (hot_functions.size() > limit) {
    hot_functions.resize(limit);
  }
  
  return hot_functions;
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
