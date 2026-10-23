/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CRYPT_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CRYPT_H_

#include "xenia/kernel/util/shim_utils.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Returns the 16-byte key at idx, or nullptr if it isn't known. Only 0x19
// (roamable obfuscation key) is available.
const uint8_t* XeKeysGetKeyData(uint32_t idx, bool devkit = false);

void XeCryptHmacSha_entry(lpvoid_t key, dword_t key_size_in, lpvoid_t inp_1,
                          dword_t inp_1_size, lpvoid_t inp_2,
                          dword_t inp_2_size, lpvoid_t inp_3,
                          dword_t inp_3_size, lpvoid_t out, dword_t out_size);

void XeCryptRc4_entry(lpvoid_t key, dword_t key_size, lpvoid_t data,
                      dword_t size);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_CRYPT_H_
