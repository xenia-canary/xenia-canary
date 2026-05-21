/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/cpu_features_mac.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#include <atomic>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#endif

#ifdef _WIN32
#include <intrin.h>
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

CPUInfo CPUFeatureDetector::cpu_info_ = {};
std::atomic<bool> CPUFeatureDetector::initialized_(false);
std::atomic<int> CPUFeatureDetector::init_call_count_(0);

void CPUFeatureDetector::IncrementInitCallCount() {
  int count = init_call_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  // Only log periodically to avoid spam
  if (count <= 5 || (count % 100 == 0)) {
    XELOGI("[CPUFeatureDetector] InitializeCPUInfo() active calls: {}", count);
  }
}

const CPUInfo& CPUFeatureDetector::GetCPUInfo() {
  bool is_init = initialized_.load(std::memory_order_acquire);
  XELOGI("[CPUFeatureDetector] GetCPUInfo() called, initialized_=%d", (int)is_init);
  fflush(stderr);
  if (!is_init) {
    InitializeCPUInfo();
  }
  return cpu_info_;
}

bool CPUFeatureDetector::HasFeature(CPUFeature feature) {
  if (!initialized_.load(std::memory_order_acquire)) {
    InitializeCPUInfo();
  }
  return (cpu_info_.supported_features & static_cast<uint64_t>(feature)) != 0;
}

uint64_t CPUFeatureDetector::GetSupportedFeatures() {
  if (!initialized_.load(std::memory_order_acquire)) {
    InitializeCPUInfo();
  }
  return cpu_info_.supported_features;
}

uint64_t CPUFeatureDetector::GetEnabledFeatures() {
  if (!initialized_.load(std::memory_order_acquire)) {
    InitializeCPUInfo();
  }
  return cpu_info_.enabled_features;
}

void CPUFeatureDetector::EnableFeature(CPUFeature feature) {
  if (!initialized_.load(std::memory_order_acquire)) {
    InitializeCPUInfo();
  }
  uint64_t feature_bit = static_cast<uint64_t>(feature);
  if (cpu_info_.supported_features & feature_bit) {
    if (!(cpu_info_.enabled_features & feature_bit)) {
      XELOGI("[CPUFeatureDetector] Enabling feature bit 0x{:016X}", feature_bit);
      cpu_info_.enabled_features |= feature_bit;
    }
  } else {
    XELOGW("[CPUFeatureDetector] precise EnableFeature requested for unsupported feature 0x{:016X}", feature_bit);
  }
}

void CPUFeatureDetector::DisableFeature(CPUFeature feature) {
  if (!initialized_.load(std::memory_order_acquire)) {
    InitializeCPUInfo();
  }
  uint64_t feature_bit = static_cast<uint64_t>(feature);
  if (cpu_info_.enabled_features & feature_bit) {
    XELOGI("[CPUFeatureDetector] Disabling feature bit 0x{:016X}", feature_bit);
    cpu_info_.enabled_features &= ~feature_bit;
  }
}

void CPUFeatureDetector::SetFeatureMask(uint64_t mask) {
  if (!initialized_.load(std::memory_order_acquire)) {
    InitializeCPUInfo();
  }
  // We ensure that nothing that isn't supported isn't used
  cpu_info_.enabled_features = cpu_info_.supported_features & mask;
}

void CPUFeatureDetector::OptimizeForCPU(const CPUInfo& cpu_info) {
  if (cpu_info.platform.is_apple_silicon) {
    cpu_info_.platform.optimal_memcpy_size = 64;
    cpu_info_.platform.optimal_memset_size = 64;
    
    if (cpu_info.platform.is_rosetta2) {
      DisableFeature(CPUFeature::kAVX512F);
      DisableFeature(CPUFeature::kAVX512VL);
    }
  }
  
  if (cpu_info.vendor == "GenuineIntel") {
    if (cpu_info.family >= 6 && cpu_info.model >= 0x4E) {
      // Skylake and later have improved REP MOVSB
      EnableFeature(CPUFeature::kFastRepMovs);
    }
    
    if (HasFeature(CPUFeature::kMOVBE)) {
      EnableFeature(CPUFeature::kMOVBE);
    }
  }
  
  if (cpu_info.vendor == "AuthenticAMD") {
    if (cpu_info.family >= 0x17) {
      EnableFeature(CPUFeature::kFastJrcx);
      EnableFeature(CPUFeature::kFastLoop);
      EnableFeature(CPUFeature::kIndependentVars);
    }
    
    // Enable AMD-specific features
    if (HasFeature(CPUFeature::kLZCNT)) {
      EnableFeature(CPUFeature::kLZCNT);
    }
  }
  
  if (HasFeature(CPUFeature::kAVX512F) && HasFeature(CPUFeature::kAVX512VL)) {
    // Only enable AVX-512 if our hardware supports it
    if (cpu_info.total_threads >= 8 && !cpu_info.platform.is_apple_silicon) {
      EnableFeature(CPUFeature::kAVX512F);
      EnableFeature(CPUFeature::kAVX512VL);
      EnableFeature(CPUFeature::kAVX512BW);
      EnableFeature(CPUFeature::kAVX512DQ);
    }
  }
}

std::vector<std::string> CPUFeatureDetector::GetOptimizationRecommendations() {
  std::vector<std::string> recommendations;
  
  if (!initialized_) {
    InitializeCPUInfo();
  }
  
  if (!HasFeature(CPUFeature::kAVX2)) {
    recommendations.push_back("Consider upgrading to a CPU with AVX2 support for better performance");
  }
  
  if (!HasFeature(CPUFeature::kBMI2) && !HasFeature(CPUFeature::kBMI1)) {
    recommendations.push_back("BMI instructions can improve bit manipulation performance");
  }
  
  if (!HasFeature(CPUFeature::kLZCNT)) {
    recommendations.push_back("LZCNT can improve bit counting performance");
  }
  
  // Platform-specific recommendations
  if (cpu_info_.platform.is_apple_silicon) {
    recommendations.push_back("Apple Silicon detected: optimizing for x86->ARM64 translation");
  }
  
  if (cpu_info_.platform.is_rosetta2) {
    recommendations.push_back("Running under Rosetta 2: AVX-512 disabled for compatibility");
  }
  
  return recommendations;
}

bool CPUFeatureDetector::ShouldUseAVX512() {
  return HasFeature(CPUFeature::kAVX512F) && 
         HasFeature(CPUFeature::kAVX512VL) &&
         cpu_info_.total_threads >= 8;
}

bool CPUFeatureDetector::ShouldUseBMIInstructions() {
  return HasFeature(CPUFeature::kBMI1) || HasFeature(CPUFeature::kBMI2);
}

bool CPUFeatureDetector::ShouldUseMOVBE() {
  return HasFeature(CPUFeature::kMOVBE) && 
         (cpu_info_.vendor == "GenuineIntel" || cpu_info_.vendor == "AuthenticAMD");
}

void CPUFeatureDetector::InitializeCPUInfo() {
  static thread_local bool is_initializing = false;
  
  if (is_initializing) {
    return;
  }
  
  if (initialized_.load(std::memory_order_acquire)) {
    return;
  }
  
  is_initializing = true;
  
  IncrementInitCallCount();  // Debug: track call count
  
  XELOGI("[CPUFeatureDetector] InitializeCPUInfo() starting detection sequence");
  
  XELOGI("[CPUFeatureDetector] Step 1/5: DetectBasicInfo()");
  DetectBasicInfo();
  
  XELOGI("[CPUFeatureDetector] Step 2/5: DetectCacheInfo()");
  DetectCacheInfo();
  
  XELOGI("[CPUFeatureDetector] Step 3/5: DetectFeatures()");
  DetectFeatures();
  
  XELOGI("[CPUFeatureDetector] Step 4/5: DetectPerformanceInfo()");
  DetectPerformanceInfo();
  
  XELOGI("[CPUFeatureDetector] Step 5/5: DetectPlatformInfo()");
  DetectPlatformInfo();
  
  // Anything supported is the only thing set
  cpu_info_.enabled_features = cpu_info_.supported_features;
  
  XELOGI("[CPUFeatureDetector] Applying CPU-specific optimizations...");
  OptimizeForCPU(cpu_info_);
  
  initialized_.store(true, std::memory_order_release);
  is_initializing = false;
  
  XELOGI("[CPUFeatureDetector] Initialization complete, features finalized");
  
  XELOGI("CPU: {} {} (Family: {}, Model: {})", 
           cpu_info_.vendor, cpu_info_.brand_string, 
           cpu_info_.family, cpu_info_.model);
  
  XELOGI("CPU Features: 0x{:016X} (Enabled: 0x{:016X})", 
           cpu_info_.supported_features, cpu_info_.enabled_features);
}

void CPUFeatureDetector::DetectBasicInfo() {
  XELOGI("[CPUFeatureDetector] DetectBasicInfo: Getting vendor ID (CPUID leaf 0)");
  fflush(stderr);
  // Get vendor ID
  char vendor_buffer[13] = {0};
  GetCPUIDString(0, vendor_buffer, 12);
  XELOGI("[CPUFeatureDetector] DetectBasicInfo: Vendor=%s", vendor_buffer);
  fflush(stderr);
  cpu_info_.vendor = std::string(vendor_buffer);
  
  char brand_buffer[49] = {0};
  for (int i = 0; i < 3; ++i) {
    XELOGI("[CPUFeatureDetector] DetectBasicInfo: Getting brand string (CPUID leaf 0x%X)", 0x80000002 + i);
    fflush(stderr);
    GetCPUIDString(0x80000002 + i, brand_buffer + i * 16, 16);
  }
  XELOGI("[CPUFeatureDetector] DetectBasicInfo: Brand=%s", brand_buffer);
  fflush(stderr);
  cpu_info_.brand_string = std::string(brand_buffer);
  
  XELOGI("[CPUFeatureDetector] DetectBasicInfo: Getting family/model/stepping (CPUID leaf 1)");
  fflush(stderr);
  uint32_t eax, ebx, ecx, edx;
  GetCPUID(1, eax, ebx, ecx, edx);
  cpu_info_.family = (eax >> 8) & 0xF;
  cpu_info_.model = (eax >> 4) & 0xF;
  cpu_info_.stepping = eax & 0xF;

  if (cpu_info_.family == 0xF) {
    cpu_info_.family += (eax >> 20) & 0xFF;
  }
  if (cpu_info_.family == 0xF || cpu_info_.family == 0x6) {
    cpu_info_.model += ((eax >> 16) & 0xF) << 4;
  }
}

void CPUFeatureDetector::DetectCacheInfo() {
  uint32_t eax, ebx, ecx, edx;
  
  XELOGI("[CPUFeatureDetector] DetectCacheInfo: Getting L1 cache info (CPUID leaf 4)");
  fflush(stderr);
  GetCPUID(4, eax, ebx, ecx, edx);
  cpu_info_.cache.l1d_size = ((ebx >> 0) & 0xFFF) * ((ebx >> 12) & 0x3FF) + 1;
  cpu_info_.cache.l1i_size = ((edx >> 0) & 0xFFF) * ((edx >> 12) & 0x3FF) + 1;
  cpu_info_.cache.l1d_line_size = ((ebx >> 0) & 0xFFF) ? ((ebx >> 0) & 0xFFF) : 64;
  cpu_info_.cache.l2_line_size = ((ebx >> 0) & 0xFFF) ? ((ebx >> 0) & 0xFFF) : 64;
  cpu_info_.cache.l3_line_size = ((ebx >> 0) & 0xFFF) ? ((ebx >> 0) & 0xFFF) : 64;
  
  // L2 and L3 cache
  if (ecx != 0) {
    cpu_info_.cache.l2_size = ((ecx >> 0) & 0xFFF) * ((ecx >> 12) & 0x3FF) + 1;
  }
  
  XELOGI("[CPUFeatureDetector] DetectCacheInfo: Getting L3 cache info (CPUID leaf 0x80000006)");
  fflush(stderr);
  GetCPUID(0x80000006, eax, ebx, ecx, edx);
  if (ecx != 0) {
    cpu_info_.cache.l3_size = ((ecx >> 18) & 0x3FFF) * 512 * 1024; // Size in KB
  }
}

void CPUFeatureDetector::DetectFeatures() {
  uint32_t eax, ebx, ecx, edx;
  
  // CPUID 1 feature detection
  XELOGI("[CPUFeatureDetector] DetectFeatures: Getting basic features (CPUID leaf 1)");
  fflush(stderr);
  GetCPUID(1, eax, ebx, ecx, edx);
  
  if (edx & (1 << 26)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kSSE2);
  if (ecx & (1 << 0))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kSSE3);
  if (ecx & (1 << 9))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kSSSE3);
  if (ecx & (1 << 19)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kSSE4_1);
  if (ecx & (1 << 20)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kSSE4_2);
  if (ecx & (1 << 23)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kPOPCNT);
  
  // CPUID 7 feature detection
  XELOGI("[CPUFeatureDetector] DetectFeatures: Getting extended features (CPUID leaf 7)");
  fflush(stderr);
  GetCPUID(7, eax, ebx, ecx, edx);
  
  if (ebx & (1 << 5))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAVX2);
  if (ecx & (1 << 1))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAVX512F);
  if (ecx & (1 << 2))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAVX512VL);
  if (ecx & (1 << 3))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAVX512BW);
  if (ecx & (1 << 4))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAVX512DQ);
  
  // BMI features
  if (ebx & (1 << 0))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kBMI1);
  if (ebx & (1 << 8))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kBMI2);
  if (ebx & (1 << 9))  cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kLZCNT);
  
  // Other features
  if (ebx & (1 << 22)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kMOVBE);
  if (ebx & (1 << 19)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kADX);
  if (ebx & (1 << 25)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAES);
  
  // AVX detection (CPUID 1, ECX bit 28)
  if (ecx & (1 << 28)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kAVX);
  
  // FMA detection (CPUID 1, ECX bit 12)
  if (ecx & (1 << 12)) cpu_info_.supported_features |= static_cast<uint64_t>(CPUFeature::kFMA);
}

void CPUFeatureDetector::DetectPerformanceInfo() {
#ifdef __APPLE__
  // Call on sysctl to poll cpu performance data on macOS
  size_t len = sizeof(cpu_info_.performance.base_frequency_mhz);
  if (sysctlbyname("hw.cpufrequency", &cpu_info_.performance.base_frequency_mhz, &len, nullptr, 0) == 0) {
    cpu_info_.performance.base_frequency_mhz /= 1000000; // Convert Hz to MHz
    cpu_info_.performance.max_frequency_mhz = cpu_info_.performance.base_frequency_mhz;
  }
  
  // Check for invariant TSC
  uint64_t tsc_freq = 0;
  len = sizeof(tsc_freq);
  if (sysctlbyname("hw.tsc.frequency", &tsc_freq, &len, nullptr, 0) == 0) {
    cpu_info_.performance.tsc_frequency_mhz = static_cast<uint32_t>(tsc_freq / 1000000);
    cpu_info_.performance.invariant_tsc = true;
  }
#else
  // We really aren't using this under Windows/Linux given this file only targets macOS
  cpu_info_.performance.base_frequency_mhz = 0;
  cpu_info_.performance.max_frequency_mhz = 0;
  cpu_info_.performance.invariant_tsc = false;
#endif
}

void CPUFeatureDetector::DetectPlatformInfo() {
  cpu_info_.platform.is_apple_silicon = false;
  cpu_info_.platform.is_rosetta2 = false;
  cpu_info_.platform.supports_mmap_jit = true;
  cpu_info_.platform.has_unaligned_access_penalty = false;
  cpu_info_.platform.optimal_memcpy_size = 32;
  cpu_info_.platform.optimal_memset_size = 32;
  
#ifdef __APPLE__
  DetectAppleSiliconFeatures();
#endif
}

void CPUFeatureDetector::DetectAppleSiliconFeatures() {
#ifdef __APPLE__
  size_t len;
  char buffer[256];
  
  len = sizeof(buffer);
  if (sysctlbyname("hw.machine", buffer, &len, nullptr, 0) == 0) {
    if (strncmp(buffer, "arm64", 5) == 0) {
      cpu_info_.platform.is_apple_silicon = true;
      // JIT is complex so if anything goes fucky wucky, it's this shit here
      cpu_info_.platform.supports_mmap_jit = false;
      cpu_info_.platform.has_unaligned_access_penalty = true;
      cpu_info_.platform.optimal_memcpy_size = 64;
      cpu_info_.platform.optimal_memset_size = 64;
      
      static bool apple_silicon_logged = false;
      if (!apple_silicon_logged) {
        XELOGI("CPU: Apple Silicon detected - applying platform-specific optimizations");
        apple_silicon_logged = true;
      }
    }
  }
  
  // Check for Rosetta 2
  len = sizeof(buffer);
  if (sysctlbyname("sysctl.proc_translated", buffer, &len, nullptr, 0) == 0) {
    int translated = *(int*)buffer;
    if (translated) {
      cpu_info_.platform.is_rosetta2 = true;
      static bool rosetta_logged = false;
      if (!rosetta_logged) {
        XELOGI("CPU: Rosetta 2 translation detected - some optimizations disabled");
        rosetta_logged = true;
      }
    }
  }
  
  len = sizeof(cpu_info_.cores);
  if (sysctlbyname("hw.physicalcpu", &cpu_info_.cores, &len, nullptr, 0) == 0) {
    len = sizeof(cpu_info_.total_threads);
    sysctlbyname("hw.logicalcpu", &cpu_info_.total_threads, &len, nullptr, 0);
    
    if (cpu_info_.cores > 0) {
      cpu_info_.threads_per_core = cpu_info_.total_threads / cpu_info_.cores;
    }
  }
#endif
}

uint32_t CPUFeatureDetector::GetCPUID(uint32_t leaf, uint32_t subleaf) {
#ifdef _WIN32
  uint32_t eax, ebx, ecx, edx;
  __cpuidex(eax, ebx, ecx, edx, leaf, subleaf);
  return eax; // Return EAX for now
#else
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                  : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                  : "a"(leaf), "c"(subleaf));
  return eax;
#endif
}

void CPUFeatureDetector::GetCPUID(uint32_t leaf, uint32_t& eax, uint32_t& ebx, uint32_t& ecx, uint32_t& edx, uint32_t subleaf) {
#ifdef _WIN32
  __cpuidex(reinterpret_cast<int*>(&eax), reinterpret_cast<int*>(&ebx),
            reinterpret_cast<int*>(&ecx), reinterpret_cast<int*>(&edx), leaf,
            subleaf);
#else
  __asm__ volatile("cpuid"
                  : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                  : "a"(leaf), "c"(subleaf));
#endif
}

void CPUFeatureDetector::GetCPUIDString(uint32_t leaf, char* buffer, size_t buffer_size) {
#ifdef _WIN32
  int cpu_info[4];
  __cpuid(cpu_info, leaf);
  
  // CPUID returns string in EBX, EDX, ECX order
  memcpy(buffer, &cpu_info[1], 4); // EBX
  memcpy(buffer + 4, &cpu_info[3], 4); // EDX
  memcpy(buffer + 8, &cpu_info[2], 4); // ECX
  buffer[12] = '\0';
#else
  uint32_t eax, ebx, ecx, edx;
  __asm__ volatile("cpuid"
                  : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                  : "a"(leaf));
  
  memcpy(buffer, &ebx, 4);
  memcpy(buffer + 4, &edx, 4);
  memcpy(buffer + 8, &ecx, 4);
  buffer[12] = '\0';
#endif
}

}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
