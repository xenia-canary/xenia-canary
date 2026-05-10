/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_performance_dialog.h"

#include "third_party/imgui/imgui.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/imgui_host_notification.h"

DECLARE_bool(clear_memory_page_state);
DECLARE_bool(readback_memexport);
DECLARE_bool(readback_memexport_fast);
DECLARE_string(readback_resolve);
DECLARE_bool(guest_display_refresh_cap);
DECLARE_string(occlusion_query);

namespace xe {
namespace ui {

ImGuiPerformanceDialog::ImGuiPerformanceDialog(
    ImGuiDrawer* drawer, app::EmulatorWindow* emulator_window,
    hid::InputSystem* input_system)
    : ImGuiGamepadDialog(drawer, input_system),
      emulator_window_(emulator_window) {
  LoadCurrentSettings();

  // Initialize highlight positions to match current selections
  resolve_highlight_ = readback_resolve_mode_;
  memexport_highlight_ = readback_memexport_mode_;
  occlusion_query_highlight_ = occlusion_query_mode_;
}

void ImGuiPerformanceDialog::OnClose() {
  if (on_close_callback_) {
    on_close_callback_();
  }
}

void ImGuiPerformanceDialog::LoadCurrentSettings() {
  // Load Emulated Display Uncapped (inverted from guest_display_refresh_cap)
  display_uncapped_ = !cvars::guest_display_refresh_cap;

  // Load Occlusion Query setting (0=fake, 1=fast, 2=strict)
  const std::string& oq_mode = cvars::occlusion_query;
  if (oq_mode == "fast") {
    occlusion_query_mode_ = 1;
  } else if (oq_mode == "strict") {
    occlusion_query_mode_ = 2;
  } else {
    occlusion_query_mode_ = 0;  // Default to "fake"
  }

  // Load Readback Resolve setting (0=none, 1=some, 2=fast, 3=full)
  const std::string& resolve_mode = cvars::readback_resolve;
  if (resolve_mode == "none") {
    readback_resolve_mode_ = 0;
  } else if (resolve_mode == "some") {
    readback_resolve_mode_ = 1;
  } else if (resolve_mode == "full") {
    readback_resolve_mode_ = 3;
  } else {
    readback_resolve_mode_ = 2;  // Default to "fast"
  }

  // Load Readback Memexport setting (0=none, 1=fast, 2=full)
  if (!cvars::readback_memexport) {
    readback_memexport_mode_ = 0;
  } else if (cvars::readback_memexport_fast) {
    readback_memexport_mode_ = 1;
  } else {
    readback_memexport_mode_ = 2;
  }

  // Load Clear Memory Page State setting
  clear_memory_page_state_ = cvars::clear_memory_page_state;
}

void ImGuiPerformanceDialog::ShowNotification(const std::string& title,
                                              const std::string& description) {
  // Position 10 = RIGHT-BOTTOM (default for HostNotificationWindow)
  new HostNotificationWindow(imgui_drawer(), title, description, 0);
}

void ImGuiPerformanceDialog::OnReadbackResolveChanged(int value) {
  auto emulator = emulator_window_->emulator();
  if (!emulator) {
    return;
  }

  auto graphics_system = emulator->graphics_system();
  if (!graphics_system) {
    return;
  }

  auto command_processor = graphics_system->command_processor();
  if (!command_processor) {
    return;
  }

  gpu::ReadbackResolveMode mode;
  switch (value) {
    case 0:
      mode = gpu::ReadbackResolveMode::kDisabled;
      break;
    case 1:
      mode = gpu::ReadbackResolveMode::kSome;
      break;
    case 3:
      mode = gpu::ReadbackResolveMode::kFull;
      break;
    default:
      mode = gpu::ReadbackResolveMode::kFast;
      break;
  }

  command_processor->SetReadbackResolveMode(mode);

  const char* mode_names[] = {"None", "Some", "Fast", "Full"};
  ShowNotification("Readback Resolve", mode_names[value]);
}

void ImGuiPerformanceDialog::OnReadbackMemexportChanged(int value) {
  bool memexport_enabled = true;
  bool memexport_fast = true;

  switch (value) {
    case 0:
      memexport_enabled = false;
      break;
    case 1:
      memexport_fast = true;
      break;
    case 2:
      memexport_fast = false;
      break;
  }

  gpu::SaveGPUSetting(gpu::GPUSetting::ReadbackMemexport, memexport_enabled);
  gpu::SaveGPUSetting(gpu::GPUSetting::ReadbackMemexportFast, memexport_fast);
  config::SaveGameConfigSetting(emulator_window_->emulator(), "GPU",
                                "readback_memexport", memexport_enabled);
  config::SaveGameConfigSetting(emulator_window_->emulator(), "GPU",
                                "readback_memexport_fast", memexport_fast);

  const char* mode_names[] = {"None", "Fast", "Full"};
  ShowNotification("Readback Memexport", mode_names[value]);
}

void ImGuiPerformanceDialog::OnEmulatedDisplayUncappedChanged(bool uncapped) {
  SetGuestDisplayRefreshCap(!uncapped);
  config::SaveGameConfigSetting(emulator_window_->emulator(), "GPU",
                                "guest_display_refresh_cap", !uncapped);
  ShowNotification("Emulated Display", uncapped ? "Uncapped" : "Capped");
}

void ImGuiPerformanceDialog::OnOcclusionQueryChanged(int value) {
  auto emulator = emulator_window_->emulator();
  if (!emulator) {
    return;
  }

  auto graphics_system = emulator->graphics_system();
  if (!graphics_system) {
    return;
  }

  auto command_processor = graphics_system->command_processor();
  if (!command_processor) {
    return;
  }

  gpu::ZPDMode mode;
  switch (value) {
    case 1:
      mode = gpu::ZPDMode::kFast;
      break;
    case 2:
      mode = gpu::ZPDMode::kStrict;
      break;
    default:
      mode = gpu::ZPDMode::kFake;
      break;
  }

  command_processor->SetZPDMode(mode);

  const char* mode_names[] = {"Fake", "Fast", "Strict"};
  ShowNotification("Occlusion Query Mode", mode_names[static_cast<int>(mode)]);
}

void ImGuiPerformanceDialog::OnClearMemoryPageStateChanged(bool enabled) {
  gpu::SaveGPUSetting(gpu::GPUSetting::ClearMemoryPageState, enabled);
  config::SaveGameConfigSetting(emulator_window_->emulator(), "GPU",
                                "clear_memory_page_state", enabled);
  ShowNotification("Clear Memory Page State", enabled ? "Enabled" : "Disabled");
}

void ImGuiPerformanceDialog::OnDraw(ImGuiIO& io) {
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

  // Center on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  bool is_open = true;
  if (ImGui::Begin("Performance Settings", &is_open,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse)) {
    // Handle keyboard escape, F7, or gamepad B/Back
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        ImGui::IsKeyPressed(ImGuiKey_F7) || ShouldCloseFromGamepad()) {
      Close();
    }

    // Colors - highlight uses lighter green for non-selected options
    ImVec4 highlight_color = ImVec4(0.1f, 0.6f, 0.1f, 1.0f);

    // Readback Resolve section
    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Readback Resolve");
    ImGui::PopStyleColor();

    ImGui::Indent(10);
    ImGui::PushID("resolve");
    const char* resolve_labels[] = {"None", "Some", "Fast", "Full"};
    for (int i = 0; i < 4; i++) {
      bool is_selected = (readback_resolve_mode_ == i);
      bool is_highlighted = (resolve_highlight_ == i);

      if (is_highlighted && !is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, highlight_color);
      }

      if (ImGui::RadioButton(resolve_labels[i], is_selected)) {
        if (!is_selected) {
          readback_resolve_mode_ = i;
          resolve_highlight_ = i;
          OnReadbackResolveChanged(i);
        }
      }

      if (is_highlighted && !is_selected) {
        ImGui::PopStyleColor();
      }

      if (i < 3) {
        ImGui::SameLine();
      }
    }
    ImGui::PopID();
    ImGui::Unindent(10);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Readback Memexport section
    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Readback Memexport");
    ImGui::PopStyleColor();

    ImGui::Indent(10);
    ImGui::PushID("memexport");
    const char* memexport_labels[] = {"None", "Fast", "Full"};
    for (int i = 0; i < 3; i++) {
      bool is_selected = (readback_memexport_mode_ == i);
      bool is_highlighted = (memexport_highlight_ == i);

      if (is_highlighted && !is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, highlight_color);
      }

      if (ImGui::RadioButton(memexport_labels[i], is_selected)) {
        if (!is_selected) {
          readback_memexport_mode_ = i;
          memexport_highlight_ = i;
          OnReadbackMemexportChanged(i);
        }
      }

      if (is_highlighted && !is_selected) {
        ImGui::PopStyleColor();
      }

      if (i < 2) {
        ImGui::SameLine();
      }
    }
    ImGui::PopID();
    ImGui::Unindent(10);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Occlusion Query Mode");
    ImGui::PopStyleColor();

    ImGui::Indent(10);
    ImGui::PushID("occlusion_query");
    const char* oq_labels[] = {"Fake", "Fast", "Strict"};
    for (int i = 0; i < 3; i++) {
      bool is_selected = (occlusion_query_mode_ == i);
      bool is_highlighted = (occlusion_query_highlight_ == i);

      if (is_highlighted && !is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, highlight_color);
      }

      if (ImGui::RadioButton(oq_labels[i], is_selected)) {
        if (!is_selected) {
          occlusion_query_mode_ = i;
          occlusion_query_highlight_ = i;
          OnOcclusionQueryChanged(i);
        }
      }

      if (is_highlighted && !is_selected) {
        ImGui::PopStyleColor();
      }

      if (i < 2) {
        ImGui::SameLine();
      }
    }
    ImGui::PopID();
    ImGui::Unindent(10);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Other section
    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Other");
    ImGui::PopStyleColor();

    ImGui::Indent(10);

    if (ImGui::Checkbox("Emulated Display Uncapped", &display_uncapped_)) {
      OnEmulatedDisplayUncappedChanged(display_uncapped_);
    }

    if (ImGui::Checkbox("Clear memory page state on GPU cache invalidation",
                        &clear_memory_page_state_)) {
      OnClearMemoryPageStateChanged(clear_memory_page_state_);
    }

    ImGui::Unindent(10);

    ImGui::End();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(9);

  if (!is_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xe
