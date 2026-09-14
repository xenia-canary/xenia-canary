/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cmath>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/ui/imgui_guest_notification.h"
#include "xenia/ui/imgui_notification.h"

#if XE_PLATFORM_WIN32
#include <playsoundapi.h>
#endif

DEFINE_string(notification_sound_path, "",
              "Path (including filename) to selected notification sound. Sound "
              "MUST be in wav format!",
              "General");

namespace xe {
namespace ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kToastDurationFrames = 510.0f;

struct XboxToastState {
  float frame = 0.0f;
  float opacity = 1.0f;
  float body_alpha = 1.0f;
  float line_alpha = 1.0f;
  float orb_alpha = 1.0f;
  float ring_alpha = 1.0f;
  float body_scale = 1.0f;
  float ring_scale = 1.0f;
  float logo_alpha = 1.0f;
  float logo_scale = 1.0f;
  float achievement_alpha = 0.0f;
  float achievement_scale = 1.0f;
  float text_alpha = 1.0f;
};

float SmoothStep(float start, float end, float value) {
  if (start == end) {
    return value >= end ? 1.0f : 0.0f;
  }
  const float t = std::clamp((value - start) / (end - start), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float Lerp(float from, float to, float amount) {
  return from + (to - from) * std::clamp(amount, 0.0f, 1.0f);
}

XboxToastState GetXboxToastState(float frame) {
  XboxToastState state;
  state.frame = frame;
  state.opacity = SmoothStep(0.0f, 9.0f, frame) *
                  (1.0f - SmoothStep(434.0f, 472.0f, frame));
  state.body_alpha = SmoothStep(0.0f, 9.0f, frame) *
                     (1.0f - SmoothStep(406.0f, 418.0f, frame));
  state.line_alpha = SmoothStep(0.0f, 9.0f, frame) *
                     (1.0f - SmoothStep(444.0f, 460.0f, frame));
  state.orb_alpha = SmoothStep(3.0f, 21.0f, frame) *
                    (1.0f - SmoothStep(444.0f, 478.0f, frame));
  state.ring_alpha = SmoothStep(7.0f, 25.0f, frame) *
                     (1.0f - SmoothStep(438.0f, 472.0f, frame));
  state.text_alpha = SmoothStep(36.0f, 44.0f, frame) *
                     (1.0f - SmoothStep(424.0f, 448.0f, frame));

  state.body_scale = Lerp(0.12f, 1.0f, SmoothStep(13.0f, 31.0f, frame));
  if (frame > 426.0f) {
    state.body_scale = Lerp(1.0f, 0.12f, SmoothStep(426.0f, 462.0f, frame));
  }

  state.ring_scale = Lerp(0.08f, 1.07f, SmoothStep(0.0f, 25.0f, frame));
  if (frame >= 25.0f) {
    state.ring_scale = Lerp(1.07f, 1.0f, SmoothStep(25.0f, 60.0f, frame));
  }
  if (frame > 426.0f && frame <= 440.0f) {
    state.ring_scale = Lerp(1.0f, 1.16f, SmoothStep(426.0f, 440.0f, frame));
  } else if (frame > 440.0f) {
    state.ring_scale = Lerp(1.16f, 0.22f, SmoothStep(440.0f, 490.0f, frame));
  }

  state.logo_alpha = 0.0f;
  if (frame < 60.0f) {
    state.logo_alpha = 1.0f;
    state.logo_scale = frame < 34.0f
                           ? Lerp(0.08f, 1.38f, SmoothStep(0.0f, 34.0f, frame))
                           : Lerp(1.38f, 1.0f, SmoothStep(34.0f, 60.0f, frame));
  } else if (frame < 425.0f) {
    const float loop_frame = std::fmod(frame - 60.0f, 120.0f);
    state.logo_alpha = std::max(1.0f - SmoothStep(48.0f, 60.0f, loop_frame),
                                SmoothStep(108.0f, 120.0f, loop_frame));
    state.achievement_alpha = SmoothStep(54.0f, 66.0f, loop_frame) *
                              (1.0f - SmoothStep(102.0f, 114.0f, loop_frame));
    state.achievement_scale =
        Lerp(0.96f, 1.0f, SmoothStep(54.0f, 70.0f, loop_frame));
  } else {
    state.logo_alpha = 1.0f;
    const float visual_scale =
        frame > 440.0f ? Lerp(1.0f, 0.2f, SmoothStep(440.0f, 502.0f, frame))
                       : 1.0f;
    state.logo_scale = visual_scale / std::max(state.ring_scale, 0.001f);
  }
  return state;
}

ImU32 ToastColor(uint32_t rgb, float alpha) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                  static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

ImVec2 RingPoint(const ImVec2& center, float radius, float degrees) {
  const float radians = degrees * kPi / 180.0f;
  return {center.x + std::cos(radians) * radius,
          center.y + std::sin(radians) * radius};
}

void DrawRingArc(ImDrawList* draw_list, const ImVec2& center, float radius,
                 float start, float end, ImU32 color, float thickness) {
  draw_list->PathArcTo(center, radius, start * kPi / 180.0f, end * kPi / 180.0f,
                       20);
  draw_list->PathStroke(color, ImDrawFlags_None, thickness);
  const float cap_radius = thickness * 0.5f;
  draw_list->AddCircleFilled(RingPoint(center, radius, start), cap_radius,
                             color);
  draw_list->AddCircleFilled(RingPoint(center, radius, end), cap_radius, color);
}

void DrawGuideRing(ImDrawList* draw_list, const ImVec2& center, float scale,
                   const XboxToastState& state) {
  const float ring_scale = scale * state.ring_scale;
  draw_list->AddCircleFilled(center, 26.25f * ring_scale,
                             ToastColor(0x000000, 0.72f * state.orb_alpha));
  draw_list->AddCircleFilled(center, 16.0f * ring_scale,
                             ToastColor(0x000000, 0.98f * state.orb_alpha));

  constexpr std::array<float, 4> centers = {225.0f, 315.0f, 45.0f, 135.0f};
  const float radius = 21.825f * ring_scale;
  const float thickness = 5.15f * ring_scale;
  for (float angle : centers) {
    DrawRingArc(draw_list, center, radius, angle - 39.0f, angle + 38.0f,
                ToastColor(0x707070, 0.96f * state.ring_alpha), thickness);
  }

  const float closing = SmoothStep(418.0f, 426.0f, state.frame);
  const float dim = 1.0f - 0.35f * closing;
  const float first_alpha = (1.0f - closing) * dim;
  const float other_alpha =
      (1.0f - SmoothStep(58.0f, 68.0f, state.frame)) * first_alpha;
  for (size_t i = 0; i < centers.size(); ++i) {
    const float alpha = i ? other_alpha : first_alpha;
    if (alpha > 0.001f) {
      DrawRingArc(
          draw_list, center, radius, centers[i] - 39.0f, centers[i] + 38.0f,
          ToastColor(0xFFD85F, 0.92f * state.ring_alpha * alpha), thickness);
    }
  }
}

void DrawTrophy(ImDrawList* draw_list, const ImVec2& center, float size,
                ImU32 color) {
  const float unit = size / 24.0f;
  const auto point = [&](float x, float y) {
    return ImVec2(center.x + x * unit, center.y + y * unit);
  };
  const ImVec2 cup[] = {point(-6.0f, -8.0f), point(6.0f, -8.0f),
                        point(5.2f, -3.0f),  point(3.0f, 0.0f),
                        point(1.5f, 1.2f),   point(-1.5f, 1.2f),
                        point(-3.0f, 0.0f),  point(-5.2f, -3.0f)};
  const ImVec2 left_handle[] = {point(-5.7f, -6.5f), point(-9.0f, -6.5f),
                                point(-9.0f, -3.0f), point(-6.1f, -0.8f),
                                point(-5.0f, -2.5f), point(-7.1f, -3.7f),
                                point(-7.1f, -4.7f), point(-5.6f, -4.7f)};
  const ImVec2 right_handle[] = {point(5.7f, -6.5f), point(9.0f, -6.5f),
                                 point(9.0f, -3.0f), point(6.1f, -0.8f),
                                 point(5.0f, -2.5f), point(7.1f, -3.7f),
                                 point(7.1f, -4.7f), point(5.6f, -4.7f)};
  draw_list->AddConvexPolyFilled(cup, IM_ARRAYSIZE(cup), color);
  draw_list->AddConvexPolyFilled(left_handle, IM_ARRAYSIZE(left_handle), color);
  draw_list->AddConvexPolyFilled(right_handle, IM_ARRAYSIZE(right_handle),
                                 color);
  draw_list->AddRectFilled(point(-1.3f, 0.5f), point(1.3f, 6.0f), color);
  draw_list->AddRectFilled(point(-3.7f, 5.2f), point(3.7f, 7.2f), color,
                           0.8f * unit);
  draw_list->AddRectFilled(point(-5.0f, 7.0f), point(5.0f, 9.0f), color,
                           0.8f * unit);
}

}  // namespace

ImGuiGuestNotification::ImGuiGuestNotification(ui::ImGuiDrawer* imgui_drawer,
                                               std::string& title,
                                               std::string& description,
                                               uint8_t user_index,
                                               uint8_t position_id)
    : ImGuiNotification(imgui_drawer, NotificationType::Guest, title,
                        description, user_index, position_id),
      current_stage_(NotificationStage::Awaiting),
      notification_draw_progress_(0.0f) {
  imgui_drawer->AddNotification(this);
}

ImGuiGuestNotification::~ImGuiGuestNotification() {}

void ImGuiGuestNotification::UpdateNotificationState() {
  switch (current_stage_) {
    case NotificationStage::Awaiting:
      // TODO(Gliniak): Implement delayed notifications.
      current_stage_ = NotificationStage::FazeIn;
      notification_draw_progress_ = 0.2f;
#if XE_PLATFORM_WIN32
      if (!cvars::notification_sound_path.empty()) {
        auto notification_sound_path = cvars::notification_sound_path;
        if (std::filesystem::exists(notification_sound_path)) {
          PlaySound(std::wstring(notification_sound_path.begin(),
                                 notification_sound_path.end())
                        .c_str(),
                    NULL,
                    SND_FILENAME | SND_NODEFAULT | SND_NOSTOP | SND_ASYNC);
        }
      }
#endif

      break;
    case NotificationStage::FazeIn: {
      SetCreationTime(Clock::QueryHostUptimeMillis());
      if (notification_draw_progress_ < 1.1f) {
        notification_draw_progress_ += 0.02f;
      }

      // Mimics a bit original console behaviour when it makes window a bit
      // longer for few frames then decreases size
      if (notification_draw_progress_ >= 1.1f) {
        current_stage_ = NotificationStage::Present;
        notification_draw_progress_ = 1.0f;
      }
      break;
    }
    case NotificationStage::Present:
      if (IsNotificationClosingTime()) {
        current_stage_ = NotificationStage::FazeOut;
      }
      break;
    case NotificationStage::FazeOut: {
      if (notification_draw_progress_ > 0.2f) {
        notification_draw_progress_ -= 0.02f;
      } else {
        current_stage_ = NotificationStage::Finished;
      }
      break;
    }
    default:
      break;
  }
}

const ImVec2 ImGuiGuestNotification::CalculateNotificationSize(ImVec2 text_size,
                                                               float scale) {
  const ImVec2 result = ImVec2(floorf((default_notification_icon_size.x +
                                       default_notification_margin_size.x) *
                                      scale) +
                                   text_size.x,
                               floorf((default_notification_icon_size.y +
                                       default_notification_margin_size.y) *
                                      scale));

  return result;
}

void AchievementNotificationWindow::OnDraw(ImGuiIO& io) {
  const uint64_t now = Clock::QueryHostUptimeMillis();
  if (!animation_started_) {
    animation_started_ = true;
    animation_start_ = now;
    UpdateNotificationState();
  }

  const float frame = static_cast<float>(now - animation_start_) * 0.06f;
  if (frame >= kToastDurationFrames) {
    delete this;
    return;
  }
  if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
    return;
  }

  constexpr float kMargin = 6.0f;
  constexpr float kBodyLeft = 17.5f;
  constexpr float kTextLeft = 73.5f;
  constexpr float kTextOffsetY = 3.5f;
  constexpr float kRingCenterX = 39.5f;
  constexpr float kRingCenterY = 30.5f;
  const XboxToastState state = GetXboxToastState(frame);
  const float scale =
      std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f);
  ImFont* title_font = GetDrawer()->GetTitleFont();
  ImFont* subtitle_font = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];
  const auto title = GetTitle();
  const auto subtitle = GetDescription();
  const float title_width =
      title_font
          ->CalcTextSizeA(21.25f * scale, FLT_MAX, 0.0f, title.data(),
                          title.data() + title.size())
          .x /
      scale;
  const float subtitle_width =
      subtitle_font
          ->CalcTextSizeA(22.0f * scale, FLT_MAX, 0.0f, subtitle.data(),
                          subtitle.data() + subtitle.size())
          .x /
      scale;
  const float final_right =
      std::clamp(kTextLeft + std::max(title_width, subtitle_width) + 34.0f,
                 366.0f, 1150.0f);
  const ImVec2 window_size = {(final_right + 8.0f + kMargin * 2.0f) * scale,
                              (64.0f + kMargin * 2.0f) * scale};
  ImVec2 window_position = CalculateNotificationScreenPosition(
      io.DisplaySize, window_size, GetPositionId());
  if (std::isnan(window_position.x) || std::isnan(window_position.y)) {
    return;
  }
  if (GetPositionId() == 0) {
    window_position.x -= 380.0f * scale;
    window_position.y -= 265.0f * scale;
  }

  ImGui::SetNextWindowPos(window_position);
  ImGui::SetNextWindowSize(window_size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
  ImGui::Begin(
      "##AchievementNotification", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
          ImGuiWindowFlags_NoFocusOnAppearing |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

  ImDrawList* draw_list = ImGui::GetWindowDrawList();
  const ImVec2 origin = {window_position.x + kMargin * scale,
                         window_position.y + kMargin * scale};
  const float opening_extra =
      frame >= 18.0f && frame <= 78.0f
          ? std::clamp((final_right - kBodyLeft) * 0.055f, 10.0f, 34.0f) *
                SmoothStep(18.0f, 32.0f, frame) *
                (1.0f - SmoothStep(38.0f, 78.0f, frame))
          : 0.0f;
  const float current_right =
      kBodyLeft + (final_right + opening_extra - kBodyLeft) * state.body_scale;
  const ImVec2 body_min = {origin.x + 11.2f * scale, origin.y + 4.0f * scale};
  const ImVec2 body_max = {origin.x + (current_right - 0.4f) * scale,
                           origin.y + 56.9f * scale};
  const float radius = 26.45f * scale;

  draw_list->AddRectFilled(
      {body_min.x - 3.7f * scale, body_min.y - 3.0f * scale},
      {body_max.x + 3.4f * scale, body_max.y + 3.1f * scale},
      ToastColor(0xE2E2E2, 0.08f * state.body_alpha), radius);
  draw_list->AddRectFilled(
      body_min, body_max,
      ToastColor(0x373737, state.opacity * state.body_alpha), radius);
  draw_list->AddRect(body_min, body_max,
                     ToastColor(0xFFFFFF, state.opacity * state.line_alpha),
                     radius, ImDrawFlags_RoundCornersAll, 2.2f * scale);

  const ImVec2 ring_center = {origin.x + kRingCenterX * scale,
                              origin.y + kRingCenterY * scale};
  draw_list->AddCircleFilled(ring_center, 33.5f * state.ring_scale * scale,
                             ToastColor(0xFFFFFF, 0.02f * state.ring_alpha));
  DrawGuideRing(draw_list, ring_center, scale, state);

  ImmediateTexture* logo = GetDrawer()->GetAchievementNotificationLogo();
  if (!logo) {
    logo = GetDrawer()->GetNotificationIcon(GetUserIndex());
  }
  const float logo_size = 33.5f * state.logo_scale * state.ring_scale * scale;
  if (logo && state.logo_alpha > 0.001f) {
    const float half = logo_size * 0.5f;
    draw_list->AddImage(
        reinterpret_cast<ImTextureID>(logo),
        {ring_center.x - half, ring_center.y - half},
        {ring_center.x + half, ring_center.y + half}, {0.0f, 0.0f},
        {1.0f, 1.0f},
        ToastColor(0xFFFFFF, state.logo_alpha * state.ring_alpha));
  }

  if (state.achievement_alpha > 0.001f) {
    const float icon_size = 33.8f * state.achievement_scale * scale;
    const ImVec2 trophy_center = {ring_center.x, ring_center.y - 0.15f * scale};
    DrawTrophy(draw_list, trophy_center, icon_size,
               ToastColor(0xFFFFFF, state.achievement_alpha * state.opacity));
  }

  if (state.text_alpha > 0.001f) {
    const ImU32 shadow =
        ToastColor(0x000000, 0.36f * state.opacity * state.text_alpha);
    const ImU32 text = ToastColor(0xFFFFFF, state.opacity * state.text_alpha);
    const ImVec2 title_position = {origin.x + kTextLeft * scale,
                                   origin.y + (5.75f + kTextOffsetY) * scale};
    const ImVec2 subtitle_position = {
        origin.x + kTextLeft * scale,
        origin.y + (28.0f + kTextOffsetY) * scale};
    const float shadow_offset = 0.5f * scale;
    draw_list->AddText(
        title_font, 21.25f * scale,
        {title_position.x + shadow_offset, title_position.y + shadow_offset},
        shadow, title.data(), title.data() + title.size());
    draw_list->AddText(subtitle_font, 22.0f * scale,
                       {subtitle_position.x + shadow_offset,
                        subtitle_position.y + shadow_offset},
                       shadow, subtitle.data(),
                       subtitle.data() + subtitle.size());
    draw_list->AddText(title_font, 21.25f * scale, title_position, text,
                       title.data(), title.data() + title.size());
    draw_list->AddText(subtitle_font, 22.0f * scale, subtitle_position, text,
                       subtitle.data(), subtitle.data() + subtitle.size());
  }

  ImGui::End();
  ImGui::PopStyleVar();
}

void XNotifyWindow::OnDraw(ImGuiIO& io) {
  UpdateNotificationState();

  if (IsNotificationExpired()) {
    delete this;
    return;
  }

  const std::string_view longest_notification_text_line =
      GetTitle().size() > GetDescription().size() ? GetTitle()
                                                  : GetDescription();

  const ImVec2 screen_size = io.DisplaySize;
  const float window_scale =
      std::fminf(screen_size.x / default_drawing_resolution.x,
                 screen_size.y / default_drawing_resolution.y);
  const float font_scale = default_font_size / io.Fonts->Fonts[0]->FontSize;
  const ImVec2 text_size = io.Fonts->Fonts[0]->CalcTextSizeA(
      default_font_size * default_notification_text_scale * window_scale,
      FLT_MAX, -1.0f, longest_notification_text_line.data());

  const ImVec2 final_notification_size =
      CalculateNotificationSize(text_size, window_scale);

  const ImVec2 notification_position = CalculateNotificationScreenPosition(
      screen_size, final_notification_size, GetPositionId());

  if (std::isnan(notification_position.x) ||
      std::isnan(notification_position.y)) {
    return;
  }

  ImVec2 current_notification_size = final_notification_size;
  current_notification_size.x *= notification_draw_progress_;
  current_notification_size.x = floorf(current_notification_size.x);

  // Initialize position and window size
  ImGui::SetNextWindowSize(current_notification_size);
  ImGui::SetNextWindowPos(notification_position);

  // Set new window style before drawing window
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                      default_notification_rounding * window_scale);
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        default_notification_background_color);
  ImGui::PushStyleColor(ImGuiCol_Separator,
                        default_notification_background_color);
  ImGui::PushStyleColor(ImGuiCol_Border, default_notification_border_color);

  ImGui::Begin("Notification Window", NULL, NOTIFY_TOAST_FLAGS);
  {
    ImGui::SetWindowFontScale(default_notification_text_scale * font_scale *
                              window_scale);
    // Set offset to image to prevent it from being right on border.
    ImGui::SetCursorPos(ImVec2(final_notification_size.x * 0.005f,
                               final_notification_size.y * 0.05f));
    // Elements of window
    ImGui::Image(reinterpret_cast<ImTextureID>(
                     GetDrawer()->GetNotificationIcon(GetUserIndex())),
                 ImVec2(default_notification_icon_size.x * window_scale,
                        default_notification_icon_size.y * window_scale));

    // Set offset to image to prevent it from being right on border.
    // ImGui::SetCursorPos(ImVec2(final_notification_size.x * 0.1f,
    //                           final_notification_size.y * 0.2f));

    ImGui::SameLine();
    if (notification_draw_progress_ > 0.5f) {
      ImGui::TextColored(white_color, "%s", GetDescription().data());
    }
  }
  // Restore previous style
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);

  ImGui::End();
}

}  // namespace ui
}  // namespace xe
