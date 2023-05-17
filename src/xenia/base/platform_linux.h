/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_PLATFORM_LINUX_H_
#define XENIA_BASE_PLATFORM_LINUX_H_

// NOTE: if you're including this file it means you are explicitly depending
// on Linux headers. Including this file outside of linux platform specific
// source code will break portability
#include <cstddef>

#include "xenia/base/platform.h"

#include <cstdint>
#include <glib.h>

typedef uint8_t BYTE;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int64_t LONGLONG;

typedef union _LARGE_INTEGER {
  struct {
    DWORD LowPart;
    LONG  HighPart;
  };
  struct {
    DWORD LowPart;
    LONG  HighPart;
  } u;
  LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

static constexpr size_t KUSER_SHARED_INTERRUPTTIME_OFFSET = 8;
static unsigned char* KUserShared() { return (unsigned char*)0x7FFE0000ULL; }

#endif  // XENIA_BASE_PLATFORM_LINUX_H_
