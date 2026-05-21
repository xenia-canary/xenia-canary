/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XAudioGetSpeakerConfig_entry(lpdword_t config_ptr) {
  kernel_state()->xconfig()->ReadSetting(
      XCONFIG_USER_CATEGORY,
      XCONFIG_USER_CATEGORY_ENTRIES::XCONFIG_USER_AUDIO_FLAGS, config_ptr);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioGetSpeakerConfig, kAudio, kImplemented);

// Structure for XAudioQueryDriverPerformance
struct XAudioDriverPerformance {
  xe::be<uint32_t> frames_submitted;
  xe::be<uint32_t> frames_processed;
  xe::be<uint32_t> frames_dropped;
  xe::be<uint32_t> latency_ms;
  xe::be<uint32_t> reserved[4];
};

dword_result_t XAudioQueryDriverPerformance_entry(
    lpunknown_t driver_ptr, pointer_t<XAudioDriverPerformance> perf_ptr) {
  if (!driver_ptr || !perf_ptr) {
    return X_E_INVALIDARG;
  }

  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  size_t client_index = driver_ptr.guest_address() & 0x0000FFFF;
  auto audio_system = kernel_state()->emulator()->audio_system();

  // Get real performance metrics from the audio system
  apu::AudioSystem::ClientPerformance perf;
  if (audio_system->GetClientPerformance(client_index, &perf)) {
    perf_ptr->frames_submitted = perf.frames_submitted;
    perf_ptr->frames_processed = perf.frames_processed;
    perf_ptr->frames_dropped = perf.frames_dropped;
  } else {
    // Client not found, return zeros
    perf_ptr->frames_submitted = 0;
    perf_ptr->frames_processed = 0;
    perf_ptr->frames_dropped = 0;
  }

  // Calculate latency based on queued frames
  // At 48kHz with 256 samples per frame for 6-channel audio:
  // Frame duration = 256 samples / 48000 Hz = ~5.33ms per frame
  // With default 8 queued frames: 8 * 5.33ms = ~42.67ms latency
  uint32_t queued_frames = perf.frames_submitted - perf.frames_processed;
  constexpr float kFrameDurationMs = 256.0f / 48000.0f * 1000.0f;  // ~5.33ms
  perf_ptr->latency_ms =
      static_cast<uint32_t>(queued_frames * kFrameDurationMs);

  for (int i = 0; i < 4; i++) {
    perf_ptr->reserved[i] = 0;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioQueryDriverPerformance, kAudio, kStub);

dword_result_t XAudioGetVoiceCategoryVolumeChangeMask_entry(
    lpunknown_t driver_ptr, lpdword_t out_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  xe::threading::NanoSleep(1000);

  // Checking these bits to see if any voice volume changed.
  // I think.
  *out_ptr = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolumeChangeMask, kAudio, kStub,
                         kHighFrequency);

dword_result_t XAudioGetVoiceCategoryVolume_entry(dword_t unk,
                                                  lpfloat_t out_ptr) {
  // Expects a floating point single. Volume %?
  *out_ptr = 1.0f;

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioGetVoiceCategoryVolume, kAudio, kStub,
                         kHighFrequency);

dword_result_t XAudioEnableDucker_entry(dword_t unk) { return X_ERROR_SUCCESS; }
DECLARE_XBOXKRNL_EXPORT1(XAudioEnableDucker, kAudio, kStub);

dword_result_t XAudioRegisterRenderDriverClient_entry(lpdword_t callback_ptr,
                                                      lpdword_t driver_ptr) {
  if (!callback_ptr) {
    return X_E_INVALIDARG;
  }

  uint32_t callback = callback_ptr[0];

  if (!callback) {
    return X_E_INVALIDARG;
  }
  uint32_t callback_arg = callback_ptr[1];

  auto audio_system = kernel_state()->emulator()->audio_system();

  size_t index;
  auto result = audio_system->RegisterClient(callback, callback_arg, &index);
  if (XFAILED(result)) {
    return result;
  }

  assert_true(!(index & ~0x0000FFFF));
  *driver_ptr = 0x41550000 | (static_cast<uint32_t>(index) & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioRegisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioUnregisterRenderDriverClient_entry(
    lpunknown_t driver_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto audio_system = kernel_state()->emulator()->audio_system();
  audio_system->UnregisterClient(driver_ptr.guest_address() & 0x0000FFFF);
  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XAudioUnregisterRenderDriverClient, kAudio,
                         kImplemented);

dword_result_t XAudioSubmitRenderDriverFrame_entry(lpunknown_t driver_ptr,
                                                   lpunknown_t samples_ptr) {
  assert_true((driver_ptr.guest_address() & 0xFFFF0000) == 0x41550000);

  auto audio_system = kernel_state()->emulator()->audio_system();
  auto samples =
      kernel_state()->memory()->TranslateVirtual<float*>(samples_ptr);
  audio_system->SubmitFrame(driver_ptr.guest_address() & 0x0000FFFF, samples);

  return X_ERROR_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(XAudioSubmitRenderDriverFrame, kAudio, kImplemented,
                         kHighFrequency);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Audio);
