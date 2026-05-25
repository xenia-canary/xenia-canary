/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/cpu_features_mac.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#if XE_PLATFORM_MACOS
#include "xenia/base/xsimd_avx_support_mac.h"
#else
#include "xenia/base/xsimd_avx_support.h"
#endif

#if XE_ARCH_AMD64
#if XE_COMPILER_MSVC
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if XE_PLATFORM_MAC
#include <sys/sysctl.h>
#endif

namespace xe {

namespace {

const char* YesNo(bool value) { return value ? "YES" : "NO"; }

const char* AvailableMissing(bool value) {
  return value ? "available" : "missing";
}

#if XE_ARCH_AMD64

void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* regs) {
#if XE_COMPILER_MSVC
  int cpu_info[4] = {};
  __cpuidex(cpu_info, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (size_t i = 0; i < 4; ++i) {
    regs[i] = static_cast<uint32_t>(cpu_info[i]);
  }
#else
  __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
}

void cpuid(uint32_t leaf, uint32_t* regs) {
  cpuid(leaf, 0, regs);
}

#endif  // XE_ARCH_AMD64

}  // namespace

CpuFeatures& CpuFeatures::Get() {
  static CpuFeatures instance;
  return instance;
}

CpuFeatures::CpuFeatures() {
  DetectFeatures();
  DetectRosetta();
  DetectLibrarySupport();
}

void CpuFeatures::DetectFeatures() {
#if XE_ARCH_AMD64
  uint32_t regs[4];
  
  // Get CPU brand string
  cpuid(0x80000000, regs);
  uint32_t max_extended = regs[0];
  if (max_extended >= 0x80000004) {
    uint32_t brand_regs[12] = {};
    cpuid(0x80000002, &brand_regs[0]);
    cpuid(0x80000003, &brand_regs[4]);
    cpuid(0x80000004, &brand_regs[8]);
    char brand[49] = {};
    std::memcpy(brand, brand_regs, sizeof(brand_regs));
    brand[48] = '\0';
    
    // Trim leading spaces
    const char* start = brand;
    while (*start == ' ') ++start;
    size_t brand_length = 0;
    while (brand_length < sizeof(cpu_brand_) - 1 &&
           start[brand_length] != '\0') {
      ++brand_length;
    }
    std::memcpy(cpu_brand_, start, brand_length);
    cpu_brand_[brand_length] = '\0';
  }
  
  // CPUID function 1: Feature Information
  cpuid(1, regs);
  uint32_t ecx_1 = regs[2];
  uint32_t edx_1 = regs[3];
  
  features_.sse2 = (edx_1 & (UINT32_C(1) << 26)) != 0;
  features_.sse3 = (ecx_1 & (UINT32_C(1) << 0)) != 0;
  features_.ssse3 = (ecx_1 & (UINT32_C(1) << 9)) != 0;
  features_.sse41 = (ecx_1 & (UINT32_C(1) << 19)) != 0;
  features_.sse42 = (ecx_1 & (UINT32_C(1) << 20)) != 0;
  features_.pclmulqdq = (ecx_1 & (UINT32_C(1) << 1)) != 0;
  features_.fma = (ecx_1 & (UINT32_C(1) << 12)) != 0;
  features_.f16c = (ecx_1 & (UINT32_C(1) << 29)) != 0;
  
  // Check OSXSAVE (bit 27) - OS supports XSAVE/XRSTOR
  bool osxsave = (ecx_1 & (UINT32_C(1) << 27)) != 0;
  
  // AVX requires OSXSAVE and AVX bit (bit 28)
  if (osxsave && (ecx_1 & (UINT32_C(1) << 28))) {
#if XE_COMPILER_MSVC
    uint64_t xcr0 = _xgetbv(0);
#elif defined(__GNUC__) || defined(__clang__)
    uint32_t xcr0_low, xcr0_high;
    __asm__ volatile("xgetbv" : "=a"(xcr0_low), "=d"(xcr0_high) : "c"(0));
    uint64_t xcr0 = (static_cast<uint64_t>(xcr0_high) << 32) | xcr0_low;
#endif

#if XE_COMPILER_MSVC || defined(__GNUC__) || defined(__clang__)
    // AVX: bits 1 and 2 must be set (XMM and YMM)
    constexpr uint64_t kXmmYmmMask = 0x6;
    bool xmm_ymm_enabled = (xcr0 & kXmmYmmMask) == kXmmYmmMask;
    features_.avx = xmm_ymm_enabled;
    
    // AVX-512: bits 5, 6, 7 must be set (opmask, ZMM_Hi256, Hi16_ZMM)
    constexpr uint64_t kZmmMask = 0xE0;
    bool zmm_enabled = (xcr0 & kZmmMask) == kZmmMask;
    
    if (features_.avx) {
      // CPUID function 7: Extended Features
      cpuid(7, 0, regs);
      uint32_t ebx_7 = regs[1];
      
      features_.avx2 = (ebx_7 & (UINT32_C(1) << 5)) != 0;
      features_.bmi1 = (ebx_7 & (UINT32_C(1) << 3)) != 0;
      features_.bmi2 = (ebx_7 & (UINT32_C(1) << 8)) != 0;
      
      if (zmm_enabled) {
        features_.avx512f = (ebx_7 & (UINT32_C(1) << 16)) != 0;
        features_.avx512dq = (ebx_7 & (UINT32_C(1) << 17)) != 0;
        features_.avx512bw = (ebx_7 & (UINT32_C(1) << 30)) != 0;
        features_.avx512vl = (ebx_7 & (UINT32_C(1) << 31)) != 0;
      }
    }
#endif
  }
#elif XE_ARCH_ARM64
  // ARM detection would go here if needed
  features_.sse2 = false;  // ARM doesn't have SSE
#endif
}

void CpuFeatures::DetectRosetta() {
#if XE_PLATFORM_MAC
  // Check if running under Rosetta 2 translation
  int ret = 0;
  size_t size = sizeof(ret);
  if (sysctlbyname("sysctl.proc_translated", &ret, &size, nullptr, 0) == 0) {
    features_.rosetta2 = (ret == 1);
  }
  
  if (features_.rosetta2) {
    bool avx_ok = xe::xsimd_support::IsAVXSafeToExecute();
    bool avx2_ok = xe::xsimd_support::IsAVX2SafeToExecute();
    if (!avx_ok) {
      features_.avx = false;
      features_.avx2 = false;
      features_.avx512f = false;
      features_.avx512dq = false;
      features_.avx512bw = false;
      features_.avx512vl = false;
      features_.fma = false;  // FMA uses VEX encoding
    } else if (!avx2_ok) {
      features_.avx2 = false;
      features_.avx512f = false;
      features_.avx512dq = false;
      features_.avx512bw = false;
      features_.avx512vl = false;
    }
  }
#endif
}

void CpuFeatures::LogFeatures() const {
  XELOGI("================ CPU Feature Detection ================");
  
  if (cpu_brand_[0] != '\0') {
    XELOGI("CPU: {}", cpu_brand_);
  }
  
#if XE_PLATFORM_MAC
  if (features_.rosetta2) {
    bool avx_ok = xe::xsimd_support::IsAVXSafeToExecute();
    bool avx2_ok = xe::xsimd_support::IsAVX2SafeToExecute();
    XELOGI("DEBUG: ROSETTA 2 DETECTED - Running x86_64 code via translation");
    XELOGI("AVX probe:  {} (VEX instructions)", avx_ok ? "PASS" : "FAIL");
    XELOGI("AVX2 probe: {} (YMM integer ops)", avx2_ok ? "PASS" : "FAIL");
    XELOGI("Library emulation (xsimd/SIMDe) applies to host code only");
  }
#endif
  
  XELOGI("");
  XELOGI("Native Hardware SIMD Support:");
  XELOGI("SSE2: {}", AvailableMissing(has_sse2()));
  XELOGI("SSE3: {}", AvailableMissing(has_sse3()));
  XELOGI("SSSE3: {}", AvailableMissing(has_ssse3()));
  XELOGI("SSE4.1: {}", AvailableMissing(has_sse41()));
  XELOGI("SSE4.2: {}", AvailableMissing(has_sse42()));
  XELOGI("PCLMULQDQ: {}", AvailableMissing(has_pclmulqdq()));
  
  XELOGI("AVX: {}", AvailableMissing(has_avx()));
  XELOGI("AVX2: {}", AvailableMissing(has_avx2()));
  if (has_avx512f()) {
    XELOGI("AVX-512F: available");
    XELOGI("AVX-512DQ: {}", AvailableMissing(has_avx512dq()));
    XELOGI("AVX-512BW: {}", AvailableMissing(has_avx512bw()));
    XELOGI("AVX-512VL: {}", AvailableMissing(has_avx512vl()));
  }
  
  XELOGI("");
  XELOGI("Library-Emulated SIMD Support:");
  XELOGI("xsimd:");
  XELOGI("AVX: {}", has_xsimd_avx() ? "emulated" : "missing");
  XELOGI("AVX2: {}", has_xsimd_avx2() ? "emulated" : "missing");
  XELOGI("AVX-512:  {}",
         has_xsimd_avx512()
             ? "emulated"
             : (has_simde_avx512() ? "SIMDe fallback" : "missing"));
  XELOGI("SIMDe:");
  XELOGI("AVX: {}", has_simde_avx() ? "emulated" : "missing");
  XELOGI("AVX2: {}", has_simde_avx2() ? "emulated" : "missing");
  XELOGI("AVX-512: {}", has_simde_avx512() ? "emulated" : "missing");
  XELOGI("Effective Emulation:");
  XELOGI("AVX-512: {}", (has_xsimd_avx512() || has_simde_avx512())
                             ? "emulated"
                             : "missing");
  XELOGI("Translation Helpers:");
  XELOGI("vex2NEON: {}", AvailableMissing(has_vex2neon()));
  XELOGI("ymm2NEON: {}", AvailableMissing(has_ymm2neon()));
  XELOGI("bmi2NEON: {}", AvailableMissing(has_bmi2neon()));
  XELOGI("ppcFloat2NEON: {}", AvailableMissing(has_ppcfloat2neon()));
  
  XELOGI("");
  XELOGI("PowerPC VMX/AltiVecEmulation:");
  if (has_vmx2neon()) {
    #if defined(__aarch64__) && !defined(__x86_64__)
    XELOGI("Method:     vmx2NEON (VMX -> ARM NEON)");
    XELOGI("Status:     available Native ARM64 VMX emulation available");
    #else
    XELOGI("Method:     x64 Backend (VMX -> SSE/AVX)");
    XELOGI("Status:     available x86_64 VMX emulation available");
    XELOGI("Details:    Xbox 360 VMX instructions translated to SSE/AVX");
    #endif
    XELOGI("Impact:     Xbox 360 games with VMX code will work correctly");
  } else {
    XELOGI("Status:     not available VMX emulation NOT available");
    XELOGI("Impact:     CRITICAL - Many Xbox 360 games will NOT work!");
    XELOGI("Required:   SSE4.2 minimum for VMX emulation");
#if XE_PLATFORM_MAC
    if (features_.rosetta2) {
      XELOGI("Debug: Rosetta 2 detected, SSE4.2: {}",
             AvailableMissing(features_.sse42));
      XELOGI("Debug: VMX library support: {}",
             AvailableMissing(library_support_.vmx2neon));
    }
#endif
  }
  
  XELOGI("");
  XELOGI("Other Extensions:");
  XELOGI("BMI1: {}",
         has_bmi1() ? "available" : (has_simde_bmi1() ? "emulated" : "missing"));
  XELOGI("BMI2: {}",
         has_bmi2() ? "available" : (has_simde_bmi2() ? "emulated" : "missing"));
  XELOGI("FMA: {}",
         has_fma() ? "available" : (has_simde_fma() ? "emulated" : "missing"));
  XELOGI("F16C: {}",
         has_f16c() ? "available" : (has_simde_f16c() ? "emulated" : "missing"));
  
  XELOGI("=======================================================");
}

void CpuFeatures::ValidateRequiredFeatures() const {
  // Xenia requires at minimum SSE4.2
  if (!has_sse42()) {
    XELOGE("=======================================================");
    XELOGE("FATAL: CPU does not support required instruction sets!");
    XELOGE("Xenia requires at minimum: SSE4.2");
    XELOGE("=======================================================");
    // Don't call exit() here - let the app handle it
  }
  
  // Warn about Rosetta limitations
#if XE_PLATFORM_MAC
  if (features_.rosetta2) {
    XELOGI("=======================================================");
    XELOGI("NOTICE: Running under Rosetta 2 translation");
    XELOGI("Hardware config: SSE4.2 + PCLMULQDQ only (AVX masked)");
    XELOGI("Emulation: xsimd={} SIMDe={} AVX-512={}",
           YesNo(has_xsimd_avx()), YesNo(has_simde_avx()),
           YesNo(has_xsimd_avx512() || has_simde_avx512()));
    XELOGI("=======================================================");
  }
#endif
}

}  // namespace xe
