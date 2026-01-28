#include <cstdio>
#include "xenia/base/xsimd_avx_support.h"

int main() {
  printf("Testing AVX support...\n");

  xe::xsimd_support::InitializeSIMDSupport();

  bool has_avx = xe::xsimd_support::IsAVXSupported();
  bool has_avx2 = xe::xsimd_support::IsAVX2Supported();

  printf("AVX supported: %s\n", has_avx ? "YES" : "NO");
  printf("AVX2 supported: %s\n", has_avx2 ? "YES" : "NO");
  printf("Running under Rosetta 2: %s\n",
         xe::xsimd_support::IsRunningUnderRosetta2() ? "YES" : "NO");

  return 0;
}
