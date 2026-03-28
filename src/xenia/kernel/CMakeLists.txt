add_library(xenia-kernel STATIC)
xe_platform_sources(xenia-kernel ${CMAKE_CURRENT_SOURCE_DIR} RECURSIVE)
target_compile_definitions(xenia-kernel PRIVATE WITH_GZFILEOP)
if(XE_TARGET_X86_64)
  target_compile_definitions(xenia-kernel PRIVATE
    X86_FEATURES
    X86_HAVE_XSAVE_INTRIN
    X86_SSE2
    X86_SSSE3
    X86_SSE42
    X86_AVX2
    X86_AVX512
    X86_AVX512VNNI
    X86_PCLMULQDQ_CRC
    X86_VPCLMULQDQ_CRC
  )
endif()
target_link_libraries(xenia-kernel PUBLIC
  aes_128 fmt zlib-ng pugixml xenia-apu xenia-base xenia-cpu xenia-hid xenia-vfs
)
xe_target_defaults(xenia-kernel)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
endif()
