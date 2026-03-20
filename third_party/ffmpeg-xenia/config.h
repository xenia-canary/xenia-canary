/*
 * FFmpeg build configuration for xenia.
 * Replaces 4 per-platform generated config headers with a single file
 * using preprocessor platform detection.
 * Undefined macros evaluate to 0 in #if checks.
 */
#ifndef FFMPEG_CONFIG_H
#define FFMPEG_CONFIG_H

#define FFMPEG_CONFIGURATION "xenia-custom"
#define FFMPEG_LICENSE "LGPL version 2.1 or later"
#define CONFIG_THIS_YEAR 2021
#define av_restrict restrict
#define EXTERN_PREFIX ""
#define EXTERN_ASM
#define BUILDSUF ""
#define HAVE_MMX2 HAVE_MMXEXT
#define SWS_MAX_FILTER_SIZE 256

/* ------------------------------------------------------------------ */
/*  Architecture + CPU features                                       */
/*  Some of these are used in C if() expressions, not just #if, so    */
/*  every ARCH_* / HAVE_*_EXTERNAL must be explicitly defined.        */
/* ------------------------------------------------------------------ */

/* Zero-default all architecture flags (used in C if() expressions) */
#define ARCH_AARCH64 0
#define ARCH_ALPHA 0
#define ARCH_ARM 0
#define ARCH_MIPS 0
#define ARCH_PPC 0
#define ARCH_X86 0
#define ARCH_X86_64 0

#if defined(__aarch64__) || defined(_M_ARM64)
  /* ARM64 */
  #if defined(_WIN32)
    #define SLIBSUF ".dll"
  #else
    #define SLIBSUF ".so"
  #endif
  #undef  ARCH_AARCH64
  #define ARCH_AARCH64 1
  #define HAVE_ARMV8 1
  #define HAVE_NEON 1
  #define HAVE_VFP 1
  #define HAVE_ARMV8_EXTERNAL 1
  #define HAVE_NEON_EXTERNAL 1
  #define HAVE_VFP_EXTERNAL 1
  #if defined(_MSC_VER)
    #define HAVE_ARMV8_INLINE 0
    #define HAVE_NEON_INLINE 0
    #define HAVE_VFP_INLINE 0
  #else
    #define HAVE_ARMV8_INLINE 1
    #define HAVE_NEON_INLINE 1
    #define HAVE_VFP_INLINE 1
  #endif
  #define HAVE_INTRINSICS_NEON 1
  #define HAVE_AS_FUNC 0
  #define HAVE_AS_ARCH_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_CRC_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_I8MM_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_SVE_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_SVE2_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_SME_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_SME_I16I64_DIRECTIVE 0
  #define HAVE_AS_ARCHEXT_SME2_DIRECTIVE 0
  #define HAVE_ALIGNED_STACK 1
  #define HAVE_FAST_64BIT 1
  #define HAVE_FAST_CLZ 1
  #define HAVE_LOCAL_ALIGNED 1
  #define HAVE_SIMD_ALIGN_16 1
  #define HAVE_SIMD_ALIGN_32 0
  #define HAVE_SIMD_ALIGN_64 0
#elif defined(__x86_64__) || defined(_M_X64)
  /* x86_64 (Windows, Linux, Android x86_64) */
  #if defined(_WIN32)
    #define SLIBSUF ".dll"
  #else
    #define SLIBSUF ".so"
  #endif
  #undef  ARCH_X86
  #define ARCH_X86 1
  #undef  ARCH_X86_64
  #define ARCH_X86_64 1
  #define HAVE_AESNI 1
  #define HAVE_AMD3DNOW 1
  #define HAVE_AMD3DNOWEXT 1
  #define HAVE_AVX 1
  #define HAVE_AVX2 1
  #define HAVE_AVX512 1
  #define HAVE_FMA3 1
  #define HAVE_FMA4 1
  #define HAVE_MMX 1
  #define HAVE_MMXEXT 1
  #define HAVE_SSE 1
  #define HAVE_SSE2 1
  #define HAVE_SSE3 1
  #define HAVE_SSE4 1
  #define HAVE_SSE42 1
  #define HAVE_SSSE3 1
  #define HAVE_XOP 1
  #define HAVE_CPUNOP 1
  #define HAVE_I686 1
  #define HAVE_ALIGNED_STACK 1
  #define HAVE_FAST_64BIT 1
  #define HAVE_FAST_CLZ 1
  #define HAVE_FAST_CMOV 1
  #define HAVE_LOCAL_ALIGNED 1
  #define HAVE_SIMD_ALIGN_16 1
  #define HAVE_SIMD_ALIGN_32 1
  #define HAVE_SIMD_ALIGN_64 1
  /* GCC/Clang x86 inline asm variants */
  #if !defined(_MSC_VER)
    #define HAVE_AESNI_INLINE 1
    #define HAVE_AMD3DNOW_INLINE 1
    #define HAVE_AMD3DNOWEXT_INLINE 1
    #define HAVE_AVX_INLINE 1
    #define HAVE_AVX2_INLINE 1
    #define HAVE_AVX512_INLINE 1
    #define HAVE_FMA3_INLINE 1
    #define HAVE_FMA4_INLINE 1
    #define HAVE_MMX_INLINE 1
    #define HAVE_MMXEXT_INLINE 1
    #define HAVE_SSE_INLINE 1
    #define HAVE_SSE2_INLINE 1
    #define HAVE_SSE3_INLINE 1
    #define HAVE_SSE4_INLINE 1
    #define HAVE_SSE42_INLINE 1
    #define HAVE_SSSE3_INLINE 1
    #define HAVE_XOP_INLINE 1
    #define HAVE_MM_EMPTY 1
    #define HAVE_EBP_AVAILABLE 1
    #define HAVE_EBX_AVAILABLE 1
    #define HAVE_XMM_CLOBBERS 1
    #define HAVE_INLINE_ASM_DIRECT_SYMBOL_REFS 1
  #endif
#else
  #error "Unsupported architecture for FFmpeg build"
#endif

/* Endianness (used in C expressions, not just #if) */
#define HAVE_BIGENDIAN 0

/* Zero-default all INLINE flags (only set to 1 on GCC/Clang above) */
#ifndef HAVE_AESNI_INLINE
#define HAVE_AESNI_INLINE 0
#endif
#ifndef HAVE_AMD3DNOW_INLINE
#define HAVE_AMD3DNOW_INLINE 0
#endif
#ifndef HAVE_AMD3DNOWEXT_INLINE
#define HAVE_AMD3DNOWEXT_INLINE 0
#endif
#ifndef HAVE_AVX_INLINE
#define HAVE_AVX_INLINE 0
#endif
#ifndef HAVE_AVX2_INLINE
#define HAVE_AVX2_INLINE 0
#endif
#ifndef HAVE_AVX512_INLINE
#define HAVE_AVX512_INLINE 0
#endif
#ifndef HAVE_FMA3_INLINE
#define HAVE_FMA3_INLINE 0
#endif
#ifndef HAVE_FMA4_INLINE
#define HAVE_FMA4_INLINE 0
#endif
#ifndef HAVE_MMX_INLINE
#define HAVE_MMX_INLINE 0
#endif
#ifndef HAVE_MMXEXT_INLINE
#define HAVE_MMXEXT_INLINE 0
#endif
#ifndef HAVE_SSE_INLINE
#define HAVE_SSE_INLINE 0
#endif
#ifndef HAVE_SSE2_INLINE
#define HAVE_SSE2_INLINE 0
#endif
#ifndef HAVE_SSE3_INLINE
#define HAVE_SSE3_INLINE 0
#endif
#ifndef HAVE_SSE4_INLINE
#define HAVE_SSE4_INLINE 0
#endif
#ifndef HAVE_SSE42_INLINE
#define HAVE_SSE42_INLINE 0
#endif
#ifndef HAVE_SSSE3_INLINE
#define HAVE_SSSE3_INLINE 0
#endif
#ifndef HAVE_XOP_INLINE
#define HAVE_XOP_INLINE 0
#endif

/* x86asm (nasm/yasm) is not used; all EXTERNAL flags are 0 */
#define HAVE_X86ASM 0
#define HAVE_AESNI_EXTERNAL 0
#define HAVE_AMD3DNOW_EXTERNAL 0
#define HAVE_AMD3DNOWEXT_EXTERNAL 0
#define HAVE_AVX_EXTERNAL 0
#define HAVE_AVX2_EXTERNAL 0
#define HAVE_AVX512_EXTERNAL 0
#define HAVE_FMA3_EXTERNAL 0
#define HAVE_FMA4_EXTERNAL 0
#define HAVE_MMX_EXTERNAL 0
#define HAVE_MMXEXT_EXTERNAL 0
#define HAVE_SSE_EXTERNAL 0
#define HAVE_SSE2_EXTERNAL 0
#define HAVE_SSE3_EXTERNAL 0
#define HAVE_SSE4_EXTERNAL 0
#define HAVE_SSE42_EXTERNAL 0
#define HAVE_SSSE3_EXTERNAL 0
#define HAVE_XOP_EXTERNAL 0
#define HAVE_CPUNOP_EXTERNAL 0
#define HAVE_I686_EXTERNAL 0

/* MIPS CPU features (used in C if() expressions) */
#define HAVE_MIPSFPU 0
#define HAVE_MIPSDSP 0

/* CPU features used in C expressions cross-platform (e.g. mem.c ALIGN) */
#ifndef HAVE_AVX
#define HAVE_AVX 0
#endif
#ifndef HAVE_AVX512
#define HAVE_AVX512 0
#endif

/* Disabled features (used in C if() expressions, not just #if) */
#define CONFIG_FRAME_THREAD_ENCODER 0
#define CONFIG_GRAY 0
#define CONFIG_MEMORY_POISONING 0
#define CONFIG_MPEG4_DECODER 0
#define CONFIG_SMALL 0
#define CONFIG_ZLIB 0

/* ------------------------------------------------------------------ */
/*  OS-specific defines                                               */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
  /* Windows (MSVC) */
  #define HAVE_W32THREADS 1
  #define HAVE_WINDOWS_H 1
  #define HAVE_IO_H 1
  #define HAVE_DIRECT_H 1
  #define HAVE_DXGIDEBUG_H 1
  #define HAVE_DXVA_H 1
  #define HAVE_ALIGNED_MALLOC 1
  #define HAVE_BCRYPT 1
  #define HAVE_DOS_PATHS 1
  #define HAVE_LIBC_MSVCRT 1
  #define HAVE_MEMORYBARRIER 1
  #define HAVE_RDTSC 1
  #define HAVE_COMMANDLINETOARGVW 1
  #define HAVE_GETMODULEHANDLE 1
  #define HAVE_GETPROCESSAFFINITYMASK 1
  #define HAVE_GETPROCESSMEMORYINFO 1
  #define HAVE_GETPROCESSTIMES 1
  #define HAVE_GETSTDHANDLE 1
  #define HAVE_GETSYSTEMTIMEASFILETIME 1
  #define HAVE_KBHIT 1
  #define HAVE_MAPVIEWOFFILE 1
  #define HAVE_PEEKNAMEDPIPE 1
  #define HAVE_SETCONSOLETEXTATTRIBUTE 1
  #define HAVE_SETCONSOLECTRLHANDLER 1
  #define HAVE_SETDLLDIRECTORY 1
  #define HAVE_SETMODE 1
  #define HAVE_SLEEP 1
  #define HAVE_VIRTUALALLOC 1
#else
  /* POSIX (Linux, Android) */
  #define HAVE_DOS_PATHS 0
  #define HAVE_PTHREADS 1
  #define HAVE_INLINE_ASM 1
  #define HAVE_UNISTD_H 1
  #define HAVE_ARPA_INET_H 1
  #define HAVE_ASM_TYPES_H 1
  #define HAVE_DIRENT_H 1
  #define HAVE_LINUX_PERF_EVENT_H 1
  #define HAVE_POLL_H 1
  #define HAVE_SYS_PARAM_H 1
  #define HAVE_SYS_RESOURCE_H 1
  #define HAVE_SYS_SELECT_H 1
  #define HAVE_SYS_TIME_H 1
  #define HAVE_SYS_UN_H 1
  #define HAVE_TERMIOS_H 1
  #define HAVE_SEM_TIMEDWAIT 1
  #define HAVE_SYNC_VAL_COMPARE_AND_SWAP 1
  #define HAVE_SECTION_DATA_REL_RO 1
  #define HAVE_CLOCK_GETTIME 1
  #define HAVE_FCNTL 1
  #define HAVE_GETADDRINFO 1
  #define HAVE_GETOPT 1
  #define HAVE_GETRUSAGE 1
  #define HAVE_GETTIMEOFDAY 1
  #define HAVE_GMTIME_R 1
  #define HAVE_INET_ATON 1
  #define HAVE_LOCALTIME_R 1
  #define HAVE_LSTAT 1
  #define HAVE_MEMALIGN 1
  #define HAVE_MKSTEMP 1
  #define HAVE_MMAP 1
  #define HAVE_MPROTECT 1
  #define HAVE_NANOSLEEP 1
  #define HAVE_POSIX_MEMALIGN 1
  #define HAVE_SCHED_GETAFFINITY 1
  #define HAVE_SETRLIMIT 1
  #define HAVE_STRERROR_R 1
  #define HAVE_SYSCONF 1
  #define HAVE_USLEEP 1
  #define HAVE_INLINE_ASM_LABELS 1
  #define HAVE_INLINE_ASM_NONLOCAL_LABELS 1
  #define HAVE_SYMVER_ASM_LABEL 1
  #define HAVE_SYMVER_GNU_ASM 1
  #define HAVE_SOCKLEN_T 1
  #define HAVE_STRUCT_ADDRINFO 1
  #define HAVE_STRUCT_GROUP_SOURCE_REQ 1
  #define HAVE_STRUCT_IP_MREQ_SOURCE 1
  #define HAVE_STRUCT_IPV6_MREQ 1
  #define HAVE_STRUCT_MSGHDR_MSG_FLAGS 1
  #define HAVE_STRUCT_POLLFD 1
  #define HAVE_STRUCT_RUSAGE_RU_MAXRSS 1
  #define HAVE_STRUCT_SOCKADDR_IN6 1
  #define HAVE_STRUCT_SOCKADDR_STORAGE 1
  #define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
  #define CONFIG_PIC 1
  #if defined(__ANDROID__)
    #define HAVE_ARC4RANDOM 1
    #define HAVE_RSYNC_CONTIMEOUT 1
  #else
    /* Linux desktop */
    #define HAVE_PTHREAD_CANCEL 1
    #define HAVE_SYMVER 1
    #define HAVE_SYS_SOUNDCARD_H 1
    #define HAVE_GLOB 1
    #define CONFIG_ICONV 1
  #endif
#endif

/* ------------------------------------------------------------------ */
/*  Common to all platforms                                           */
/* ------------------------------------------------------------------ */

#define HAVE_FAST_UNALIGNED 1
#define HAVE_THREADS 1
#define HAVE_ACCESS 1
#define HAVE_ISATTY 1
#define HAVE_MALLOC_H 1
#define HAVE_PRAGMA_DEPRECATED 1

/* Math functions */
#define HAVE_ATANF 1
#define HAVE_ATAN2F 1
#define HAVE_CBRT 1
#define HAVE_CBRTF 1
#define HAVE_COPYSIGN 1
#define HAVE_COSF 1
#define HAVE_ERF 1
#define HAVE_EXP2 1
#define HAVE_EXP2F 1
#define HAVE_EXPF 1
#define HAVE_HYPOT 1
#define HAVE_ISFINITE 1
#define HAVE_ISINF 1
#define HAVE_ISNAN 1
#define HAVE_LDEXPF 1
#define HAVE_LLRINT 1
#define HAVE_LLRINTF 1
#define HAVE_LOG2 1
#define HAVE_LOG2F 1
#define HAVE_LOG10F 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_POWF 1
#define HAVE_RINT 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_SINF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1

/* Enabled library components */
#define CONFIG_AVCODEC 1
#define CONFIG_AVFORMAT 1
#define CONFIG_AVUTIL 1
#define CONFIG_RUNTIME_CPUDETECT 1
#define CONFIG_SAFE_BITSTREAM_READER 1
#define CONFIG_STATIC 1
#define CONFIG_SWSCALE_ALPHA 1

/* DSP / transform support */
#define CONFIG_DCT 1
#define CONFIG_FAAN 1
#define CONFIG_FAANDCT 1
#define CONFIG_FAANIDCT 1
#define CONFIG_FAST_UNALIGNED 1
#define CONFIG_FDCTDSP 1
#define CONFIG_FFT 1
#define CONFIG_IDCTDSP 1
#define CONFIG_MDCT 1
#define CONFIG_RDFT 1
#define CONFIG_SINEWIN 1

/* Audio codec support */
#define CONFIG_MPEGAUDIO 1
#define CONFIG_MPEGAUDIODSP 1
#define CONFIG_MPEGAUDIOHEADER 1
#define CONFIG_WMA_FREQS 1
#define CONFIG_RIFFDEC 1

/* Category enables */
#define CONFIG_BSFS 1
#define CONFIG_DECODERS 1
#define CONFIG_DEMUXERS 1
#define CONFIG_PARSERS 1
#define CONFIG_PROTOCOLS 1

/* Enabled codecs */
#define CONFIG_MP3_DECODER 1
#define CONFIG_MP3FLOAT_DECODER 1
#define CONFIG_WMAPRO_DECODER 1
#define CONFIG_WMAV2_DECODER 1
#define CONFIG_XMAFRAMES_DECODER 1

/* Enabled parsers */
#define CONFIG_MPEGAUDIO_PARSER 1

/* Enabled demuxers */
#define CONFIG_ASF_DEMUXER 1
#define CONFIG_MP3_DEMUXER 1

/* Enabled protocols */
#define CONFIG_FILE_PROTOCOL 1

/* Enabled bitstream filters */
#define CONFIG_NULL_BSF 1

#endif /* FFMPEG_CONFIG_H */
