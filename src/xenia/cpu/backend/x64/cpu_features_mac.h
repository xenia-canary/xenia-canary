/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_X64_CPU_FEATURES_H_
#define XENIA_CPU_BACKEND_X64_CPU_FEATURES_H_

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

// CPU feature flags with bit positions for efficient masking
enum class CPUFeature : uint64_t {
  // Basic x86_64 features
  kSSE2      = 1ULL << 0,
  kSSE3      = 1ULL << 1,
  kSSSE3     = 1ULL << 2,
  kSSE4_1    = 1ULL << 3,
  kSSE4_2    = 1ULL << 4,
  kPOPCNT     = 1ULL << 5,
  
  // Advanced instruction sets
  kAVX        = 1ULL << 6,
  kAVX2       = 1ULL << 7,
  kFMA        = 1ULL << 8,
  kFMA4       = 1ULL << 9,
  
  // BMI instructions
  kBMI1       = 1ULL << 10,
  kBMI2       = 1ULL << 11,
  kTBM        = 1ULL << 12,
  kLZCNT      = 1ULL << 13,
  
  // Special instructions
  kMOVBE      = 1ULL << 14,
  kADX        = 1ULL << 15,
  kAES        = 1ULL << 16,
  kSHA        = 1ULL << 17,
  kGFNI       = 1ULL << 18,
  kVAES       = 1ULL << 19,
  kVPCLMULQDQ = 1ULL << 20,
  
  // AVX-512 features
  kAVX512F    = 1ULL << 21,
  kAVX512VL   = 1ULL << 22,
  kAVX512BW   = 1ULL << 23,
  kAVX512DQ   = 1ULL << 24,
  kAVX512IFMA = 1ULL << 25,
  kAVX512VBMI = 1ULL << 26,
  
  // Performance features
  kFastRepMovs = 1ULL << 27,
  kFastJrcx   = 1ULL << 28,
  kFastLoop    = 1ULL << 29,
  kIndependentVars = 1ULL << 30,
  
  // Platform-specific
  kMovdir64B  = 1ULL << 31,
  kMovdir64M  = 1ULL << 32,
  kCLFLUSHOPT = 1ULL << 33,
  kCLWB       = 1ULL << 34,
  kPrefetchW  = 1ULL << 35,
};

// CPU feature sets for convenience
constexpr uint64_t kBaselineFeatures = 
    static_cast<uint64_t>(CPUFeature::kSSE2) |
    static_cast<uint64_t>(CPUFeature::kSSE3) |
    static_cast<uint64_t>(CPUFeature::kSSSE3);

constexpr uint64_t kModernFeatures =
    static_cast<uint64_t>(CPUFeature::kAVX2) |
    static_cast<uint64_t>(CPUFeature::kBMI1) |
    static_cast<uint64_t>(CPUFeature::kBMI2) |
    static_cast<uint64_t>(CPUFeature::kLZCNT) |
    static_cast<uint64_t>(CPUFeature::kMOVBE) |
    static_cast<uint64_t>(CPUFeature::kFMA);

constexpr uint64_t kAdvancedFeatures =
    static_cast<uint64_t>(CPUFeature::kAVX512F) |
    static_cast<uint64_t>(CPUFeature::kAVX512VL) |
    static_cast<uint64_t>(CPUFeature::kAVX512BW) |
    static_cast<uint64_t>(CPUFeature::kAVX512DQ);

struct CPUInfo {
  std::string vendor;
  std::string brand_string;
  uint32_t family;
  uint32_t model;
  uint32_t stepping;
  uint8_t cores;
  uint8_t threads_per_core;
  uint32_t total_threads;
  
  struct {
    uint32_t l1d_size;
    uint32_t l1i_size;
    uint32_t l2_size;
    uint32_t l3_size;
    uint32_t l1d_line_size;
    uint32_t l2_line_size;
    uint32_t l3_line_size;
  } cache;
  
  uint64_t supported_features;
  uint64_t enabled_features;
  
  struct {
    uint32_t base_frequency_mhz;
    uint32_t max_frequency_mhz;
    bool turbo_boost;
    uint32_t tsc_frequency_mhz;
    bool invariant_tsc;
  } performance;
  
  struct {
    bool is_apple_silicon;
    bool is_rosetta2;
    bool supports_mmap_jit;
    bool has_unaligned_access_penalty;
    uint32_t optimal_memcpy_size;
    uint32_t optimal_memset_size;
  } platform;
};

class CPUFeatureDetector {
 public:
  static const CPUInfo& GetCPUInfo();
  static bool HasFeature(CPUFeature feature);
  static uint64_t GetSupportedFeatures();
  static uint64_t GetEnabledFeatures();
  static void EnableFeature(CPUFeature feature);
  static void DisableFeature(CPUFeature feature);
  static void SetFeatureMask(uint64_t mask);
  static void OptimizeForCPU(const CPUInfo& cpu_info);
  
  // Optimization recommendations
  static std::vector<std::string> GetOptimizationRecommendations();
  static bool ShouldUseAVX512();
  static bool ShouldUseBMIInstructions();
  static bool ShouldUseMOVBE();
  static void InitializeCPUInfo();
   
 private:
  static void DetectBasicInfo();
  static void DetectCacheInfo();
  static void DetectFeatures();
  static void DetectPerformanceInfo();
  static void DetectPlatformInfo();
  
  static uint32_t GetCPUID(uint32_t leaf, uint32_t subleaf = 0);
  static void GetCPUID(uint32_t leaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx, uint32_t subleaf = 0);
  static void GetCPUIDString(uint32_t leaf, char* buffer, size_t buffer_size);
  
  static void DetectAppleSiliconFeatures();
  static void DetectRosetta2Features();
  
  static CPUInfo cpu_info_;
  static std::atomic<bool> initialized_;
  static std::atomic<int> init_call_count_;

 private:
  static void IncrementInitCallCount();
};

inline bool HasAVX2() { return CPUFeatureDetector::HasFeature(CPUFeature::kAVX2); }
inline bool HasBMI1() { return CPUFeatureDetector::HasFeature(CPUFeature::kBMI1); }
inline bool HasBMI2() { return CPUFeatureDetector::HasFeature(CPUFeature::kBMI2); }
inline bool HasLZCNT() { return CPUFeatureDetector::HasFeature(CPUFeature::kLZCNT); }
inline bool HasMOVBE() { return CPUFeatureDetector::HasFeature(CPUFeature::kMOVBE); }
inline bool HasAVX512() { 
  return CPUFeatureDetector::HasFeature(CPUFeature::kAVX512F) &&
         CPUFeatureDetector::HasFeature(CPUFeature::kAVX512VL); 
}
inline bool HasFMA() { return CPUFeatureDetector::HasFeature(CPUFeature::kFMA); }

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_X64_CPU_FEATURES_H_