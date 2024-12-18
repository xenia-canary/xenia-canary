/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x00050002: {
      // Current session must have PRESENCE flag.
      XELOGD("XInviteSend({:08X}, {:08X}), implemented by netplay build",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00050008: {
      // Required to be successful for 534507D4
      // Guess:
      // XStorageDownloadToMemory -> XStorageDownloadToMemoryGetProgress
      XELOGD(
          "XStorageDownloadToMemoryGetProgress({:08x}, {:08x}), unimplemented",
          buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050009: {
      // Fixes Xbox Live error for 513107D9
      XELOGD("XStorageDownloadToMemory({:08X}, {:08X}), unimplemented",
             buffer_ptr, buffer_length);
      return XStorageDownloadToMemory(buffer_ptr);
    }
    case 0x0005000A: {
      XELOGD("XStorageEnumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005000B: {
      // Fixes Xbox Live error for 43430821
      XELOGD("XStorageUploadFromMemory({:08X}, {:08X}), unimplemented",
             buffer_ptr, buffer_length);
      return XStorageUploadFromMemory(buffer_ptr);
    }
    case 0x0005000C: {
      XELOGD("XStringVerify({:08X} {:08X}), Stubbed", buffer_ptr,
             buffer_length);
      return XStringVerify(buffer_ptr, buffer_length);
    }
    case 0x0005000D: {
      // Fixes hang when leaving session for 545107D5
      // 415607D2 says this is XStringVerify
      XELOGD("XStringVerify({:08X}, {:08X}), Stubbed", buffer_ptr,
             buffer_length);
      return XStringVerify(buffer_ptr, buffer_length);
    }
    case 0x0005000E: {
      // Before every call there is a call to XUserFindUsers
      // Success stub:
      // 584113E8 successfully creates session.
      // 58410B5D craches.
      XELOGD("XUserFindUsersResponseSize({:08X}, {:08X}) unimplemented",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x0005000F: {
      // 41560855 included from TU 7
      // Attempts to set a dvar for ui_email_address but fails on
      // WideCharToMultiByte
      //
      // 4D530AA5 encounters "Failed to retrieve account credentials".
      XELOGD("_XAccountGetUserInfo({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_ERROR_FUNCTION_FAILED;
    }
    case 0x00050010: {
      XELOGD("XAccountGetUserInfo({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_ERROR_FUNCTION_FAILED;
    }
    case 0x00050036: {
      XELOGD("XOnlineQuerySearch({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050038: {
      // 4D5307D3
      // 4D5307D1
      XELOGD("XOnlineQuerySearch({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050077: {
      // Called on blades dashboard v1888
      // Current Balance in sub menus:
      // All New Demos and Trailers
      // More Videos and Downloads
      XELOGD("XLiveBaseUnk50077({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050079: {
      // Fixes Xbox Live error for 454107DB
      XELOGD("XLiveBaseUnk50079({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005008B: {
      // Called on blades dashboard v1888
      // Fixes accessing marketplace Featured Downloads.
      XELOGD("XLiveBaseUnk5008B({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x0005008F: {
      // Called on blades dashboard v1888
      // Fixes accessing marketplace sub menus:
      // All New Demos and Trailers
      // More Videos and Downloads
      XELOGD("XLiveBaseUnk5008F({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050090: {
      // Called on blades dashboard v1888
      // Fixes accessing marketplace Game Downloads->All Games->Xbox Live Arcade
      // sub menu.
      XELOGD("XLiveBaseUnk50090({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050091: {
      // Called on blades dashboard v1888
      // Fixes accessing marketplace Game Downloads.
      XELOGD("XLiveBaseUnk50091({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094({:08X}, {:08X}), unimplemented", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x00050097: {
      // Called on blades dashboard v1888
      // Fixes accessing marketplace Memberships.
      XELOGD("XLiveBaseUnk50097({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058003: {
      // Called on startup of dashboard (netplay build)
      // Always receives a buffer ptr and buffer length of zero
      XELOGD("XLiveBaseLogonGetHR, implemented by netplay build");
      return 0x00151000L;  // X_ONLINE_E_LOGON_NO_NETWORK_CONNECTION
    }
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058009: {
      XELOGD("XContentGetMarketplaceCounts({:08X}, {:08X}) unimplemented",
             buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005800C: {
      XELOGD(
          "XUserMuteListSetState({:08X}, {:08X}), implemented by netplay build",
          buffer_ptr, buffer_length);

      return X_E_FAIL;
    }
    case 0x0005800E: {
      // Fixes Xbox Live error for 513107D9
      XELOGD("XUserMuteListQuery({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058017: {
      XELOGD("XUserFindUsers({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058019: {
      // 54510846
      XELOGD(
          "XPresenceCreateEnumerator({:08X}, {:08X}), Stubbed by netplay build",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x0005801C: {
      // Called on blades dashboard v1888
      XELOGD("XLiveBaseUnk5801C({:08X}, {:08X}), Stubbed by netplay build",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x0005801E: {
      // 54510846
      XELOGD("XPresenceSubscribe({:08X}, {:08X}), Stubbed by netplay build",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      XELOGD("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, "
          "{:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058024: {
      // Called on blades dashboard v1888
      XELOGD("XLiveBaseUnk58024({:08X}, {:08X}), Stubbed by netplay build",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058032: {
      XELOGD("XGetTaskProgress({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      // Fixes Xbox Live error for 513107D9
      // Required for 534507D4
      XELOGD(
          "XStorageBuildServerPath({:08X}, {:08X}), Stubbed by netplay "
          "build",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058037: {
      // Used in older games such as Crackdown, FM2, Saints Row 1
      XELOGD("XPresenceInitializeLegacy({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058044: {
      XELOGD("XPresenceUnsubscribe({:08X}, {:08X}), Stubbed by netplay build",
             buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Used in newer games such as Forza 4, MW3, FH2
      //
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only
      // takes input
      XELOGD("XPresenceInitialize({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, "
      "arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

X_HRESULT XLiveBaseApp::XStringVerify(uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  if (!buffer_ptr) {
    return X_E_INVALIDARG;
  }

  // uint32_t* data_ptr =
  // kernel_state_->memory()->TranslateVirtual<uint32_t*>(buffer_ptr);

  // TODO(Gliniak): Figure out structure after marshaling.
  // Based on what game does there must be some structure that
  // checks if string is proper.
  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::XStorageDownloadToMemory(uint32_t buffer_ptr) {
  // 41560817, 513107D5, 513107D9 has issues with X_E_FAIL.
  // 513107D5, 513107D9 prefer X_ERROR_FUNCTION_FAILED.

  if (!buffer_ptr) {
    return X_E_INVALIDARG;
  }

  // 415607DD has issues with X_E_SUCCESS and X_ERROR_FUNCTION_FAILED.
  // if (kernel_state()->title_id() == 0x415607DD) {
  //  return X_E_FAIL;
  // }

  return X_E_SUCCESS;
}

X_HRESULT XLiveBaseApp::XStorageUploadFromMemory(uint32_t buffer_ptr) {
  if (!buffer_ptr) {
    return X_E_INVALIDARG;
  }

  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
