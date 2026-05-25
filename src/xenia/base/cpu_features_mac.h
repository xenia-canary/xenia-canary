/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CPU_FEATURES_MAC_H_
#define XENIA_BASE_CPU_FEATURES_MAC_H_

#include <cstdint>

namespace xe {

/**
 * Runtime CPU feature detection utility.
 * 
 * Detects available SIMD instruction sets and provides safety checks
 * for environments like Rosetta 2 where certain instructions are not supported.
 * 
 * Usage:
 *   auto& cpu = CpuFeatures::Get();
 *   cpu.LogFeatures();
 *   if (cpu.has_avx2() && !cpu.is_rosetta2()) {
 *     // Use AVX2 instructions
 *   }
 */
class CpuFeatures {
 public:
  static CpuFeatures& Get();
  
  // SSE family
  bool has_sse2() const { return features_.sse2; }
  bool has_sse3() const { return features_.sse3; }
  bool has_ssse3() const { return features_.ssse3; }
  bool has_sse41() const { return features_.sse41; }
  bool has_sse42() const { return features_.sse42; }
  
  // AVX family (native hardware; may be unavailable under Rosetta 2)
  bool has_avx() const { return features_.avx; }
  bool has_avx2() const { return features_.avx2; }
  bool has_avx512f() const { return features_.avx512f; }
  bool has_avx512dq() const { return features_.avx512dq; }
  bool has_avx512bw() const { return features_.avx512bw; }
  bool has_avx512vl() const { return features_.avx512vl; }
  
  // Library-emulated support (xsimd, SIMDe)
  bool has_xsimd_avx() const { return library_support_.xsimd_avx; }
  bool has_xsimd_avx2() const { return library_support_.xsimd_avx2; }
  bool has_xsimd_avx512() const { return library_support_.xsimd_avx512; }
  bool has_simde_avx() const { return library_support_.simde_avx; }
  bool has_simde_avx2() const { return library_support_.simde_avx2; }
  bool has_simde_avx512() const { return library_support_.simde_avx512; }
  bool has_simde_bmi1() const { return library_support_.simde_bmi1; }
  bool has_simde_bmi2() const { return library_support_.simde_bmi2; }
  bool has_simde_fma() const { return library_support_.simde_fma; }
  bool has_simde_f16c() const { return library_support_.simde_f16c; }
  bool has_vex2neon() const { return library_support_.vex2neon; }
  bool has_ymm2neon() const { return library_support_.ymm2neon; }
  bool has_bmi2neon() const { return library_support_.bmi2neon; }
  bool has_ppcfloat2neon() const { return library_support_.ppcfloat2neon; }
  
  // VMX (PowerPC SIMD) emulation via vmx2NEON
  bool has_vmx2neon() const { return library_support_.vmx2neon; }
  
  // Other extensions
  bool has_pclmulqdq() const { return features_.pclmulqdq; }
  bool has_bmi1() const { return features_.bmi1; }
  bool has_bmi2() const { return features_.bmi2; }
  bool has_fma() const { return features_.fma; }
  bool has_f16c() const { return features_.f16c; }
  
  // Platform detection
  bool is_rosetta2() const { return features_.rosetta2; }
  bool is_emulated() const { return features_.rosetta2; }
  
  // Validation
  void LogFeatures() const;
  void ValidateRequiredFeatures() const;
  
  // Verification tests
  bool VerifyXsimdSupport() const;
  bool VerifySimdeSupport() const;
  bool VerifyVmx2NeonSupport() const;
  void RunAllVerificationTests() const;
  
  // Get a human-readable description of detected features
  const char* GetCpuString() const { return cpu_brand_; }
  
 private:
  CpuFeatures();
  ~CpuFeatures() = default;
  
  // No copying
  CpuFeatures(const CpuFeatures&) = delete;
  CpuFeatures& operator=(const CpuFeatures&) = delete;
  
  void DetectFeatures();
  void DetectRosetta();
  void DetectLibrarySupport();
  
  struct Features {
    // SSE
    bool sse2 = false;
    bool sse3 = false;
    bool ssse3 = false;
    bool sse41 = false;
    bool sse42 = false;
    
    // AVX (native hardware)
    bool avx = false;
    bool avx2 = false;
    bool avx512f = false;
    bool avx512dq = false;
    bool avx512bw = false;
    bool avx512vl = false;
    
    // Other
    bool pclmulqdq = false;
    bool bmi1 = false;
    bool bmi2 = false;
    bool fma = false;
    bool f16c = false;
    
    // Platform
    bool rosetta2 = false;
  } features_;
  
  struct LibrarySupport {
    // xsimd library support
    bool xsimd_avx = false;
    bool xsimd_avx2 = false;
    bool xsimd_avx512 = false;
    
    // SIMDe library support
    bool simde_avx = false;
    bool simde_avx2 = false;
    bool simde_avx512 = false;
    bool simde_bmi1 = false;
    bool simde_bmi2 = false;
    bool simde_fma = false;
    bool simde_f16c = false;
    
    // Rosetta translation helper libraries (header-only)
    bool vex2neon = false;
    bool ymm2neon = false;
    bool bmi2neon = false;
    bool ppcfloat2neon = false;

    // vmx2NEON support
    bool vmx2neon = false;
  } library_support_;
  
  char cpu_brand_[64] = {};
};

}  // namespace xe

#endif  // XENIA_BASE_CPU_FEATURES_MAC_H_
