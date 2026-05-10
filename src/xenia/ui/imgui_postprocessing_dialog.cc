/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_postprocessing_dialog.h"

#include "third_party/imgui/imgui.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/ui/imgui_host_notification.h"
#include "xenia/ui/presenter.h"

DECLARE_string(postprocess_antialiasing);
DECLARE_string(postprocess_scaling_and_sharpening);
DECLARE_double(postprocess_ffx_fsr_sharpness_reduction);
DECLARE_double(postprocess_ffx_cas_additional_sharpness);
DECLARE_uint32(postprocess_ffx_fsr_max_upsampling_passes);
DECLARE_bool(postprocess_dither);

namespace xe {
namespace ui {

ImGuiPostProcessingDialog::ImGuiPostProcessingDialog(
    ImGuiDrawer* drawer, app::EmulatorWindow* emulator_window,
    hid::InputSystem* input_system)
    : ImGuiGamepadDialog(drawer, input_system),
      emulator_window_(emulator_window) {
  LoadCurrentSettings();

  // Initialize highlight positions to match current selections
  aa_highlight_ = anti_aliasing_mode_;
  resampling_highlight_ = resampling_mode_;
}

void ImGuiPostProcessingDialog::OnClose() {
  if (on_close_callback_) {
    on_close_callback_();
  }
}

void ImGuiPostProcessingDialog::LoadCurrentSettings() {
  // Load anti-aliasing setting
  auto aa_effect = app::EmulatorWindow::GetSwapPostEffectForCvarValue(
      cvars::postprocess_antialiasing);
  switch (aa_effect) {
    case gpu::CommandProcessor::SwapPostEffect::kFxaa:
      anti_aliasing_mode_ = 1;
      break;
    case gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme:
      anti_aliasing_mode_ = 2;
      break;
    default:
      anti_aliasing_mode_ = 0;
      break;
  }

  // Load resampling/scaling effect
  auto paint_config = app::EmulatorWindow::GetGuestOutputPaintConfigForCvars();
  switch (paint_config.GetEffect()) {
    case Presenter::GuestOutputPaintConfig::Effect::kCas:
      resampling_mode_ = 1;
      break;
    case Presenter::GuestOutputPaintConfig::Effect::kFsr:
      resampling_mode_ = 2;
      break;
    default:
      resampling_mode_ = 0;
      break;
  }

  // Load slider values
  fsr_sharpness_ = paint_config.GetFsrSharpnessReduction();
  fsr_max_upsampling_passes_ =
      static_cast<int>(paint_config.GetFsrMaxUpsamplingPasses());
  cas_additional_sharpness_ = paint_config.GetCasAdditionalSharpness();

  // Load dithering
  dither_ = paint_config.GetDither();
}

void ImGuiPostProcessingDialog::ShowNotification(
    const std::string& title, const std::string& description) {
  // Position 10 = RIGHT-BOTTOM (default for HostNotificationWindow)
  new HostNotificationWindow(imgui_drawer(), title, description, 0);
}

void ImGuiPostProcessingDialog::OnAntiAliasingChanged(int value) {
  gpu::CommandProcessor::SwapPostEffect effect;
  const char* mode_name;

  switch (value) {
    case 1:
      effect = gpu::CommandProcessor::SwapPostEffect::kFxaa;
      mode_name = "FXAA";
      break;
    case 2:
      effect = gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
      mode_name = "FXAA Extreme";
      break;
    default:
      effect = gpu::CommandProcessor::SwapPostEffect::kNone;
      mode_name = "None";
      break;
  }

  emulator_window_->UpdateAntiAliasingCvar(effect);
  emulator_window_->ApplyDisplayConfigForCvars();
  ShowNotification("Anti-Aliasing", mode_name);
}

void ImGuiPostProcessingDialog::OnResamplingChanged(int value) {
  Presenter::GuestOutputPaintConfig::Effect effect;
  const char* mode_name;

  switch (value) {
    case 1:
      effect = Presenter::GuestOutputPaintConfig::Effect::kCas;
      mode_name = "CAS";
      break;
    case 2:
      effect = Presenter::GuestOutputPaintConfig::Effect::kFsr;
      mode_name = "FSR";
      break;
    default:
      effect = Presenter::GuestOutputPaintConfig::Effect::kBilinear;
      mode_name = "Bilinear";
      break;
  }

  emulator_window_->UpdateScalingAndSharpeningCvar(effect);
  emulator_window_->ApplyDisplayConfigForCvars();
  ShowNotification("Resampling", mode_name);
}

void ImGuiPostProcessingDialog::OnFsrSharpnessChanged(float value) {
  emulator_window_->UpdateFsrSharpnessCvar(value);
  emulator_window_->ApplyDisplayConfigForCvars();
}

void ImGuiPostProcessingDialog::OnFsrMaxUpsamplingPassesChanged(int value) {
  emulator_window_->UpdateFsrMaxUpsamplingPassesCvar(
      static_cast<uint32_t>(value));
  emulator_window_->ApplyDisplayConfigForCvars();
}

void ImGuiPostProcessingDialog::OnCasSharpnessChanged(float value) {
  emulator_window_->UpdateCasSharpnessCvar(value);
  emulator_window_->ApplyDisplayConfigForCvars();
}

void ImGuiPostProcessingDialog::OnDitherChanged(bool value) {
  emulator_window_->UpdateDitherCvar(value);
  emulator_window_->ApplyDisplayConfigForCvars();
  ShowNotification("Dithering", value ? "Enabled" : "Disabled");
}

void ImGuiPostProcessingDialog::OnDraw(ImGuiIO& io) {
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
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

  // Center on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  bool is_open = true;
  if (ImGui::Begin("Post-Processing Settings", &is_open,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse)) {
    // Handle keyboard escape or gamepad B/Back
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      Close();
    }

    // Colors - highlight uses lighter green for non-selected options
    ImVec4 highlight_color = ImVec4(0.1f, 0.6f, 0.1f, 1.0f);

    // Anti-aliasing section
    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Anti-Aliasing");
    ImGui::PopStyleColor();

    ImGui::Indent(10);
    const char* aa_labels[] = {"None", "FXAA", "FXAA Extreme"};
    for (int i = 0; i < 3; i++) {
      bool is_selected = (anti_aliasing_mode_ == i);
      bool is_highlighted = (aa_highlight_ == i);

      if (is_highlighted && !is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, highlight_color);
      }

      if (ImGui::RadioButton(aa_labels[i], is_selected)) {
        if (!is_selected) {
          anti_aliasing_mode_ = i;
          aa_highlight_ = i;
          OnAntiAliasingChanged(i);
        }
      }

      if (is_highlighted && !is_selected) {
        ImGui::PopStyleColor();
      }

      if (i < 2) {
        ImGui::SameLine();
      }
    }
    ImGui::Unindent(10);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Resampling / Scaling section
    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Resampling / Scaling");
    ImGui::PopStyleColor();

    ImGui::Indent(10);
    const char* resampling_labels[] = {"Bilinear", "CAS", "FSR"};
    for (int i = 0; i < 3; i++) {
      bool is_selected = (resampling_mode_ == i);
      bool is_highlighted = (resampling_highlight_ == i);

      if (is_highlighted && !is_selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, highlight_color);
      }

      if (ImGui::RadioButton(resampling_labels[i], is_selected)) {
        if (!is_selected) {
          resampling_mode_ = i;
          resampling_highlight_ = i;
          OnResamplingChanged(i);
        }
      }

      if (is_highlighted && !is_selected) {
        ImGui::PopStyleColor();
      }

      if (i < 2) {
        ImGui::SameLine();
      }
    }
    ImGui::Unindent(10);

    // FSR-specific options (only show when FSR is selected)
    if (resampling_mode_ == 2) {
      ImGui::Spacing();

      ImGui::Indent(10);
      ImGui::Text("FSR Sharpness Reduction");
      ImGui::SetNextItemWidth(200);
      if (ImGui::SliderFloat("##fsr_sharpness", &fsr_sharpness_, 0.0f, 2.0f,
                             "%.2f")) {
        OnFsrSharpnessChanged(fsr_sharpness_);
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset##fsr_sharp")) {
        fsr_sharpness_ =
            Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault;
        OnFsrSharpnessChanged(fsr_sharpness_);
      }

      ImGui::Text("FSR Max Upsampling Passes");
      ImGui::SetNextItemWidth(200);
      if (ImGui::SliderInt(
              "##fsr_passes", &fsr_max_upsampling_passes_, 1,
              Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax)) {
        OnFsrMaxUpsamplingPassesChanged(fsr_max_upsampling_passes_);
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset##fsr_passes")) {
        fsr_max_upsampling_passes_ =
            Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax;
        OnFsrMaxUpsamplingPassesChanged(fsr_max_upsampling_passes_);
      }
      ImGui::Unindent(10);
    }

    // CAS-specific options (only show when CAS or FSR is selected)
    if (resampling_mode_ >= 1) {
      ImGui::Spacing();

      ImGui::Indent(10);
      ImGui::Text("CAS Additional Sharpness");
      ImGui::SetNextItemWidth(200);
      if (ImGui::SliderFloat("##cas_sharpness", &cas_additional_sharpness_,
                             0.0f, 1.0f, "%.2f")) {
        OnCasSharpnessChanged(cas_additional_sharpness_);
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset##cas_sharp")) {
        cas_additional_sharpness_ =
            Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault;
        OnCasSharpnessChanged(cas_additional_sharpness_);
      }
      ImGui::Unindent(10);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Dithering section
    ImGui::PushStyleColor(ImGuiCol_Text, xbox_green);
    ImGui::Text("Output");
    ImGui::PopStyleColor();

    ImGui::Indent(10);
    if (ImGui::Checkbox("Dithering", &dither_)) {
      OnDitherChanged(dither_);
    }
    ImGui::Unindent(10);

    ImGui::End();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(15);

  if (!is_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xe
