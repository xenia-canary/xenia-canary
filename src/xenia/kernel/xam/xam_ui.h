/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_H_
#define XENIA_KERNEL_XAM_XAM_UI_H_

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace kernel {
namespace xam {

class XamDialog : public xe::ui::ImGuiDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamDialog(xe::ui::ImGuiDrawer* imgui_drawer)
      : xe::ui::ImGuiDialog(imgui_drawer) {}

  virtual ~XamDialog() {}
  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

class MessageBoxDialog : public XamDialog {
 public:
  MessageBoxDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                   std::string& description, std::vector<std::string> buttons,
                   uint32_t default_button)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }

  void OnDraw(ImGuiIO& io) override;
  virtual ~MessageBoxDialog() {}

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
};

class ProfilePasscodeDialog : public XamDialog {
 public:
  ProfilePasscodeDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                        std::string& description, MESSAGEBOX_RESULT* result_ptr)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        result_ptr_(result_ptr) {
    std::memset(result_ptr, 0, sizeof(MESSAGEBOX_RESULT));

    if (title_.empty()) {
      title_ = "Enter Pass Code";
    }

    if (description_.empty()) {
      description_ = "Enter your Xbox LIVE pass code.";
    }
  }

  void DrawPasscodeField(uint8_t key_id);

  void OnDraw(ImGuiIO& io) override;

  virtual ~ProfilePasscodeDialog() {}

  bool SelectedSignedIn() const { return selected_signed_in_; }

 private:
  const char* labelled_keys_[11] = {"None", "X",  "Y",    "RB",   "LB",   "LT",
                                    "RT",   "Up", "Down", "Left", "Right"};

  const std::map<std::string, uint16_t> keys_map_ = {
      {"None", 0},
      {"X", X_BUTTON_PASSCODE},
      {"Y", Y_BUTTON_PASSCODE},
      {"RB", RIGHT_BUMPER_PASSCODE},
      {"LB", LEFT_BUMPER_PASSCODE},
      {"LT", LEFT_TRIGGER_PASSCODE},
      {"RT", RIGHT_TRIGGER_PASSCODE},
      {"Up", DPAD_UP_PASSCODE},
      {"Down", DPAD_DOWN_PASSCODE},
      {"Left", DPAD_LEFT_PASSCODE},
      {"Right", DPAD_RIGHT_PASSCODE}};

  bool has_opened_ = false;
  bool selected_signed_in_ = false;
  std::string title_;
  std::string description_;

  static constexpr uint8_t passcode_length = sizeof(X_XAMACCOUNTINFO::passcode);
  int key_indexes_[passcode_length] = {0, 0, 0, 0};
  MESSAGEBOX_RESULT* result_ptr_;
};

class GameAchievementsDialog final : public XamDialog {
 public:
  GameAchievementsDialog(ui::ImGuiDrawer* imgui_drawer,
                         const ImVec2 drawing_position,
                         const TitleInfo* title_info,
                         const UserProfile* profile)
      : XamDialog(imgui_drawer),
        drawing_position_(drawing_position),
        title_info_(*title_info),
        profile_(profile),
        window_id_(GetWindowId()) {
    LoadAchievementsData();
  }

 private:
  ~GameAchievementsDialog() {
    for (auto& entry : achievements_icons_) {
      entry.second.release();
    }
  }
  bool LoadAchievementsData();

  std::string GetAchievementTitle(const Achievement& achievement_entry) const;

  std::string GetAchievementDescription(
      const Achievement& achievement_entry) const;

  ui::ImmediateTexture* GetIcon(const Achievement& achievement_entry) const;

  std::string GetUnlockedTime(const Achievement& achievement_entry) const;

  void DrawTitleAchievementInfo(ImGuiIO& io,
                                const Achievement& achievement_entry) const;

  void OnDraw(ImGuiIO& io) override;

 private:
  bool show_locked_info_ = false;

  uint64_t window_id_;
  const ImVec2 drawing_position_ = {};

  const TitleInfo title_info_;
  const UserProfile* profile_;

  std::vector<Achievement> achievements_info_;
  std::map<uint32_t, std::unique_ptr<ui::ImmediateTexture>> achievements_icons_;
};

class GamesInfoDialog final : public XamDialog {
 public:
  GamesInfoDialog(ui::ImGuiDrawer* imgui_drawer, const ImVec2 drawing_position,
                  const UserProfile* profile)
      : XamDialog(imgui_drawer),
        drawing_position_(drawing_position),
        profile_(profile),
        profile_manager_(kernel_state()->xam_state()->profile_manager()),
        dialog_name_(fmt::format("{}'s Games List###{}", profile->name(),
                                 GetWindowId())) {
    LoadProfileGameInfo(imgui_drawer, profile);
  }

 private:
  ~GamesInfoDialog() {
    for (auto& entry : title_icon) {
      entry.second.release();
    }
  }
  void LoadProfileGameInfo(ui::ImGuiDrawer* imgui_drawer,
                           const UserProfile* profile);

  void DrawTitleEntry(ImGuiIO& io, TitleInfo& entry);

  void OnDraw(ImGuiIO& io) override;

  static constexpr uint8_t title_name_filter_size = 15;

  std::string dialog_name_ = "";
  char title_name_filter_[title_name_filter_size] = "";
  uint32_t selected_title_ = 0;
  const ImVec2 drawing_position_ = {};

  const UserProfile* profile_;
  const ProfileManager* profile_manager_;

  std::map<uint32_t, std::unique_ptr<ui::ImmediateTexture>> title_icon;
  std::vector<TitleInfo> info_;
};

class SigninDialog : public XamDialog {
 public:
  SigninDialog(xe::ui::ImGuiDrawer* imgui_drawer, uint32_t users_needed)
      : XamDialog(imgui_drawer),
        users_needed_(users_needed),
        title_("Sign In") {
    Initialize();
  }

  virtual ~SigninDialog() {}

  void OnDraw(ImGuiIO& io) override;

 private:
  void Initialize();
  void ReloadProfiles(bool first_draw);

  bool has_opened_ = false;
  std::string title_;
  uint32_t users_needed_ = 1;
  uint32_t last_user_ = 0;

  std::vector<std::pair<uint8_t, std::string>> slot_data_;
  std::vector<std::pair<uint64_t, std::string>> profile_data_;
  uint8_t chosen_slots_[XUserMaxUserCount] = {};
  uint64_t chosen_xuids_[XUserMaxUserCount] = {};

  bool creating_profile_ = false;
  char gamertag_[16] = "";
};

class CreateProfileDialog final : public XamDialog {
 public:
  CreateProfileDialog(ui::ImGuiDrawer* imgui_drawer, Emulator* emulator,
                      bool with_migration = false)
      : XamDialog(imgui_drawer),
        emulator_(emulator),
        migration_(with_migration) {
    memset(gamertag_, 0, sizeof(gamertag_));
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

  bool has_opened_ = false;
  bool migration_ = false;
  char gamertag_[16] = "";
  Emulator* emulator_;
};

class KeyboardInputDialog : public XamDialog {
 public:
  KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer, std::string& title,
                      std::string& description, std::string& default_text,
                      size_t max_length)
      : XamDialog(imgui_drawer),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_length_(max_length),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    text_buffer_.resize(max_length);
    xe::string_util::copy_truncating(text_buffer_.data(), default_text_,
                                     text_buffer_.size());
  }
  virtual ~KeyboardInputDialog() {}

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  size_t max_length_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
};

bool xeDrawProfileContent(ui::ImGuiDrawer* imgui_drawer, const uint64_t xuid,
                          const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
