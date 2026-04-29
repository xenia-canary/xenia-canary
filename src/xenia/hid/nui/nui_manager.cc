/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Copyright 2026 Kinectix Contributors. All rights reserved.                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/nui/nui_manager.h"

#include <chrono>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/hid/nui/nui_null_backend.h"
#include "xenia/hid/nui/recorded/recorded_backend.h"

#if XE_KINECTIX_NUI_FREENECT
#include "xenia/hid/nui/freenect/freenect_backend.h"
#endif
#if XE_KINECTIX_NUI_FREENECT2
#include "xenia/hid/nui/freenect/freenect2_backend.h"
#endif
#if XE_KINECTIX_NUI_MEDIAPIPE
#include "xenia/hid/nui/mediapipe/mediapipe_backend.h"
#endif

DEFINE_string(
    nui_backend, "none",
    "Active NUI (Kinect) backend. One of: "
    "none, recorded, freenect, freenect2, mediapipe. "
    "Some values require build flags (KINECTIX_NUI_FREENECT, ...).",
    "NUI");

DEFINE_string(
    nui_record_path, "",
    "Path to a .xnuirec file when --nui_backend=recorded. "
    "If empty, the recorded backend operates in null mode.",
    "NUI");

namespace xe {
namespace hid {
namespace nui {

namespace {
NuiManager* g_instance = nullptr;

uint64_t MonotonicNowUs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::microseconds>(
             clock::now().time_since_epoch())
      .count();
}
}  // namespace

void NuiManager::Setup() {
  if (g_instance) {
    XELOGW("NuiManager::Setup() called twice; ignoring.");
    return;
  }
  g_instance = new NuiManager();
  g_instance->host_epoch_us_ = MonotonicNowUs();
  g_instance->backend_ = g_instance->CreateBackendFromCvars();

  if (!g_instance->backend_->Initialize()) {
    XELOGE("NUI backend '%s' failed to Initialize(); falling back to null.",
           g_instance->backend_->Name().c_str());
    g_instance->backend_ = std::make_unique<NullNuiBackend>();
    g_instance->backend_->Initialize();
  }

  XELOGI("NUI: %s", g_instance->StatusString().c_str());
}

void NuiManager::Shutdown() {
  if (!g_instance) {
    return;
  }
  if (g_instance->backend_) {
    g_instance->backend_->Shutdown();
  }
  delete g_instance;
  g_instance = nullptr;
}

NuiManager* NuiManager::Instance() { return g_instance; }

uint64_t NuiManager::HostUsToGuestTimestamp(uint64_t host_us) const {
  // For the recorded backend we want playback determinism: a fixture
  // recorded at host_us=1.5e6 must surface to the guest as 1.5e6 every
  // run, not as "now". The recorded backend therefore reports timestamps
  // already rebased to its own zero-point, and we translate to guest by
  // subtracting our own epoch.
  //
  // Live backends report host wall-clock microseconds; we subtract our
  // epoch to give the guest a session-relative timeline.
  if (host_us < host_epoch_us_) {
    return 0;
  }
  return host_us - host_epoch_us_;
}

std::string NuiManager::StatusString() const {
  if (!backend_) {
    return "kinectix: backend=<none-installed>";
  }
  return std::string("kinectix: backend=") + backend_->Name() +
         " connected=" + (backend_->IsConnected() ? "yes" : "no") +
         " caps=0x" + std::to_string(backend_->Capabilities());
}

std::unique_ptr<INuiBackend> NuiManager::CreateBackendFromCvars() {
  const std::string& kind = cvars::nui_backend;

  if (kind == "none" || kind.empty()) {
    return std::make_unique<NullNuiBackend>();
  }

  if (kind == "recorded") {
    return std::make_unique<recorded::RecordedBackend>(cvars::nui_record_path);
  }

#if XE_KINECTIX_NUI_FREENECT
  if (kind == "freenect") {
    return std::make_unique<freenect::FreenectBackend>();
  }
#endif

#if XE_KINECTIX_NUI_FREENECT2
  if (kind == "freenect2") {
    return std::make_unique<freenect::Freenect2Backend>();
  }
#endif

#if XE_KINECTIX_NUI_MEDIAPIPE
  if (kind == "mediapipe") {
    return std::make_unique<mediapipe::MediaPipeBackend>();
  }
#endif

  XELOGW(
      "Unknown or unbuilt NUI backend '%s'. Falling back to null. "
      "Did you forget a build flag (KINECTIX_NUI_FREENECT etc.)?",
      kind.c_str());
  return std::make_unique<NullNuiBackend>();
}

}  // namespace nui
}  // namespace hid
}  // namespace xe
