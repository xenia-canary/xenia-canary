/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NUI_NUI_MANAGER_H_
#define XENIA_HID_NUI_NUI_MANAGER_H_

#include <memory>
#include <mutex>
#include <string>

#include "xenia/hid/nui/nui_backend.h"

namespace xe {
namespace hid {
namespace nui {

// ----------------------------------------------------------------------------
// NuiManager
//
// Process-singleton owning the active NUI backend. Constructed during
// emulator startup, after cvars are parsed. The active backend is
// determined by the --nui_backend cvar.
//
// xam_nui.cc is the only consumer; everything else routes through it.
// We deliberately do not expose backend selection at runtime via XAM —
// the guest cannot change which Kinect implementation is in use.
// ----------------------------------------------------------------------------

class NuiManager {
 public:
  // Singleton accessors. Setup() must be called once at emulator startup;
  // Shutdown() must be called at emulator shutdown.
  static void Setup();
  static void Shutdown();
  static NuiManager* Instance();

  // Active backend. Never null after Setup() — at minimum, the null
  // backend is installed.
  INuiBackend* backend() { return backend_.get(); }
  const INuiBackend* backend() const { return backend_.get(); }

  // Convenience: capability check.
  bool IsConnected() const { return backend_->IsConnected(); }

  // Translate a host-side microsecond timestamp (as reported by
  // INuiBackend::Poll* frames) into a guest-side timestamp matching
  // the calling guest's clock domain. Used by xam_nui.cc when
  // populating XAM NUI return structures.
  uint64_t HostUsToGuestTimestamp(uint64_t host_us) const;

  // Diagnostics. Returns a single-line summary suitable for the title bar
  // and crash logs: "kinectix: backend=recorded file=session.xnuirec
  //                  connected=yes skeletons=1".
  std::string StatusString() const;

  NuiManager(const NuiManager&) = delete;
  NuiManager& operator=(const NuiManager&) = delete;

 private:
  NuiManager() = default;
  ~NuiManager() = default;

  // Constructs the backend selected by --nui_backend cvar.
  // Falls back to NullNuiBackend on any failure (logs a warning).
  std::unique_ptr<INuiBackend> CreateBackendFromCvars();

  std::unique_ptr<INuiBackend> backend_;

  // Host clock at the moment Setup() returned. Used as the zero-point
  // for HostUsToGuestTimestamp() translation.
  uint64_t host_epoch_us_ = 0;
};

}  // namespace nui
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NUI_NUI_MANAGER_H_
