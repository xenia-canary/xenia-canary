/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_debug_dialog.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>

#include "third_party/imgui/imgui.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/config.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/imgui_host_notification.h"

DECLARE_int32(anisotropic_override);
DECLARE_bool(gpu_allow_invalid_fetch_constants);
DECLARE_bool(gpu_3d_to_2d_texture);
DECLARE_bool(half_pixel_offset);
DECLARE_bool(submit_on_primary_buffer_end);
DECLARE_int32(occlusion_query_fake_lower_threshold);
DECLARE_int32(occlusion_query_fake_upper_threshold);
DECLARE_double(occlusion_query_sample_count_saturation);
DECLARE_string(occlusion_query);
DECLARE_bool(present_letterbox);
DECLARE_bool(draw_resolution_scaled_texture_offsets);
DECLARE_bool(readback_resolve_half_pixel_offset);
DECLARE_bool(resolve_resolution_scale_fill_half_pixel_offset);
DECLARE_bool(use_fuzzy_alpha_epsilon);
DECLARE_bool(vulkan_precise_interpolation);
DECLARE_bool(dxbc_switch);
DECLARE_bool(execute_unclipped_draw_vs_on_cpu);
DECLARE_bool(execute_unclipped_draw_vs_on_cpu_for_psi_render_backend);
DECLARE_bool(execute_unclipped_draw_vs_on_cpu_with_scissor);
DECLARE_bool(mrt_edram_used_range_clamp_to_min);
DECLARE_bool(depth_float24_convert_in_pixel_shader);
DECLARE_bool(depth_float24_round);
DECLARE_bool(depth_transfer_not_equal_test);
DECLARE_bool(force_convert_triangle_strips_to_lists);
DECLARE_bool(force_convert_triangle_fans_to_lists);
DECLARE_bool(force_convert_quad_lists_to_triangle_lists);
DECLARE_bool(force_convert_line_loops_to_strips);
DECLARE_int32(log_level);
DECLARE_uint32(log_mask);
DECLARE_bool(scribble_heap);
DECLARE_int32(scribble_heap_value);
DECLARE_bool(occlusion_query_log);
DECLARE_bool(gpu_debug_markers);
DECLARE_bool(disassemble_pm4);
DECLARE_bool(log_guest_driven_gpu_register_written_values);
DECLARE_bool(log_ringbuffer_kickoff_initiator_bts);

namespace xe {
namespace ui {

namespace {

std::string g_debug_settings_filter;

constexpr float kFilterWidth = 250.0f;
constexpr float kTextInputWidth = 80.0f;
constexpr float kComboWidth = kTextInputWidth;
constexpr float kControlColumnWidth = 90.0f;

void FormatDouble(char* buffer, size_t buffer_size, double value) {
  std::snprintf(buffer, buffer_size, "%.8f", value);

  size_t length = std::strlen(buffer);
  while (length > 0 && buffer[length - 1] == '0') {
    buffer[--length] = '\0';
  }
  if (length > 0 && buffer[length - 1] == '.') {
    buffer[--length] = '\0';
  }
  if (length == 0) {
    std::snprintf(buffer, buffer_size, "0");
  }
}

void FormatInt32(char* buffer, size_t buffer_size, int32_t value) {
  std::snprintf(buffer, buffer_size, "%d", value);
}

void FormatUint32(char* buffer, size_t buffer_size, uint32_t value) {
  std::snprintf(buffer, buffer_size, "%u", value);
}

bool ParseInt32(const char* text, int32_t* value_out) {
  char* end = nullptr;
  errno = 0;
  long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE ||
      value < std::numeric_limits<int32_t>::min() ||
      value > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  *value_out = static_cast<int32_t>(value);
  return true;
}

bool ParseUint32(const char* text, uint32_t* value_out) {
  char* end = nullptr;
  errno = 0;
  unsigned long value = std::strtoul(text, &end, 0);
  if (end == text || *end != '\0' || errno == ERANGE ||
      value > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *value_out = static_cast<uint32_t>(value);
  return true;
}

template <typename T>
bool OverrideCvarByName(const char* name, T value) {
  if (!cvar::ConfigVars) {
    return false;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return false;
  }
  auto* config_var = dynamic_cast<cvar::ConfigVar<T>*>(it->second);
  if (!config_var) {
    return false;
  }
  config_var->SetGameConfigValue(value);
  return true;
}

void RightAlignNextItem(float width) {
  float cursor_x = ImGui::GetCursorPosX();
  float available_width = ImGui::GetContentRegionAvail().x;
  if (available_width > width) {
    ImGui::SetCursorPosX(cursor_x + available_width - width);
  }
  ImGui::SetNextItemWidth(width);
}

bool RightAlignedCheckbox(const char* id, bool* value) {
  float width = ImGui::GetFrameHeight();
  float cursor_x = ImGui::GetCursorPosX();
  float available_width = ImGui::GetContentRegionAvail().x;
  if (available_width > width) {
    ImGui::SetCursorPosX(cursor_x + available_width - width);
  }
  return ImGui::Checkbox(id, value);
}

bool BeginSettingsTable(const char* id) {
  if (!ImGui::BeginTable(id, 2,
                         ImGuiTableFlags_SizingStretchProp |
                             ImGuiTableFlags_NoSavedSettings)) {
    return false;
  }
  ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed,
                          kControlColumnWidth);
  return true;
}

void DrawLabelCell(const char* label, const char* suffix = nullptr) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (suffix != nullptr && suffix[0] != '\0') {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", suffix);
  }
}

struct BackendState {
  std::string label = "Unknown";
  bool d3d12 = false;
  bool metal = false;
  bool vulkan = false;
};

BackendState GetBackendState(app::EmulatorWindow* emulator_window) {
  BackendState state;

  if (emulator_window != nullptr) {
    auto emulator = emulator_window->emulator();
    if (emulator != nullptr) {
      auto graphics_system = emulator->graphics_system();
      if (graphics_system != nullptr) {
        state.label = graphics_system->name();
        if (state.label == "D3D12") {
          state.d3d12 = true;
        } else if (state.label == "Vulkan") {
          state.vulkan = true;
        } else if (state.label == "Metal") {
          state.metal = true;
        }
      }
    }
  }

  return state;
}

bool BeginSection(const char* title, bool default_open, bool filter_active) {
  ImGuiTreeNodeFlags flags = filter_active ? ImGuiTreeNodeFlags_DefaultOpen : 0;
  if (default_open) {
    flags |= ImGuiTreeNodeFlags_DefaultOpen;
  }
  return ImGui::CollapsingHeader(title, flags);
}

}  // namespace

ImGuiDebugDialog::ImGuiDebugDialog(ImGuiDrawer* drawer,
                                   app::EmulatorWindow* emulator_window,
                                   hid::InputSystem* input_system)
    : ImGuiGamepadDialog(drawer, input_system),
      emulator_window_(emulator_window) {
  LoadCurrentSettings();
  std::snprintf(filter_buffer_, sizeof(filter_buffer_), "%s",
                g_debug_settings_filter.c_str());
}

void ImGuiDebugDialog::OnClose() {
  if (on_close_callback_) {
    on_close_callback_();
  }
}

bool ImGuiDebugDialog::HasFilter() const { return filter_buffer_[0] != '\0'; }

bool ImGuiDebugDialog::MatchesFilter(const char* cvar_name) const {
  if (!HasFilter()) {
    return true;
  }
  return strstr(cvar_name, filter_buffer_) != nullptr;
}

bool ImGuiDebugDialog::AnyMatchesFilter(
    std::initializer_list<const char*> cvar_names) const {
  if (!HasFilter()) {
    return true;
  }
  for (const char* cvar_name : cvar_names) {
    if (MatchesFilter(cvar_name)) {
      return true;
    }
  }
  return false;
}

void ImGuiDebugDialog::ShowNotification(const std::string& title,
                                        const std::string& description) {
  // Position 10 = RIGHT-BOTTOM (default for HostNotificationWindow)
  new HostNotificationWindow(imgui_drawer(), title, description, 0);
}

void ImGuiDebugDialog::SyncTextBuffers() {
  FormatInt32(occlusion_query_fake_lower_threshold_buffer_,
              sizeof(occlusion_query_fake_lower_threshold_buffer_),
              occlusion_query_fake_lower_threshold_);
  FormatInt32(occlusion_query_fake_upper_threshold_buffer_,
              sizeof(occlusion_query_fake_upper_threshold_buffer_),
              occlusion_query_fake_upper_threshold_);
  FormatInt32(log_level_buffer_, sizeof(log_level_buffer_), log_level_);
  FormatUint32(log_mask_buffer_, sizeof(log_mask_buffer_), log_mask_);
  FormatInt32(scribble_heap_value_buffer_, sizeof(scribble_heap_value_buffer_),
              scribble_heap_value_);
}

void ImGuiDebugDialog::LoadCurrentSettings() {
  anisotropic_override_ = cvars::anisotropic_override;
  gpu_allow_invalid_fetch_constants_ = cvars::gpu_allow_invalid_fetch_constants;
  gpu_3d_to_2d_texture_ = cvars::gpu_3d_to_2d_texture;
  half_pixel_offset_ = cvars::half_pixel_offset;
  submit_on_primary_buffer_end_ = cvars::submit_on_primary_buffer_end;
  occlusion_query_fake_lower_threshold_ =
      cvars::occlusion_query_fake_lower_threshold;
  occlusion_query_fake_upper_threshold_ =
      cvars::occlusion_query_fake_upper_threshold;
  occlusion_query_sample_count_saturation_ =
      cvars::occlusion_query_sample_count_saturation;

  present_letterbox_ = cvars::present_letterbox;

  draw_resolution_scaled_texture_offsets_ =
      cvars::draw_resolution_scaled_texture_offsets;
  readback_resolve_half_pixel_offset_ =
      cvars::readback_resolve_half_pixel_offset;
  resolve_resolution_scale_fill_half_pixel_offset_ =
      cvars::resolve_resolution_scale_fill_half_pixel_offset;

  use_fuzzy_alpha_epsilon_ = cvars::use_fuzzy_alpha_epsilon;
  vulkan_precise_interpolation_ = cvars::vulkan_precise_interpolation;
  dxbc_switch_ = cvars::dxbc_switch;

  execute_unclipped_draw_vs_on_cpu_ = cvars::execute_unclipped_draw_vs_on_cpu;
  execute_unclipped_draw_vs_on_cpu_for_psi_render_backend_ =
      cvars::execute_unclipped_draw_vs_on_cpu_for_psi_render_backend;
  execute_unclipped_draw_vs_on_cpu_with_scissor_ =
      cvars::execute_unclipped_draw_vs_on_cpu_with_scissor;
  mrt_edram_used_range_clamp_to_min_ = cvars::mrt_edram_used_range_clamp_to_min;

  depth_float24_convert_in_pixel_shader_ =
      cvars::depth_float24_convert_in_pixel_shader;
  depth_float24_round_ = cvars::depth_float24_round;
  depth_transfer_not_equal_test_ = cvars::depth_transfer_not_equal_test;

  force_convert_triangle_strips_to_lists_ =
      cvars::force_convert_triangle_strips_to_lists;
  force_convert_triangle_fans_to_lists_ =
      cvars::force_convert_triangle_fans_to_lists;
  force_convert_quad_lists_to_triangle_lists_ =
      cvars::force_convert_quad_lists_to_triangle_lists;
  force_convert_line_loops_to_strips_ =
      cvars::force_convert_line_loops_to_strips;

  scribble_heap_ = cvars::scribble_heap;
  scribble_heap_value_ = cvars::scribble_heap_value;

  log_level_ = cvars::log_level;
  log_mask_ = cvars::log_mask;
  occlusion_query_log_ = cvars::occlusion_query_log;
  gpu_debug_markers_ = cvars::gpu_debug_markers;
  disassemble_pm4_ = cvars::disassemble_pm4;
  log_guest_driven_gpu_register_written_values_ =
      cvars::log_guest_driven_gpu_register_written_values;
  log_ringbuffer_kickoff_initiator_bts_ =
      cvars::log_ringbuffer_kickoff_initiator_bts;

  SyncTextBuffers();
}

void ImGuiDebugDialog::ClearGpuCaches() {
  Emulator* emulator =
      emulator_window_ != nullptr ? emulator_window_->emulator() : nullptr;
  if (emulator == nullptr) {
    return;
  }
  gpu::GraphicsSystem* graphics_system = emulator->graphics_system();
  if (graphics_system == nullptr) {
    return;
  }
  graphics_system->ClearCaches();
}

void ImGuiDebugDialog::ApplyBoolSetting(const char* section,
                                        const char* cvar_name, bool value,
                                        bool clear_gpu_caches) {
  OverrideCvarByName<bool>(cvar_name, value);
  Emulator* emulator =
      emulator_window_ ? emulator_window_->emulator() : nullptr;
  config::SaveGameConfigSetting(emulator, section, cvar_name, value);
  if (clear_gpu_caches) {
    ClearGpuCaches();
  }
  ShowNotification(cvar_name, value ? "Enabled" : "Disabled");
}

void ImGuiDebugDialog::ApplyInt32Setting(const char* section,
                                         const char* cvar_name, int32_t value,
                                         bool clear_gpu_caches) {
  OverrideCvarByName<int32_t>(cvar_name, value);
  Emulator* emulator =
      emulator_window_ ? emulator_window_->emulator() : nullptr;
  config::SaveGameConfigSetting(emulator, section, cvar_name, value);
  if (clear_gpu_caches) {
    ClearGpuCaches();
  }
  char value_buffer[32];
  FormatInt32(value_buffer, sizeof(value_buffer), value);
  ShowNotification(cvar_name, value_buffer);
}

void ImGuiDebugDialog::ApplyUint32Setting(const char* section,
                                          const char* cvar_name, uint32_t value,
                                          bool clear_gpu_caches) {
  OverrideCvarByName<uint32_t>(cvar_name, value);
  Emulator* emulator =
      emulator_window_ ? emulator_window_->emulator() : nullptr;
  config::SaveGameConfigSetting(emulator, section, cvar_name, value);
  if (clear_gpu_caches) {
    ClearGpuCaches();
  }
  char value_buffer[32];
  FormatUint32(value_buffer, sizeof(value_buffer), value);
  ShowNotification(cvar_name, value_buffer);
}

void ImGuiDebugDialog::ApplyDoubleSetting(const char* section,
                                          const char* cvar_name, double value,
                                          bool clear_gpu_caches) {
  OverrideCvarByName<double>(cvar_name, value);
  Emulator* emulator =
      emulator_window_ ? emulator_window_->emulator() : nullptr;
  config::SaveGameConfigSetting(emulator, section, cvar_name, value);
  if (clear_gpu_caches) {
    ClearGpuCaches();
  }
  char value_buffer[32];
  FormatDouble(value_buffer, sizeof(value_buffer), value);
  ShowNotification(cvar_name, value_buffer);
}

void ImGuiDebugDialog::ApplyAnisoOverride(int32_t value) {
  if (value < -1 || value > 5 || value == anisotropic_override_) {
    return;
  }

  anisotropic_override_ = value;
  ApplyInt32Setting("GPU", "anisotropic_override", value, true);
}

void ImGuiDebugDialog::ApplyOQSaturation(double value) {
  if (value < 0.0) {
    value = 0.0;
  } else if (value > 1.0) {
    value = 1.0;
  }

  if (value == occlusion_query_sample_count_saturation_) {
    return;
  }

  occlusion_query_sample_count_saturation_ = value;
  ApplyDoubleSetting("GPU", "occlusion_query_sample_count_saturation", value);
}

void ImGuiDebugDialog::ApplyOQLowerThreshold() {
  int32_t value = 0;
  if (!ParseInt32(occlusion_query_fake_lower_threshold_buffer_, &value)) {
    SyncTextBuffers();
    return;
  }

  if (value < -1) {
    value = -1;
  }

  bool upper_threshold_changed = false;
  if (value != -1 && occlusion_query_fake_upper_threshold_ < value) {
    occlusion_query_fake_upper_threshold_ = value;
    upper_threshold_changed = true;
  }

  if (value == occlusion_query_fake_lower_threshold_ &&
      !upper_threshold_changed) {
    SyncTextBuffers();
    return;
  }

  occlusion_query_fake_lower_threshold_ = value;
  ApplyInt32Setting("GPU", "occlusion_query_fake_lower_threshold", value);
  if (upper_threshold_changed) {
    ApplyInt32Setting("GPU", "occlusion_query_fake_upper_threshold",
                      occlusion_query_fake_upper_threshold_);
  }
  SyncTextBuffers();
}

void ImGuiDebugDialog::ApplyOQUpperThreshold() {
  int32_t value = 0;
  if (!ParseInt32(occlusion_query_fake_upper_threshold_buffer_, &value)) {
    SyncTextBuffers();
    return;
  }

  if (value < 0) {
    value = 0;
  }
  if (occlusion_query_fake_lower_threshold_ != -1 &&
      value < occlusion_query_fake_lower_threshold_) {
    value = occlusion_query_fake_lower_threshold_;
  }

  if (value == occlusion_query_fake_upper_threshold_) {
    SyncTextBuffers();
    return;
  }

  occlusion_query_fake_upper_threshold_ = value;
  ApplyInt32Setting("GPU", "occlusion_query_fake_upper_threshold", value);
  SyncTextBuffers();
}

void ImGuiDebugDialog::ApplyLogLevel() {
  int32_t value = 0;
  if (!ParseInt32(log_level_buffer_, &value)) {
    SyncTextBuffers();
    return;
  }

  if (value < 0) {
    value = 0;
  } else if (value > 3) {
    value = 3;
  }

  if (value == log_level_) {
    SyncTextBuffers();
    return;
  }

  log_level_ = value;
  ApplyInt32Setting("Logging", "log_level", value);
  SyncTextBuffers();
}

void ImGuiDebugDialog::ApplyLogMask() {
  uint32_t value = 0;
  if (!ParseUint32(log_mask_buffer_, &value)) {
    SyncTextBuffers();
    return;
  }

  if (value == log_mask_) {
    SyncTextBuffers();
    return;
  }

  log_mask_ = value;
  ApplyUint32Setting("Logging", "log_mask", value);
  SyncTextBuffers();
}

void ImGuiDebugDialog::ApplyScribbleHeapValue() {
  int32_t value = 0;
  if (!ParseInt32(scribble_heap_value_buffer_, &value)) {
    SyncTextBuffers();
    return;
  }

  if (value < 0) {
    value = 0;
  } else if (value > 255) {
    value = 255;
  }

  if (value == scribble_heap_value_) {
    SyncTextBuffers();
    return;
  }

  scribble_heap_value_ = value;
  ApplyInt32Setting("Memory", "scribble_heap_value", value);
  SyncTextBuffers();
}

void ImGuiDebugDialog::OnDraw(ImGuiIO& io) {
  // Style - white background, black text, Xbox green accents
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

  // Center on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(450.0f, 500.0f), ImGuiCond_FirstUseEver);

  const char* anisotropic_labels[] = {"Auto (-1)", "Off (0)", "1x", "2x",
                                      "4x",        "8x",      "16x"};
  int anisotropic_combo_index = anisotropic_override_ + 1;

  BackendState backend_state = GetBackendState(emulator_window_);

  // CPU JIT tracing toggles reflect runtime backend state (not cvars) and only
  // appear for trace modes compiled into the active backend
  // (XENIA_ENABLE_ITRACE / XENIA_ENABLE_DTRACE build options).
  Emulator* cpu_emulator =
      emulator_window_ ? emulator_window_->emulator() : nullptr;
  cpu::Processor* cpu_processor =
      cpu_emulator ? cpu_emulator->processor() : nullptr;
  cpu::backend::Backend* cpu_backend =
      cpu_processor ? cpu_processor->backend() : nullptr;
  bool trace_instr_available =
      cpu_backend != nullptr && cpu_backend->trace_instr_available();
  bool trace_data_available =
      cpu_backend != nullptr && cpu_backend->trace_data_available();
  bool trace_func_available =
      cpu_backend != nullptr && cpu_backend->trace_func_available();

  bool is_fake_occlusion_query = cvars::occlusion_query == "fake";
  bool filter_active = HasFilter();
  bool is_release_build = true;
#if !defined(NDEBUG)
  is_release_build = false;
#endif

  bool show_common = AnyMatchesFilter({
      "anisotropic_override",
      "gpu_allow_invalid_fetch_constants",
      "gpu_3d_to_2d_texture",
      "half_pixel_offset",
      "submit_on_primary_buffer_end",
      "occlusion_query_fake_lower_threshold",
      "occlusion_query_fake_upper_threshold",
      "occlusion_query_sample_count_saturation",
  });
  bool show_display = AnyMatchesFilter({"present_letterbox"});
  bool show_scaling = AnyMatchesFilter({
      "draw_resolution_scaled_texture_offsets",
      "readback_resolve_half_pixel_offset",
      "resolve_resolution_scale_fill_half_pixel_offset",
  });
  bool show_shader = AnyMatchesFilter({
      "use_fuzzy_alpha_epsilon",
      "vulkan_precise_interpolation",
      "dxbc_switch",
  });
  bool show_edram = AnyMatchesFilter({
      "execute_unclipped_draw_vs_on_cpu",
      "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
      "execute_unclipped_draw_vs_on_cpu_with_scissor",
      "mrt_edram_used_range_clamp_to_min",
  });
  bool show_memory = AnyMatchesFilter({"scribble_heap", "scribble_heap_value"});
  bool show_depth = AnyMatchesFilter({"depth_float24_convert_in_pixel_shader",
                                      "depth_float24_round",
                                      "depth_transfer_not_equal_test"});

  bool show_conversion =
      AnyMatchesFilter({"force_convert_triangle_strips_to_lists",
                        "force_convert_triangle_fans_to_lists",
                        "force_convert_quad_lists_to_triangle_lists",
                        "force_convert_line_loops_to_strips"});
  bool show_logging = AnyMatchesFilter({
      "log_level",
      "log_mask",
      "occlusion_query_log",
      "gpu_debug_markers",
      "disassemble_pm4",
      "log_guest_driven_gpu_register_written_values",
      "log_ringbuffer_kickoff_initiator_bts",
      "capture_gpu_trace",
  });

  bool show_cpu_tracing =
      cpu_backend != nullptr &&
      AnyMatchesFilter({"trace_instructions", "trace_data", "trace_functions"});

  bool any_visible = show_common || show_display || show_scaling ||
                     show_shader || show_edram || show_memory || show_depth ||
                     show_conversion || show_logging || show_cpu_tracing;

  bool is_open = true;
  if (ImGui::Begin("Debug Settings", &is_open, ImGuiWindowFlags_NoCollapse)) {
    // Handle keyboard escape, F8, or gamepad B/Back
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_F8) || ShouldCloseFromGamepad()) {
      Close();
    }

    ImGui::TextUnformatted("Filter:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kFilterWidth);
    if (ImGui::InputText("##debug_settings_filter", filter_buffer_,
                         sizeof(filter_buffer_))) {
      g_debug_settings_filter = filter_buffer_;
    }
    ImGui::Text("Backend: %s", backend_state.label.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginChild("##debug_settings_scroll", ImVec2(0.0f, 0.0f),
                          false)) {
      if (show_common &&
          BeginSection("Common Overrides", true, filter_active)) {
        if (BeginSettingsTable("##debug_common_overrides")) {
          if (MatchesFilter("anisotropic_override")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("anisotropic_override");
            ImGui::TableSetColumnIndex(1);
            RightAlignNextItem(kComboWidth);
            ImGui::PushStyleColor(ImGuiCol_PopupBg,
                                  ImVec4(0.94f, 0.94f, 0.94f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(0.78f, 0.88f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                  ImVec4(0.68f, 0.82f, 0.68f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                  ImVec4(0.58f, 0.76f, 0.58f, 1.0f));
            if (ImGui::Combo("##anisotropic_override", &anisotropic_combo_index,
                             anisotropic_labels, 7)) {
              ApplyAnisoOverride(anisotropic_combo_index - 1);
            }
            ImGui::PopStyleColor(4);
          }

          if (MatchesFilter("gpu_allow_invalid_fetch_constants")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("gpu_allow_invalid_fetch_constants");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##gpu_allow_invalid_fetch_constants",
                                     &gpu_allow_invalid_fetch_constants_)) {
              ApplyBoolSetting("GPU", "gpu_allow_invalid_fetch_constants",
                               gpu_allow_invalid_fetch_constants_);
            }
          }

          if (MatchesFilter("gpu_3d_to_2d_texture")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("gpu_3d_to_2d_texture");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##gpu_3d_to_2d_texture",
                                     &gpu_3d_to_2d_texture_)) {
              ApplyBoolSetting("GPU", "gpu_3d_to_2d_texture",
                               gpu_3d_to_2d_texture_, true);
            }
          }

          if (MatchesFilter("half_pixel_offset")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("half_pixel_offset");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##half_pixel_offset",
                                     &half_pixel_offset_)) {
              ApplyBoolSetting("GPU", "half_pixel_offset", half_pixel_offset_);
            }
          }

          if (MatchesFilter("submit_on_primary_buffer_end")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("submit_on_primary_buffer_end");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##submit_on_primary_buffer_end",
                                     &submit_on_primary_buffer_end_)) {
              ApplyBoolSetting("GPU", "submit_on_primary_buffer_end",
                               submit_on_primary_buffer_end_);
            }
          }

          if (MatchesFilter("occlusion_query_fake_lower_threshold")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("occlusion_query_fake_lower_threshold",
                          "[fake only]");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!is_fake_occlusion_query);
            RightAlignNextItem(kTextInputWidth);
            bool committed = ImGui::InputText(
                "##occlusion_query_fake_lower_threshold",
                occlusion_query_fake_lower_threshold_buffer_,
                sizeof(occlusion_query_fake_lower_threshold_buffer_),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed) {
              ApplyOQLowerThreshold();
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
              ApplyOQLowerThreshold();
            }
            ImGui::EndDisabled();
          }

          if (MatchesFilter("occlusion_query_fake_upper_threshold")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("occlusion_query_fake_upper_threshold",
                          "[fake only]");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(!is_fake_occlusion_query);
            RightAlignNextItem(kTextInputWidth);
            bool committed = ImGui::InputText(
                "##occlusion_query_fake_upper_threshold",
                occlusion_query_fake_upper_threshold_buffer_,
                sizeof(occlusion_query_fake_upper_threshold_buffer_),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed) {
              ApplyOQUpperThreshold();
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
              ApplyOQUpperThreshold();
            }
            ImGui::EndDisabled();
          }

          if (MatchesFilter("occlusion_query_sample_count_saturation")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("occlusion_query_sample_count_saturation");
            ImGui::TableSetColumnIndex(1);
            float slider_value =
                static_cast<float>(occlusion_query_sample_count_saturation_);
            RightAlignNextItem(kTextInputWidth);
            if (ImGui::SliderFloat("##occlusion_query_sample_count_saturation",
                                   &slider_value, 0.0f, 1.0f, "%.2f",
                                   ImGuiSliderFlags_AlwaysClamp)) {
              ApplyOQSaturation(slider_value);
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_display &&
          BeginSection("Presentation / Display", false, filter_active)) {
        if (BeginSettingsTable("##debug_presentation")) {
          if (MatchesFilter("present_letterbox")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("present_letterbox");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##present_letterbox",
                                     &present_letterbox_)) {
              ApplyBoolSetting("Display", "present_letterbox",
                               present_letterbox_);
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_scaling &&
          BeginSection("Resolution Scaling / Resolve", false, filter_active)) {
        if (BeginSettingsTable("##debug_resolution_scaling")) {
          if (MatchesFilter("draw_resolution_scaled_texture_offsets")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("draw_resolution_scaled_texture_offsets");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##draw_resolution_scaled_texture_offsets",
                    &draw_resolution_scaled_texture_offsets_)) {
              ApplyBoolSetting("GPU", "draw_resolution_scaled_texture_offsets",
                               draw_resolution_scaled_texture_offsets_, true);
            }
          }

          if (MatchesFilter("readback_resolve_half_pixel_offset")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("readback_resolve_half_pixel_offset");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##readback_resolve_half_pixel_offset",
                                     &readback_resolve_half_pixel_offset_)) {
              ApplyBoolSetting("GPU", "readback_resolve_half_pixel_offset",
                               readback_resolve_half_pixel_offset_);
            }
          }

          if (MatchesFilter(
                  "resolve_resolution_scale_fill_half_pixel_offset")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("resolve_resolution_scale_fill_half_pixel_offset");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##resolve_resolution_scale_fill_half_pixel_offset",
                    &resolve_resolution_scale_fill_half_pixel_offset_)) {
              ApplyBoolSetting(
                  "GPU", "resolve_resolution_scale_fill_half_pixel_offset",
                  resolve_resolution_scale_fill_half_pixel_offset_);
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_shader &&
          BeginSection("Shader / Driver Workarounds", false, filter_active)) {
        if (BeginSettingsTable("##debug_shader_driver")) {
          if (MatchesFilter("use_fuzzy_alpha_epsilon")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("use_fuzzy_alpha_epsilon");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##use_fuzzy_alpha_epsilon",
                                     &use_fuzzy_alpha_epsilon_)) {
              ApplyBoolSetting("GPU", "use_fuzzy_alpha_epsilon",
                               use_fuzzy_alpha_epsilon_, true);
            }
          }

          if (MatchesFilter("vulkan_precise_interpolation")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!backend_state.vulkan);
            DrawLabelCell("vulkan_precise_interpolation", "[Vulkan]");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##vulkan_precise_interpolation",
                                     &vulkan_precise_interpolation_)) {
              ApplyBoolSetting("GPU", "vulkan_precise_interpolation",
                               vulkan_precise_interpolation_, true);
            }
            ImGui::EndDisabled();
          }

          if (MatchesFilter("dxbc_switch")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!backend_state.d3d12);
            DrawLabelCell("dxbc_switch", "[D3D12]");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##dxbc_switch", &dxbc_switch_)) {
              ApplyBoolSetting("GPU", "dxbc_switch", dxbc_switch_, true);
            }
            ImGui::EndDisabled();
          }

          ImGui::EndTable();
        }
      }

      if (show_edram &&
          BeginSection("EDRAM / Draw Heuristics", false, filter_active)) {
        if (BeginSettingsTable("##debug_edram_heuristics")) {
          if (MatchesFilter("execute_unclipped_draw_vs_on_cpu")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("execute_unclipped_draw_vs_on_cpu");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##execute_unclipped_draw_vs_on_cpu",
                                     &execute_unclipped_draw_vs_on_cpu_)) {
              ApplyBoolSetting("GPU", "execute_unclipped_draw_vs_on_cpu",
                               execute_unclipped_draw_vs_on_cpu_);
            }
          }

          if (MatchesFilter(
                  "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell(
                "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
                    &execute_unclipped_draw_vs_on_cpu_for_psi_render_backend_)) {
              ApplyBoolSetting(
                  "GPU",
                  "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend",
                  execute_unclipped_draw_vs_on_cpu_for_psi_render_backend_);
            }
          }

          if (MatchesFilter("execute_unclipped_draw_vs_on_cpu_with_scissor")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("execute_unclipped_draw_vs_on_cpu_with_scissor");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##execute_unclipped_draw_vs_on_cpu_with_scissor",
                    &execute_unclipped_draw_vs_on_cpu_with_scissor_)) {
              ApplyBoolSetting("GPU",
                               "execute_unclipped_draw_vs_on_cpu_with_scissor",
                               execute_unclipped_draw_vs_on_cpu_with_scissor_);
            }
          }

          if (MatchesFilter("mrt_edram_used_range_clamp_to_min")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("mrt_edram_used_range_clamp_to_min");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##mrt_edram_used_range_clamp_to_min",
                                     &mrt_edram_used_range_clamp_to_min_)) {
              ApplyBoolSetting("GPU", "mrt_edram_used_range_clamp_to_min",
                               mrt_edram_used_range_clamp_to_min_);
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_memory &&
          BeginSection("Memory / Boot Hacks", false, filter_active)) {
        if (BeginSettingsTable("##debug_memory_boot")) {
          if (MatchesFilter("scribble_heap")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("scribble_heap");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##scribble_heap", &scribble_heap_)) {
              ApplyBoolSetting("Memory", "scribble_heap", scribble_heap_);
            }
          }

          if (MatchesFilter("scribble_heap_value")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("scribble_heap_value");
            ImGui::TableSetColumnIndex(1);
            RightAlignNextItem(kTextInputWidth);
            bool committed = ImGui::InputText(
                "##scribble_heap_value", scribble_heap_value_buffer_,
                sizeof(scribble_heap_value_buffer_),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed) {
              ApplyScribbleHeapValue();
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
              ApplyScribbleHeapValue();
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_depth &&
          BeginSection("Depth / Precision", false, filter_active)) {
        if (BeginSettingsTable("##debug_depth_precision")) {
          if (MatchesFilter("depth_float24_convert_in_pixel_shader")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("depth_float24_convert_in_pixel_shader");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##depth_float24_convert_in_pixel_shader",
                                     &depth_float24_convert_in_pixel_shader_)) {
              ApplyBoolSetting("GPU", "depth_float24_convert_in_pixel_shader",
                               depth_float24_convert_in_pixel_shader_, true);
            }
          }

          if (MatchesFilter("depth_float24_round")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("depth_float24_round");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##depth_float24_round",
                                     &depth_float24_round_)) {
              ApplyBoolSetting("GPU", "depth_float24_round",
                               depth_float24_round_, true);
            }
          }

          if (MatchesFilter("depth_transfer_not_equal_test")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("depth_transfer_not_equal_test");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##depth_transfer_not_equal_test",
                                     &depth_transfer_not_equal_test_)) {
              ApplyBoolSetting("GPU", "depth_transfer_not_equal_test",
                               depth_transfer_not_equal_test_, true);
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_conversion &&
          BeginSection("Primitive Conversion", false, filter_active)) {
        if (BeginSettingsTable("##debug_primitive_conversion")) {
          if (MatchesFilter("force_convert_triangle_strips_to_lists")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("force_convert_triangle_strips_to_lists");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##force_convert_triangle_strips_to_lists",
                    &force_convert_triangle_strips_to_lists_)) {
              ApplyBoolSetting("GPU", "force_convert_triangle_strips_to_lists",
                               force_convert_triangle_strips_to_lists_);
            }
          }

          if (MatchesFilter("force_convert_triangle_fans_to_lists")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("force_convert_triangle_fans_to_lists");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##force_convert_triangle_fans_to_lists",
                                     &force_convert_triangle_fans_to_lists_)) {
              ApplyBoolSetting("GPU", "force_convert_triangle_fans_to_lists",
                               force_convert_triangle_fans_to_lists_);
            }
          }

          if (MatchesFilter("force_convert_quad_lists_to_triangle_lists")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("force_convert_quad_lists_to_triangle_lists");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##force_convert_quad_lists_to_triangle_lists",
                    &force_convert_quad_lists_to_triangle_lists_)) {
              ApplyBoolSetting("GPU",
                               "force_convert_quad_lists_to_triangle_lists",
                               force_convert_quad_lists_to_triangle_lists_);
            }
          }

          if (MatchesFilter("force_convert_line_loops_to_strips")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("force_convert_line_loops_to_strips");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##force_convert_line_loops_to_strips",
                                     &force_convert_line_loops_to_strips_)) {
              ApplyBoolSetting("GPU", "force_convert_line_loops_to_strips",
                               force_convert_line_loops_to_strips_);
            }
          }

          ImGui::EndTable();
        }
      }

      if (show_cpu_tracing &&
          BeginSection("CPU Tracing", false, filter_active)) {
        if (BeginSettingsTable("##debug_cpu_tracing")) {
          // Unavailable modes are shown greyed out: availability reflects how
          // the backend was built (XENIA_ENABLE_ITRACE/DTRACE/FTRACE).
          if (MatchesFilter("trace_instructions")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!trace_instr_available);
            DrawLabelCell("trace_instructions",
                          trace_instr_available
                              ? "Log each guest instruction"
                              : "[requires --enable-itrace build]");
            ImGui::TableSetColumnIndex(1);
            bool enabled =
                trace_instr_available && cpu_backend->trace_instr_enabled();
            if (RightAlignedCheckbox("##trace_instructions", &enabled) &&
                trace_instr_available) {
              cpu_backend->set_trace_instr_enabled(enabled);
              ShowNotification("trace_instructions",
                               enabled ? "Enabled" : "Disabled");
            }
            ImGui::EndDisabled();
          }
          if (MatchesFilter("trace_data")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!trace_data_available);
            DrawLabelCell("trace_data",
                          trace_data_available
                              ? "Log each guest memory/context load and store"
                              : "[requires --enable-dtrace build]");
            ImGui::TableSetColumnIndex(1);
            bool enabled =
                trace_data_available && cpu_backend->trace_data_enabled();
            if (RightAlignedCheckbox("##trace_data", &enabled) &&
                trace_data_available) {
              cpu_backend->set_trace_data_enabled(enabled);
              ShowNotification("trace_data", enabled ? "Enabled" : "Disabled");
            }
            ImGui::EndDisabled();
          }
          if (MatchesFilter("trace_functions")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(!trace_func_available);
            DrawLabelCell("trace_functions",
                          trace_func_available
                              ? "Log each guest function call"
                              : "[requires --enable-ftrace build]");
            ImGui::TableSetColumnIndex(1);
            bool enabled =
                trace_func_available && cpu_backend->trace_func_enabled();
            if (RightAlignedCheckbox("##trace_functions", &enabled) &&
                trace_func_available) {
              cpu_backend->set_trace_func_enabled(enabled);
              ShowNotification("trace_functions",
                               enabled ? "Enabled" : "Disabled");
            }
            ImGui::EndDisabled();
          }
          ImGui::EndTable();
        }
      }

      if (show_logging &&
          BeginSection("Diagnostics / Logging", false, filter_active)) {
        if (BeginSettingsTable("##debug_diagnostics")) {
          if (MatchesFilter("log_level")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("log_level");
            ImGui::TableSetColumnIndex(1);
            RightAlignNextItem(kTextInputWidth);
            bool committed = ImGui::InputText(
                "##log_level", log_level_buffer_, sizeof(log_level_buffer_),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed) {
              ApplyLogLevel();
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
              ApplyLogLevel();
            }
          }

          if (MatchesFilter("log_mask")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("log_mask");
            ImGui::TableSetColumnIndex(1);
            RightAlignNextItem(kTextInputWidth);
            bool committed = ImGui::InputText(
                "##log_mask", log_mask_buffer_, sizeof(log_mask_buffer_),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (committed) {
              ApplyLogMask();
            } else if (ImGui::IsItemDeactivatedAfterEdit()) {
              ApplyLogMask();
            }
          }

          if (MatchesFilter("occlusion_query_log")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("occlusion_query_log");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##occlusion_query_log",
                                     &occlusion_query_log_)) {
              ApplyBoolSetting("GPU", "occlusion_query_log",
                               occlusion_query_log_);
            }
          }

          if (MatchesFilter("gpu_debug_markers")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("gpu_debug_markers");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##gpu_debug_markers",
                                     &gpu_debug_markers_)) {
              ApplyBoolSetting("GPU", "gpu_debug_markers", gpu_debug_markers_);
            }
          }

          if (MatchesFilter("disassemble_pm4")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(is_release_build);
            DrawLabelCell("disassemble_pm4", "[Debug build only]");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##disassemble_pm4", &disassemble_pm4_)) {
              ApplyBoolSetting("GPU", "disassemble_pm4", disassemble_pm4_);
            }
            ImGui::EndDisabled();
          }

          if (MatchesFilter("log_guest_driven_gpu_register_written_values")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(is_release_build);
            DrawLabelCell("log_guest_driven_gpu_register_written_values",
                          "[Debug build only]");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox(
                    "##log_guest_driven_gpu_register_written_values",
                    &log_guest_driven_gpu_register_written_values_)) {
              ApplyBoolSetting("GPU",
                               "log_guest_driven_gpu_register_written_values",
                               log_guest_driven_gpu_register_written_values_);
            }
            ImGui::EndDisabled();
          }

          if (MatchesFilter("log_ringbuffer_kickoff_initiator_bts")) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::BeginDisabled(is_release_build);
            DrawLabelCell("log_ringbuffer_kickoff_initiator_bts",
                          "[Debug build only]");
            ImGui::TableSetColumnIndex(1);
            if (RightAlignedCheckbox("##log_ringbuffer_kickoff_initiator_bts",
                                     &log_ringbuffer_kickoff_initiator_bts_)) {
              ApplyBoolSetting("GPU", "log_ringbuffer_kickoff_initiator_bts",
                               log_ringbuffer_kickoff_initiator_bts_);
            }
            ImGui::EndDisabled();
          }

          if (MatchesFilter("capture_gpu_trace")) {
            Emulator* trace_emulator =
                emulator_window_ ? emulator_window_->emulator() : nullptr;
            gpu::GraphicsSystem* trace_graphics =
                trace_emulator ? trace_emulator->graphics_system() : nullptr;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawLabelCell("capture_gpu_trace",
                          "Writes one frame to trace_gpu_prefix");
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginDisabled(trace_graphics == nullptr);
            RightAlignNextItem(kTextInputWidth);
            if (ImGui::Button("Capture##capture_gpu_trace",
                              ImVec2(kTextInputWidth, 0))) {
              trace_graphics->RequestFrameTrace();
              ShowNotification("capture_gpu_trace", "Frame trace requested");
            }
            ImGui::EndDisabled();
          }

          ImGui::EndTable();
        }
      }

      if (!any_visible) {
        ImGui::TextDisabled("No results.");
      }

      ImGui::EndChild();
    }

    ImGui::End();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(10);

  if (!is_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xe
