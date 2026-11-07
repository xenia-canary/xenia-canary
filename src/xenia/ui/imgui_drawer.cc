/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_drawer.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <ranges>

#include "third_party/imgui/imgui.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_notification.h"
#include "xenia/ui/resources.h"
#include "xenia/ui/ui_event.h"
#include "xenia/ui/window.h"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"

#include "xenia/base/mapped_memory.h"

// Dear ImGui compiles its stb_truetype as static, so this file needs its own
// copy to check a font before handing it to the atlas.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/imgui/imstb_truetype.h"

#if XE_PLATFORM_WIN32
#include <ShlObj_core.h>
#endif

#if XE_PLATFORM_LINUX
#include <fontconfig/fontconfig.h>

#include "embedded_font.h"
#endif

#ifdef XE_PLATFORM_LINUX
#include <gtk/gtk.h>
#endif

DEFINE_path(
    custom_font_path, "",
    "Allows user to load custom font and use it instead of default one.", "UI");

DEFINE_uint32(font_size, 14, "Allows user to set custom font size.", "UI");
UPDATE_from_uint32(font_size, 2024, 8, 31, 20, 12);

namespace xe {
namespace ui {

static_assert(sizeof(ImmediateVertex) == sizeof(ImDrawVert),
              "Vertex types must match");

ImGuiDrawer::ImGuiDrawer(xe::ui::Window* window, size_t z_order)
    : window_(window), z_order_(z_order) {
  Initialize();
}

ImGuiDrawer::~ImGuiDrawer() {
  SetPresenter(nullptr);
  if (!dialogs_.empty()) {
    window_->RemoveInputListener(this);
    if (internal_state_) {
      ImGui::SetCurrentContext(internal_state_);
      if (touch_pointer_id_ == TouchEvent::kPointerIDNone &&
          ImGui::IsAnyMouseDown()) {
        window_->ReleaseMouse();
      }
    }
  }
  if (internal_state_) {
    DestroyTextures();
    ImGui::DestroyContext(internal_state_);
    internal_state_ = nullptr;
  }
}

void ImGuiDrawer::AddDialog(ImGuiDialog* dialog) {
  assert_not_null(dialog);
  // Check if already added.
  if (std::ranges::find(std::as_const(dialogs_), dialog) != dialogs_.cend()) {
    return;
  }
  if (dialogs_.empty() && !IsDrawingDialogs()) {
    // First dialog added. !IsDrawingDialogs() is also checked because in a
    // situation of removing the only dialog, then adding a dialog, from within
    // a dialog's Draw function, re-registering the ImGuiDrawer may result in
    // ImGui being drawn multiple times in the current frame.
    window_->AddInputListener(this, z_order_);
    if (presenter_) {
      presenter_->AddUIDrawerFromUIThread(this, z_order_);
    }
  }
  dialogs_.push_back(dialog);
}

void ImGuiDrawer::RemoveDialog(ImGuiDialog* dialog) {
  assert_not_null(dialog);
  auto it = std::ranges::find(std::as_const(dialogs_), dialog);
  if (it == dialogs_.cend()) {
    return;
  }
  if (IsDrawingDialogs()) {
    // Actualize the next dialog index after the erasure from the vector.
    size_t existing_index = size_t(std::distance(dialogs_.cbegin(), it));
    if (dialog_loop_next_index_ > existing_index) {
      --dialog_loop_next_index_;
    }
  }
  dialogs_.erase(it);
  DetachIfLastWindowRemoved();
}

void ImGuiDrawer::AddNotification(ImGuiNotification* dialog) {
  assert_not_null(dialog);
  // Check if already added.
  if (std::ranges::find(std::as_const(notifications_), dialog) !=
      notifications_.cend()) {
    return;
  }
  if (notifications_.empty()) {
    if (presenter_) {
      presenter_->AddUIDrawerFromUIThread(this, z_order_);
    }
  }
  notifications_.push_back(dialog);
}

void ImGuiDrawer::RemoveNotification(ImGuiNotification* dialog) {
  assert_not_null(dialog);
  auto it = std::ranges::find(std::as_const(notifications_), dialog);
  if (it == notifications_.cend()) {
    return;
  }
  notifications_.erase(it);
  DetachIfLastWindowRemoved();
}

#ifdef XE_PLATFORM_LINUX
static void SetClipboardText(ImGuiContext* ctx, const char* text) {
  GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(clipboard, text, -1);
}

static const char* GetClipboardText(ImGuiContext* ctx) {
  // Must stay valid until the next call.
  static gchar* text = nullptr;
  g_free(text);

  GtkClipboard* clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  text = gtk_clipboard_wait_for_text(clipboard);
  return text;
}
#endif

void ImGuiDrawer::Initialize() {
  // Setup ImGui internal state.
  // This will give us state we can swap to the ImGui globals when in use.
  internal_state_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(internal_state_);

  auto& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

  // One font for all text; larger sizes, like titles, use PushFont.
  font_size_ = std::max((float)cvars::font_size, 8.f);
  InitializeFonts(font_size_);

#ifdef XE_PLATFORM_LINUX
  auto& platform_io = ImGui::GetPlatformIO();
  platform_io.Platform_SetClipboardTextFn = SetClipboardText;
  platform_io.Platform_GetClipboardTextFn = GetClipboardText;
#endif

  auto& style = ImGui::GetStyle();
  style.FontSizeBase = font_size_;
  style.ScrollbarRounding = 6.0f;
  style.WindowRounding = 6.0f;
  style.PopupRounding = 6.0f;
  style.TabRounding = 6.0f;

  style.Colors[ImGuiCol_Text] = ImVec4(0.89f, 0.90f, 0.90f, 1.00f);
  style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.06f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style.Colors[ImGuiCol_Border] = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.30f);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.80f, 0.80f, 0.40f);
  style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.65f, 0.65f, 0.45f);
  style.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.40f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.33f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.65f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.40f, 0.11f, 0.59f);
  style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.00f, 0.68f, 0.00f, 0.68f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] =
      ImVec4(0.00f, 1.00f, 0.15f, 0.62f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] =
      ImVec4(0.00f, 0.91f, 0.09f, 0.40f);
  style.Colors[ImGuiCol_PopupBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.99f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.74f, 0.90f, 0.72f, 0.50f);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
  style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.34f, 0.75f, 0.11f, 1.00f);
  style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.56f, 0.11f, 0.60f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.72f, 0.09f, 1.00f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.19f, 0.60f, 0.09f, 1.00f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.00f, 0.40f, 0.00f, 0.71f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.60f, 0.26f, 0.80f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.00f, 0.75f, 0.00f, 0.80f);
  style.Colors[ImGuiCol_Separator] = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.36f, 0.89f, 0.38f, 1.00f);
  style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.13f, 0.50f, 0.11f, 1.00f);
  style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
  style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
  style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
  style.Colors[ImGuiCol_Tab] = style.Colors[ImGuiCol_Button];
  style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_ButtonHovered];
  style.Colors[ImGuiCol_TabSelected] = style.Colors[ImGuiCol_ButtonActive];
  style.Colors[ImGuiCol_TabDimmed] = style.Colors[ImGuiCol_FrameBg];
  style.Colors[ImGuiCol_TabDimmedSelected] =
      style.Colors[ImGuiCol_FrameBgHovered];
  style.Colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogramHovered] =
      ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 1.00f, 0.00f, 0.21f);
  style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

  frame_time_tick_frequency_ = double(Clock::QueryHostTickFrequency());
  last_frame_time_ticks_ = Clock::QueryHostTickCount();

  touch_pointer_id_ = TouchEvent::kPointerIDNone;
  reset_mouse_position_after_next_frame_ = false;
}

void ImGuiDrawer::LoadInputSystem(hid::InputSystem* input_system) {
  if (input_system_) {
    return;
  }

  input_system_ = input_system;
}

void ImGuiDrawer::SetGuideButtonAction(std::function<void(uint8_t)> func) {
  onGuidePressFunction_ = func;
}

std::optional<ImGuiKey> ImGuiDrawer::VirtualKeyToImGuiKey(VirtualKey vkey) {
  static const std::map<VirtualKey, ImGuiKey> map = {
      {ui::VirtualKey::kTab, ImGuiKey_Tab},
      {ui::VirtualKey::kLeft, ImGuiKey_LeftArrow},
      {ui::VirtualKey::kRight, ImGuiKey_RightArrow},
      {ui::VirtualKey::kUp, ImGuiKey_UpArrow},
      {ui::VirtualKey::kDown, ImGuiKey_DownArrow},
      {ui::VirtualKey::kHome, ImGuiKey_Home},
      {ui::VirtualKey::kEnd, ImGuiKey_End},
      {ui::VirtualKey::kDelete, ImGuiKey_Delete},
      {ui::VirtualKey::kBack, ImGuiKey_Backspace},
      {ui::VirtualKey::kReturn, ImGuiKey_Enter},
      {ui::VirtualKey::kEscape, ImGuiKey_Escape},
      {ui::VirtualKey::kA, ImGuiKey_A},
      {ui::VirtualKey::kC, ImGuiKey_C},
      {ui::VirtualKey::kV, ImGuiKey_V},
      {ui::VirtualKey::kX, ImGuiKey_X},
      {ui::VirtualKey::kY, ImGuiKey_Y},
      {ui::VirtualKey::kZ, ImGuiKey_Z},
  };
  if (auto search = map.find(vkey); search != map.end()) {
    return search->second;
  } else {
    return std::nullopt;
  }
}

std::unique_ptr<ImmediateTexture> ImGuiDrawer::LoadImGuiIcon(
    std::span<const uint8_t> data) {
  if (!immediate_drawer_) {
    return {};
  }

  int width, height, channels;
  unsigned char* image_data =
      stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width,
                            &height, &channels, STBI_rgb_alpha);
  if (!image_data) {
    return {};
  }

  auto texture = immediate_drawer_->CreateTexture(
      width, height, ImmediateTextureFilter::kLinear, true,
      reinterpret_cast<uint8_t*>(image_data));

  stbi_image_free(image_data);

  return texture;
}

std::map<uint32_t, std::unique_ptr<ImmediateTexture>> ImGuiDrawer::LoadIcons(
    IconsData data) {
  std::map<uint32_t, std::unique_ptr<ImmediateTexture>> icons_;

  if (!immediate_drawer_) {
    return icons_;
  }

  for (const auto& icon : data) {
    icons_[icon.first] = LoadImGuiIcon(icon.second);
  }
  return icons_;
}

void ImGuiDrawer::SetupNotificationTextures() {
  if (!immediate_drawer_) {
    return;
  }

  // We're including 4th to include all visible
  for (uint8_t i = 0; i <= 4; i++) {
    if (notification_icons.size() < i) {
      break;
    }

    unsigned char* image_data;
    int width, height, channels;
    const auto user_icon = notification_icons.at(i);
    image_data =
        stbi_load_from_memory(user_icon.first, user_icon.second, &width,
                              &height, &channels, STBI_rgb_alpha);
    notification_icon_textures_.push_back(immediate_drawer_->CreateTexture(
        width, height, ImmediateTextureFilter::kLinear, true,
        reinterpret_cast<uint8_t*>(image_data)));

    stbi_image_free(image_data);
  }
}

#if XE_PLATFORM_WIN32 || XE_PLATFORM_LINUX
static ImFontConfig MakeCjkFontConfig() {
  ImFontConfig font_config;
  font_config.MergeMode = true;
  font_config.OversampleH = font_config.OversampleV = 2;
  font_config.PixelSnapH = true;
  font_config.RasterizerMultiply = 1.15f;
  return font_config;
}
#endif

#if XE_PLATFORM_WIN32
// Returns an empty path if the folder can't be found.
static std::filesystem::path GetWindowsFontsDir() {
  PWSTR fonts_dir = nullptr;
  std::filesystem::path path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &fonts_dir))) {
    path = fonts_dir;
  } else {
    XELOGW("Unable to find Windows fonts directory");
  }

  // Required even on failure.
  CoTaskMemFree(fonts_dir);
  return path;
}
#endif

// A font stb_truetype can't parse fails the whole atlas and asserts in debug
// builds, so check it first.
static bool IsFontLoadable(const uint8_t* data, size_t size) {
  if (!data || size < 12) {
    return false;
  }

  stbtt_fontinfo font_info = {};
  const int font_offset = stbtt_GetFontOffsetForIndex(data, 0);
  return font_offset >= 0 && stbtt_InitFont(&font_info, data, font_offset);
}

static bool IsFontFileLoadable(const std::filesystem::path& path) {
  auto font_data = MappedMemory::Open(path, MappedMemory::Mode::kRead);
  return font_data && IsFontLoadable(font_data->data(), font_data->size());
}

bool ImGuiDrawer::LoadCustomFont(ImGuiIO& io, ImFontConfig& font_config,
                                 float font_size) {
  if (cvars::custom_font_path.empty()) {
    return false;
  }

  const std::string font_path = xe::path_to_utf8(cvars::custom_font_path);
  if (!std::filesystem::exists(cvars::custom_font_path)) {
    XELOGW("Custom font {} doesn't exist; using the system font", font_path);
    return false;
  }

  if (!IsFontFileLoadable(cvars::custom_font_path)) {
    XELOGW(
        "Custom font {} can't be loaded, only TrueType fonts are supported; "
        "using the system font",
        font_path);
    return false;
  }

  ImFont* font =
      io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_size, &font_config);
  if (!font) {
    XELOGE("Failed to load custom font: {}", font_path);
    return false;
  }

  XELOGI("Using custom font {}", font_path);
  return true;
}

#if XE_PLATFORM_LINUX
// Returns the best TrueType match stb_truetype can load, or an empty string.
// It can't parse CFF or variable fonts, such as Fedora's NotoSansCJK-VF.ttc,
// which fontconfig may still return.
static std::string MatchFontconfigFont(FcPattern* pattern) {
  FcConfig* config = FcInitLoadConfigAndFonts();
  if (!config) {
    XELOGW("Unable to initialize fontconfig");
    return {};
  }

  FcPatternAddString(pattern, FC_FONTFORMAT,
                     reinterpret_cast<const FcChar8*>("TrueType"));
  FcConfigSubstitute(config, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  std::string font_path;
  FcResult result;
  FcPattern* match = FcFontMatch(config, pattern, &result);
  if (match) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
      font_path = reinterpret_cast<const char*>(file);
    }
    FcPatternDestroy(match);
  }
  FcConfigDestroy(config);

  if (font_path.empty() || !std::filesystem::exists(font_path)) {
    return {};
  }

  if (!IsFontFileLoadable(font_path)) {
    XELOGW("Font {} cannot be loaded by stb_truetype; skipping", font_path);
    return {};
  }
  return font_path;
}
#endif

bool ImGuiDrawer::LoadSystemFont(ImGuiIO& io, ImFontConfig& font_config,
                                 float font_size) {
#if XE_PLATFORM_WIN32
  std::filesystem::path font_path = GetWindowsFontsDir();
  if (font_path.empty()) {
    return false;
  }

  font_path /= "tahoma.ttf";
  if (!std::filesystem::exists(font_path)) {
    return false;
  }

  ImFont* font = io.Fonts->AddFontFromFileTTF(
      xe::path_to_utf8(font_path).c_str(), font_size, &font_config);
  if (!font) {
    XELOGE("Failed to load font: {}", font_path);
    return false;
  }
  return true;
#elif XE_PLATFORM_LINUX
  // Any distribution has a monospace font, which keeps UI columns even.
  FcPattern* pattern =
      FcNameParse(reinterpret_cast<const FcChar8*>("monospace"));
  if (!pattern) {
    return false;
  }

  const std::string font_path = MatchFontconfigFont(pattern);
  FcPatternDestroy(pattern);
  if (font_path.empty()) {
    XELOGW("Unable to find a system font; using the built-in one");
    return false;
  }

  ImFont* font =
      io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_size, &font_config);
  if (!font) {
    XELOGE("Failed to load font: {}", font_path);
    return false;
  }
  XELOGI("Using font {}", font_path);
  return true;
#endif

  return false;
}

void ImGuiDrawer::LoadJapaneseFont(ImGuiIO& io, float font_size) {
#if XE_PLATFORM_WIN32
  std::filesystem::path jp_font_path = GetWindowsFontsDir();
  if (jp_font_path.empty()) {
    return;
  }

  jp_font_path /= "msgothic.ttc";
  if (!std::filesystem::exists(jp_font_path)) {
    XELOGW("Unable to load Japanese font; JP characters will be boxes");
    return;
  }

  ImFontConfig jp_font_config = MakeCjkFontConfig();
  io.Fonts->AddFontFromFileTTF(xe::path_to_utf8(jp_font_path).c_str(),
                               font_size, &jp_font_config);
#elif XE_PLATFORM_LINUX
  if (IsFontLoadable(xe::ui::embedded_font::MPLUS1p_Regular_ttf_data,
                     xe::ui::embedded_font::MPLUS1p_Regular_ttf_size)) {
    ImFontConfig jp_font_config = MakeCjkFontConfig();
    jp_font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(
            xe::ui::embedded_font::MPLUS1p_Regular_ttf_data),
        static_cast<int>(xe::ui::embedded_font::MPLUS1p_Regular_ttf_size),
        font_size, &jp_font_config);
    return;
  }
  XELOGW("Bundled CJK font cannot be loaded; trying fontconfig");

  // Fall back to a system font that has a CJK character.
  FcPattern* pattern = FcPatternCreate();
  FcCharSet* charset = FcCharSetCreate();
  FcCharSetAddChar(charset, 0x4E00);
  FcPatternAddCharSet(pattern, FC_CHARSET, charset);
  FcCharSetDestroy(charset);  // The pattern holds its own reference.

  const std::string font_path = MatchFontconfigFont(pattern);
  FcPatternDestroy(pattern);

  if (!font_path.empty()) {
    ImFontConfig jp_font_config = MakeCjkFontConfig();
    if (io.Fonts->AddFontFromFileTTF(font_path.c_str(), font_size,
                                     &jp_font_config)) {
      XELOGI("Using CJK font {}", font_path);
      return;
    }
  }

  XELOGW(
      "Unable to find CJK font; Japanese characters may not display "
      "correctly");
#endif
}

void ImGuiDrawer::InitializeFonts(const float font_size) {
  auto& io = ImGui::GetIO();

  // TODO(gibbed): disable imgui.ini saving for now,
  // imgui assumes paths are char* so we can't throw a good path at it on
  // Windows.
  io.IniFilename = nullptr;

  ImFontConfig font_config;
  // Oversampled, as the UI is stretched to the window's DPI.
  font_config.OversampleH = font_config.OversampleV = 2;
  font_config.PixelSnapH = true;
  // Dear ImGui 1.92+ draws glyphs lighter; restore the earlier weight.
  font_config.RasterizerMultiply = 1.15f;

  bool is_font_loaded = LoadCustomFont(io, font_config, font_size);
  if (!is_font_loaded) {
    is_font_loaded = LoadSystemFont(io, font_config, font_size);
  }

  if (!is_font_loaded) {
    ImFontConfig default_font_config = font_config;
    default_font_config.SizePixels = font_size;
    io.Fonts->AddFontDefaultVector(&default_font_config);
  }

  LoadJapaneseFont(io, font_size);
}

void ImGuiDrawer::UpdateTexture(ImTextureData* texture) {
  auto* immediate_texture =
      static_cast<ImmediateTexture*>(texture->BackendUserData);
  switch (texture->Status) {
    case ImTextureStatus_WantCreate:
    case ImTextureStatus_WantUpdates: {
      // The immediate drawer can't update part of a texture, so recreate it.
      // The old one is kept until the GPU is done with it.
      assert_true(texture->Format == ImTextureFormat_RGBA32);
      std::unique_ptr<ImmediateTexture> new_texture =
          immediate_drawer_->CreateTexture(
              uint32_t(texture->Width), uint32_t(texture->Height),
              ImmediateTextureFilter::kLinear, true,
              static_cast<const uint8_t*>(texture->GetPixels()));
      if (!new_texture) {
        XELOGE("Failed to create a {}x{} Dear ImGui texture", texture->Width,
               texture->Height);
        return;
      }

      delete immediate_texture;
      immediate_texture = new_texture.release();
      texture->BackendUserData = immediate_texture;
      texture->SetTexID(reinterpret_cast<ImTextureID>(immediate_texture));
      texture->SetStatus(ImTextureStatus_OK);
    } break;
    case ImTextureStatus_WantDestroy:
      delete immediate_texture;
      texture->BackendUserData = nullptr;
      texture->SetTexID(ImTextureID_Invalid);
      texture->SetStatus(ImTextureStatus_Destroyed);
      break;
    default:
      break;
  }
}

void ImGuiDrawer::DestroyTextures() {
  ImGui::SetCurrentContext(internal_state_);
  for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
    if (!texture->BackendUserData) {
      continue;
    }

    delete static_cast<ImmediateTexture*>(texture->BackendUserData);
    texture->BackendUserData = nullptr;
    texture->SetTexID(ImTextureID_Invalid);

    // Recreated by the next immediate drawer if still needed.
    texture->SetStatus(texture->Pixels ? ImTextureStatus_WantCreate
                                       : ImTextureStatus_Destroyed);
  }
}

void ImGuiDrawer::SetPresenter(Presenter* new_presenter) {
  if (presenter_) {
    if (presenter_ == new_presenter) {
      return;
    }
    if (!dialogs_.empty()) {
      presenter_->RemoveUIDrawerFromUIThread(this);
    }
  }
  presenter_ = new_presenter;
  if (presenter_) {
    if (!dialogs_.empty()) {
      presenter_->AddUIDrawerFromUIThread(this, z_order_);
    }
  }
}

void ImGuiDrawer::SetImmediateDrawer(ImmediateDrawer* new_immediate_drawer) {
  if (immediate_drawer_ == new_immediate_drawer) {
    return;
  }
  if (immediate_drawer_) {
    DestroyTextures();
    locked_achievement_icon_.reset();
    notification_icon_textures_.clear();
  }
  immediate_drawer_ = new_immediate_drawer;
  if (immediate_drawer_) {
    SetupNotificationTextures();

    // Load locked achievement icon.
    unsigned char* image_data;
    int width, height, channels;
    image_data = stbi_load_from_memory(locked_achievement_icon.first,
                                       locked_achievement_icon.second, &width,
                                       &height, &channels, STBI_rgb_alpha);
    locked_achievement_icon_ = immediate_drawer_->CreateTexture(
        width, height, ImmediateTextureFilter::kLinear, true,
        reinterpret_cast<uint8_t*>(image_data));

    stbi_image_free(image_data);
  }
}

void ImGuiDrawer::Draw(UIDrawContext& ui_draw_context) {
  // Drawing of anything is initiated by the presenter.
  assert_not_null(presenter_);
  if (!immediate_drawer_) {
    // A presenter has been attached, but an immediate drawer hasn't been
    // attached yet.
    return;
  }

  if (dialogs_.empty() && notifications_.empty()) {
    return;
  }

  ImGui::SetCurrentContext(internal_state_);

  ImGuiIO& io = ImGui::GetIO();

  uint64_t current_frame_time_ticks = Clock::QueryHostTickCount();
  io.DeltaTime =
      float(double(current_frame_time_ticks - last_frame_time_ticks_) /
            frame_time_tick_frequency_);
  if (!(io.DeltaTime > 0.0f) ||
      current_frame_time_ticks < last_frame_time_ticks_) {
    // For safety as Dear ImGui doesn't allow non-positive DeltaTime. Using the
    // same default value as in the official samples.
    io.DeltaTime = 1.0f / 60.0f;
  }
  last_frame_time_ticks_ = current_frame_time_ticks;

  float physical_to_logical =
      float(window_->GetMediumDpi()) / float(window_->GetDpi());
  // Whole pixels, or Dear ImGui anti-aliases the edges of the modal dimming.
  io.DisplaySize.x =
      std::round(window_->GetActualPhysicalWidth() * physical_to_logical);
  io.DisplaySize.y =
      std::round(window_->GetActualPhysicalHeight() * physical_to_logical);

  if (!dialogs_.empty()) {
    UpdateGamepads();
  }

  ImGui::NewFrame();

  assert_true(!IsDrawingDialogs());
  dialog_loop_next_index_ = 0;
  while (dialog_loop_next_index_ < dialogs_.size()) {
    dialogs_[dialog_loop_next_index_++]->Draw();
  }
  dialog_loop_next_index_ = SIZE_MAX;

  if (!notifications_.empty() && are_notifications_enabled_) {
    auto guest_notifications =
        notifications_ | std::views::filter([](auto* notification) {
          return notification->GetNotificationType() == NotificationType::Guest;
        });

    auto host_notifications =
        notifications_ | std::views::filter([](auto* notification) {
          return notification->GetNotificationType() == NotificationType::Host;
        });

    if (!guest_notifications.empty()) {
      guest_notifications.front()->Draw();
    }

    if (!host_notifications.empty()) {
      host_notifications.front()->Draw();

      if (std::ranges::distance(host_notifications) > 1) {
        host_notifications.front()->SetDeletionPending();
      }
    }
  }

  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();
  if (draw_data) {
    RenderDrawLists(draw_data, ui_draw_context);
  }

  if (reset_mouse_position_after_next_frame_) {
    reset_mouse_position_after_next_frame_ = false;
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
  }

  // Detaching is deferred if the last dialog is removed during drawing, perform
  // it now if needed.
  DetachIfLastWindowRemoved();

  if (!dialogs_.empty() || !notifications_.empty()) {
    // Repaint (and handle input) continuously if still active.
    presenter_->RequestUIPaintFromUIThread();
  }
}

void ImGuiDrawer::ClearDialogs() {
  size_t dialog_loop = 0;

  while (dialog_loop < dialogs_.size()) {
    RemoveDialog(dialogs_[dialog_loop++]);
  }
}

void ImGuiDrawer::RenderDrawLists(ImDrawData* data,
                                  UIDrawContext& ui_draw_context) {
  ImGuiIO& io = ImGui::GetIO();

  if (data->Textures) {
    for (ImTextureData* texture : *data->Textures) {
      if (texture->Status != ImTextureStatus_OK) {
        UpdateTexture(texture);
      }
    }
  }

  immediate_drawer_->Begin(ui_draw_context, io.DisplaySize.x, io.DisplaySize.y);

  for (int i = 0; i < data->CmdLists.Size; ++i) {
    const auto cmd_list = data->CmdLists[i];

    ImmediateDrawBatch batch;
    batch.vertices =
        reinterpret_cast<ImmediateVertex*>(cmd_list->VtxBuffer.Data);
    batch.vertex_count = cmd_list->VtxBuffer.size();
    batch.indices = cmd_list->IdxBuffer.Data;
    batch.index_count = cmd_list->IdxBuffer.size();
    immediate_drawer_->BeginDrawBatch(batch);

    for (int j = 0; j < cmd_list->CmdBuffer.size(); ++j) {
      const auto& cmd = cmd_list->CmdBuffer[j];

      ImmediateDraw draw;
      draw.primitive_type = ImmediatePrimitiveType::kTriangles;
      draw.count = cmd.ElemCount;
      draw.index_offset = cmd.IdxOffset;
      draw.texture = reinterpret_cast<ImmediateTexture*>(cmd.GetTexID());
      draw.scissor = true;
      draw.scissor_left = cmd.ClipRect.x;
      draw.scissor_top = cmd.ClipRect.y;
      draw.scissor_right = cmd.ClipRect.z;
      draw.scissor_bottom = cmd.ClipRect.w;
      immediate_drawer_->Draw(draw);
    }

    immediate_drawer_->EndDrawBatch();
  }

  immediate_drawer_->End();
}

ImGuiIO& ImGuiDrawer::GetIO() {
  ImGui::SetCurrentContext(internal_state_);
  return ImGui::GetIO();
}

void ImGuiDrawer::OnKeyDown(KeyEvent& e) { OnKey(e, true); }

void ImGuiDrawer::OnKeyUp(KeyEvent& e) { OnKey(e, false); }

void ImGuiDrawer::OnKeyChar(KeyEvent& e) {
  auto& io = GetIO();
  // TODO(Triang3l): Accept the Unicode character.
  unsigned int character = static_cast<unsigned int>(e.unicode());
  if (character > 0 && character < 0x10000) {
    io.AddInputCharacter(character);
    e.set_handled(true);
  }
}

void ImGuiDrawer::OnMouseDown(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
  auto& io = GetIO();
  int button = -1;
  switch (e.button()) {
    case xe::ui::MouseEvent::Button::kLeft: {
      button = 0;
      break;
    }
    case xe::ui::MouseEvent::Button::kRight: {
      button = 1;
      break;
    }
    default: {
      // Ignored.
      break;
    }
  }
  if (button >= 0 && button < std::size(io.MouseDown)) {
    if (!io.MouseDown[button]) {
      if (!ImGui::IsAnyMouseDown()) {
        window_->CaptureMouse();
      }
      io.MouseDown[button] = true;
    }
  }
}

void ImGuiDrawer::OnMouseMove(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
}

void ImGuiDrawer::OnMouseUp(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
  auto& io = GetIO();
  int button = -1;
  switch (e.button()) {
    case xe::ui::MouseEvent::Button::kLeft: {
      button = 0;
      break;
    }
    case xe::ui::MouseEvent::Button::kRight: {
      button = 1;
      break;
    }
    default: {
      // Ignored.
      break;
    }
  }
  if (button >= 0 && button < std::size(io.MouseDown)) {
    if (io.MouseDown[button]) {
      io.MouseDown[button] = false;
      if (!ImGui::IsAnyMouseDown()) {
        window_->ReleaseMouse();
      }
    }
  }
}

void ImGuiDrawer::OnMouseWheel(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
  auto& io = GetIO();
  io.MouseWheel += float(e.scroll_y()) / float(MouseEvent::kScrollPerDetent);
}

void ImGuiDrawer::OnTouchEvent(TouchEvent& e) {
  auto& io = GetIO();
  TouchEvent::Action action = e.action();
  uint32_t pointer_id = e.pointer_id();
  if (action == TouchEvent::Action::kDown) {
    // The latest pointer needs to be controlling the ImGui mouse.
    if (touch_pointer_id_ == TouchEvent::kPointerIDNone) {
      // Switching from the mouse to touch input.
      if (ImGui::IsAnyMouseDown()) {
        std::memset(io.MouseDown, 0, sizeof(io.MouseDown));
        window_->ReleaseMouse();
      }
    }
    touch_pointer_id_ = pointer_id;
  } else {
    if (pointer_id != touch_pointer_id_) {
      return;
    }
  }
  UpdateMousePosition(e.x(), e.y());
  if (action == TouchEvent::Action::kUp ||
      action == TouchEvent::Action::kCancel) {
    io.MouseDown[0] = false;
    touch_pointer_id_ = TouchEvent::kPointerIDNone;
    // Make sure that after a touch, the ImGui mouse isn't hovering over
    // anything.
    reset_mouse_position_after_next_frame_ = true;
  } else {
    io.MouseDown[0] = true;
    reset_mouse_position_after_next_frame_ = false;
  }
}

void ImGuiDrawer::ClearInput() {
  auto& io = GetIO();
  if (touch_pointer_id_ == TouchEvent::kPointerIDNone &&
      ImGui::IsAnyMouseDown()) {
    window_->ReleaseMouse();
  }
  io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
  std::memset(io.MouseDown, 0, sizeof(io.MouseDown));
  io.ClearInputKeys();
  touch_pointer_id_ = TouchEvent::kPointerIDNone;
  reset_mouse_position_after_next_frame_ = false;
}

void ImGuiDrawer::OnKey(KeyEvent& e, bool is_down) {
  auto& io = GetIO();
  const VirtualKey virtual_key = e.virtual_key();
  if (auto imGuiKey = VirtualKeyToImGuiKey(virtual_key); imGuiKey) {
    io.AddKeyEvent(*imGuiKey, is_down);
  }
  switch (virtual_key) {
    case VirtualKey::kShift:
      io.KeyShift = is_down;
      break;
    case VirtualKey::kControl:
      io.KeyCtrl = is_down;
      break;
    case VirtualKey::kMenu:
      // FIXME(Triang3l): Doesn't work in xenia-ui-window-demo.
      io.KeyAlt = is_down;
      break;
    case VirtualKey::kLWin:
      io.KeySuper = is_down;
      break;
    default:
      break;
  }
}

void ImGuiDrawer::UpdateMousePosition(float x, float y) {
  auto& io = GetIO();
  float physical_to_logical =
      float(window_->GetMediumDpi()) / float(window_->GetDpi());
  io.MousePos.x = x * physical_to_logical;
  io.MousePos.y = y * physical_to_logical;
}

void ImGuiDrawer::SwitchToPhysicalMouseAndUpdateMousePosition(
    const MouseEvent& e) {
  if (touch_pointer_id_ != TouchEvent::kPointerIDNone) {
    touch_pointer_id_ = TouchEvent::kPointerIDNone;
    auto& io = GetIO();
    std::memset(io.MouseDown, 0, sizeof(io.MouseDown));
    // Nothing needs to be done regarding CaptureMouse and ReleaseMouse - all
    // buttons as well as mouse capture have been released when switching to
    // touch input, the mouse is never captured during touch input, and now
    // resetting to no buttons down (therefore not capturing).
  }
  reset_mouse_position_after_next_frame_ = false;
  UpdateMousePosition(float(e.x()), float(e.y()));
}

void ImGuiDrawer::DetachIfLastWindowRemoved() {
  // IsDrawingDialogs() is also checked because in a situation of removing the
  // only dialog, then adding a dialog, from within a dialog's Draw function,
  // re-registering the ImGuiDrawer may result in ImGui being drawn multiple
  // times in the current frame.
  if (!dialogs_.empty() || !notifications_.empty() || IsDrawingDialogs()) {
    return;
  }
  if (presenter_) {
    presenter_->RemoveUIDrawerFromUIThread(this);
  }
  window_->RemoveInputListener(this);
  // Clear all input since no input will be received anymore, and when the
  // drawer becomes active again, it'd have an outdated input state otherwise
  // which will be persistent until new events actualize individual input
  // properties.
  ClearInput();
}

void ImGuiDrawer::UpdateGamepads() {
  if (!input_system_) {
    return;
  }

  hid::X_INPUT_CAPABILITIES caps = {};

  bool is_gamepad_connected = false;

  for (uint8_t i = 0; i < XUserMaxUserCount; i++) {
    if (input_system_->GetCapabilities(i, 1, &caps) == X_ERROR_SUCCESS) {
      // Special case to skip keyboard being set in gamepad mode.
      if (caps.gamepad.buttons == 0xFFFF &&
          caps.vibration.left_motor_speed == 0 &&
          caps.vibration.right_motor_speed == 0) {
        continue;
      }

      is_gamepad_connected = true;
      break;
    }
  }

  auto& io = GetIO();

  if (!is_gamepad_connected) {
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    return;
  }

  uint8_t controller_to_poke = XUserIndexNone;
  hid::X_INPUT_STATE gamepad_state;
  for (uint8_t i = 0; i < XUserMaxUserCount; i++) {
    if (input_system_->GetState(i, 1, &gamepad_state) == X_ERROR_SUCCESS) {
      if (gamepad_state.gamepad.buttons != 0) {
        controller_to_poke = i;
        break;
      }
    }
  }
  if (controller_to_poke == XUserIndexNone) {
    io.ClearInputKeys();
    return;
  }

  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  hid::X_INPUT_GAMEPAD& gamepad = gamepad_state.gamepad;

  // GUIDE BUTTON - More info needed
  if (gamepad_state.gamepad.buttons ==
      hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_GUIDE) {
    if (onGuidePressFunction_) {
      onGuidePressFunction_(controller_to_poke);
    }
  }

#define IM_SATURATE(V) (V < 0.0f ? 0.0f : V > 1.0f ? 1.0f : V)
#define MAP_BUTTON(KEY_NO, BUTTON_ENUM)                           \
  {                                                               \
    io.AddKeyEvent(KEY_NO, (gamepad.buttons & BUTTON_ENUM) != 0); \
  }
#define MAP_ANALOG(KEY_NO, VALUE, V0, V1)                      \
  {                                                            \
    float vn = (float)(VALUE - V0) / (float)(V1 - V0);         \
    io.AddKeyAnalogEvent(KEY_NO, vn > 0.10f, IM_SATURATE(vn)); \
  }

  MAP_BUTTON(ImGuiKey_GamepadStart,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_START);
  MAP_BUTTON(ImGuiKey_GamepadBack,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_BACK);
  MAP_BUTTON(ImGuiKey_GamepadFaceLeft,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_X);
  MAP_BUTTON(ImGuiKey_GamepadFaceRight,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_B);
  MAP_BUTTON(ImGuiKey_GamepadFaceUp,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_Y);
  MAP_BUTTON(ImGuiKey_GamepadFaceDown,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_A);
  MAP_BUTTON(ImGuiKey_GamepadDpadLeft,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_DPAD_LEFT);
  MAP_BUTTON(ImGuiKey_GamepadDpadRight,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_DPAD_RIGHT);
  MAP_BUTTON(ImGuiKey_GamepadDpadUp,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_DPAD_UP);
  MAP_BUTTON(ImGuiKey_GamepadDpadDown,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_DPAD_DOWN);
  MAP_BUTTON(ImGuiKey_GamepadL1,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_LEFT_SHOULDER);
  MAP_BUTTON(ImGuiKey_GamepadR1,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  MAP_ANALOG(ImGuiKey_GamepadL2, gamepad.left_trigger,
             hid::X_INPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
  MAP_ANALOG(ImGuiKey_GamepadR2, gamepad.right_trigger,
             hid::X_INPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
  MAP_BUTTON(ImGuiKey_GamepadL3,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_LEFT_THUMB);
  MAP_BUTTON(ImGuiKey_GamepadR3,
             hid::X_INPUT_GAMEPAD_BUTTON::X_INPUT_GAMEPAD_RIGHT_THUMB);
  MAP_ANALOG(ImGuiKey_GamepadLStickLeft, gamepad.thumb_lx,
             -hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
  MAP_ANALOG(ImGuiKey_GamepadLStickRight, gamepad.thumb_lx,
             +hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
  MAP_ANALOG(ImGuiKey_GamepadLStickUp, gamepad.thumb_ly,
             +hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
  MAP_ANALOG(ImGuiKey_GamepadLStickDown, gamepad.thumb_ly,
             -hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
  MAP_ANALOG(ImGuiKey_GamepadRStickLeft, gamepad.thumb_rx,
             -hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
  MAP_ANALOG(ImGuiKey_GamepadRStickRight, gamepad.thumb_rx,
             +hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
  MAP_ANALOG(ImGuiKey_GamepadRStickUp, gamepad.thumb_ry,
             +hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
  MAP_ANALOG(ImGuiKey_GamepadRStickDown, gamepad.thumb_ry,
             -hid::X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
#undef MAP_BUTTON
#undef MAP_ANALOG
}

}  // namespace ui
}  // namespace xe
