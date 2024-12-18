/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// Enum
dword_result_t XamAvatarBeginEnumAssets_entry(
    unknown_t unk1,  //  4 bytes, Var1 + 0x1ed0144
    unknown_t unk2,  //  0x32
    unknown_t unk3,  //  0x1ffffff
    unknown_t unk4,  //  ULONGLONG, (LZCOUNT(Var2) << 0x20) >> 0x25 ^ 1) + 1
    unknown_t unk5,  //  1
    unknown_t unk6   //  0
) {
  // XMsgStartIORequestEx(0xf3, 0x60000c, unkn6, stack1, 0x14, stack2)
  XELOGD("Stubbed");
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarBeginEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEnumAssets_entry(
    unknown_t unk1,  // Var1 + (0x1A0 or 0x1b0)
    unknown_t unk2,  // Var1 + 0x90
    unknown_t unk3   // 0
) {
  // XMsgStartIORequestEx(0xf3, 0x60000d, unk3, stack1, 8, stack2)

  XELOGD("Stubbed");
  return X_E_NO_MORE_FILES;  // Stop it from calling endlessly
}
DECLARE_XAM_EXPORT1(XamAvatarEnumAssets, kAvatars, kStub);

dword_result_t XamAvatarEndEnumAssets_entry(unknown_t unk1) {
  // some_function(0x60000e,param_1,0,0,local_10)

  XELOGD("Stubbed");
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarEndEnumAssets, kAvatars, kStub);

// Get
dword_result_t XamAvatarGetAssetsResultSize_entry(unknown_t unk1,  // ?
                                                  unknown_t unk2,  // ?
                                                  unknown_t unk3   // ?
) {
  XELOGD("Stubbed");
  // return XMsgInProcessCall(0xf3,0x600005,local_20,0);
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetsResultSize, kAvatars, kStub);

dword_result_t XamAvatarGetAssets_entry(unknown_t unk1,  // ?
                                        unknown_t unk2,  // ?
                                        unknown_t unk3,  // ?
                                        unknown_t unk4,  // ?
                                        unknown_t unk5,  // ?
                                        unknown_t unk6   // ?
) {
  XELOGD("Stubbed");
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssets, kAvatars, kStub);

dword_result_t XamAvatarGetAssetIcon_entry(
    unknown_t unk1,  // param_2, param_1 + 0x48
    unknown_t unk2,  // 0, param_3
    unknown_t unk3,  // param_1 + 0x3c, Var1 + 3
    unknown_t unk4,  // 4b, param_1 + 0x38, Var1[2]
    unknown_t unk5   // param_1 + 0x58, Var + 8
) {
  // XMsgStartIORequestEx(0xf3, 0x60000B, unk5, stack1, 0x1C, 0x10000000)

  XELOGD("Stubbed");
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetAssetIcon, kAvatars, kStub);

dword_result_t XamAvatarGetManifestLocalUser_entry(unknown_t unk1,  // 4b,
                                                   unknown_t unk2,  // 4b,
                                                   unknown_t unk3   // 8b,
) {
  // XMsgStartIORequestEx(0xf3, 0x600003, unk3, stack1, 8, 0)

  XELOGD("Stubbed");
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetManifestLocalUser, kAvatars, kStub);

dword_result_t XamAvatarGetMetadataRandom_entry(unknown_t unk1,  // ?
                                                unknown_t unk2,  // ?
                                                unknown_t unk3,  // ?
                                                unknown_t unk4   // ?
) {
  XELOGD("Stubbed");
  // some_function(0x600010,param_4,local_28,0xc,local_30)
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarGetMetadataRandom, kAvatars, kStub);

dword_result_t XamAvatarManifestGetBodyType_entry(unknown_t body_type) {
  // return either char of 1 - male, 2 - female, else - unknown
  XELOGD("Stubbed");
  return '\x01';
}
DECLARE_XAM_EXPORT1(XamAvatarManifestGetBodyType, kAvatars, kStub);

// Start/End
dword_result_t XamAvatarInitialize_entry(
    dword_t unk1,              // 1, 2, 4, etc
    dword_t unk2,              // 0 or 1
    dword_t processor_number,  // for thread creation?
    lpdword_t function_ptrs,   // 20b, 5 pointers
    lpunknown_t unk5,          // ptr in data segment
    dword_t unk6               // flags - 0x00300000, 0x30, etc
) {
  // Negative to fail. Game should immediately call XamAvatarShutdown.
  return X_STATUS_SUCCESS;  //~0u;
}
DECLARE_XAM_EXPORT1(XamAvatarInitialize, kAvatars, kStub);

void XamAvatarShutdown_entry() {
  // Calls XMsgStartIORequestEx(0xf3,0x600002,0,0,0,0).
}
DECLARE_XAM_EXPORT1(XamAvatarShutdown, kAvatars, kStub);

// Animation
dword_result_t XamAvatarLoadAnimation_entry(
    // https://github.com/xenia-canary/xenia-canary/commit/212c99eee2724de15f471148d10197d89794ff32
    dword_t asset_id_ptr,  //
    dword_t flags,         //
    dword_t output,        //  Var1 + 0x2C
    unknown_t unk1         //  Var1 + 0x10
) {
  // XMsgStartIORequestEx(0xf3, 0x60000F, unk4, stack1, 0x18, 0x10000000)

  XELOGD("Stubbed");
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamAvatarLoadAnimation, kAvatars, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Avatar);
