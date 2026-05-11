/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_DEBUG_DIALOG_H_
#define XENIA_UI_IMGUI_DEBUG_DIALOG_H_

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <utility>

#include "xenia/ui/imgui_gamepad_dialog.h"

namespace xe {
namespace app {
class EmulatorWindow;
}  // namespace app
}  // namespace xe

namespace xe {
namespace ui {

// ImGui-based debug settings dialog with gamepad support.
class ImGuiDebugDialog : public ImGuiGamepadDialog {
 public:
  ImGuiDebugDialog(ImGuiDrawer* drawer, app::EmulatorWindow* emulator_window,
                   hid::InputSystem* input_system);

  void CloseDialog() { Close(); }

  void SetOnCloseCallback(std::function<void()> callback) {
    on_close_callback_ = std::move(callback);
  }

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  bool HasFilter() const;
  bool MatchesFilter(const char* cvar_name) const;
  bool AnyMatchesFilter(std::initializer_list<const char*> cvar_names) const;

  void LoadCurrentSettings();
  void ShowNotification(const std::string& title,
                        const std::string& description);
  void SyncTextBuffers();
  void ClearGpuCaches();

  // Setting change handlers
  void ApplyBoolSetting(const char* section, const char* cvar_name, bool value,
                        bool clear_gpu_caches = false);
  void ApplyInt32Setting(const char* section, const char* cvar_name,
                         int32_t value, bool clear_gpu_caches = false);
  void ApplyUint32Setting(const char* section, const char* cvar_name,
                          uint32_t value, bool clear_gpu_caches = false);
  void ApplyDoubleSetting(const char* section, const char* cvar_name,
                          double value, bool clear_gpu_caches = false);

  void ApplyAnisoOverride(int32_t value);
  void ApplyOQSaturation(double value);
  void ApplyOQLowerThreshold();
  void ApplyOQUpperThreshold();
  void ApplyLogLevel();
  void ApplyLogMask();
  void ApplyScribbleHeapValue();

  app::EmulatorWindow* emulator_window_;
  std::function<void()> on_close_callback_;

  // Cached settings values.
  // Common Overrides
  int32_t anisotropic_override_;
  bool gpu_allow_invalid_fetch_constants_;
  bool gpu_3d_to_2d_texture_;
  bool half_pixel_offset_;
  bool submit_on_primary_buffer_end_;
  int32_t occlusion_query_fake_lower_threshold_;
  int32_t occlusion_query_fake_upper_threshold_;
  double occlusion_query_sample_count_saturation_;
  // Presentation / Display
  bool present_letterbox_;
  // Resolution Scaling / Resolve
  bool draw_resolution_scaled_texture_offsets_;
  bool readback_resolve_half_pixel_offset_;
  bool resolve_resolution_scale_fill_half_pixel_offset_;
  // Shader / Driver Workarounds
  bool use_fuzzy_alpha_epsilon_;
  bool vulkan_precise_interpolation_;
  bool dxbc_switch_;
  // EDRAM / Draw Heuristics
  bool execute_unclipped_draw_vs_on_cpu_;
  bool execute_unclipped_draw_vs_on_cpu_for_psi_render_backend_;
  bool execute_unclipped_draw_vs_on_cpu_with_scissor_;
  bool mrt_edram_used_range_clamp_to_min_;
  // Depth / Precision
  bool depth_float24_convert_in_pixel_shader_;
  bool depth_float24_round_;
  bool depth_transfer_not_equal_test_;
  // Primitive Conversion
  bool force_convert_triangle_strips_to_lists_;
  bool force_convert_triangle_fans_to_lists_;
  bool force_convert_quad_lists_to_triangle_lists_;
  bool force_convert_line_loops_to_strips_;
  // Memory / Boot Hacks
  bool scribble_heap_;
  int32_t scribble_heap_value_;
  // Diagnostics / Logging
  int32_t log_level_;
  uint32_t log_mask_;
  bool occlusion_query_log_;
  bool gpu_debug_markers_;
  bool disassemble_pm4_;
  bool log_guest_driven_gpu_register_written_values_;
  bool log_ringbuffer_kickoff_initiator_bts_;

  char filter_buffer_[64] = {};
  char occlusion_query_fake_lower_threshold_buffer_[32] = {};
  char occlusion_query_fake_upper_threshold_buffer_[32] = {};
  char log_level_buffer_[32] = {};
  char log_mask_buffer_[32] = {};
  char scribble_heap_value_buffer_[32] = {};
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_DEBUG_DIALOG_H_
