/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/file_picker.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>

#include "xenia/base/platform.h"
#include "xenia/ui/window_wx.h"

#if XE_PLATFORM_WIN32
// Wine fallback; only built into wxcore on Windows
// (third_party/CMakeLists.txt forces filedlgg.cpp into the wxcore sources).
#include <wx/generic/filedlgg.h>
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace ui {

// ToStdString() goes via the system codepage and loses non-ASCII paths.
static std::filesystem::path PathFromWx(const wxString& s) {
#if XE_PLATFORM_WIN32
  return std::filesystem::path(s.wc_str());
#else
  return std::filesystem::path(s.utf8_string());
#endif
}

#if XE_PLATFORM_WIN32
static bool IsRunningUnderWine() {
  static int cached = -1;
  if (cached < 0) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    cached = ntdll && GetProcAddress(ntdll, "wine_get_version") ? 1 : 0;
  }
  return cached != 0;
}
#endif

class WxFilePicker : public FilePicker {
 public:
  bool Show(Window* parent_window) override;

 private:
  wxString BuildWildcard() const;
};

wxString WxFilePicker::BuildWildcard() const {
  auto exts = extensions();
  if (exts.empty()) {
    return _("All Files") + wxT("|*.*");
  }
  wxString wildcard;
  for (size_t i = 0; i < exts.size(); ++i) {
    if (i > 0) wildcard += wxT("|");
    wildcard += wxString::FromUTF8(exts[i].first) + wxT("|") +
                wxString::FromUTF8(exts[i].second);
  }
  return wildcard;
}

bool WxFilePicker::Show(Window* parent_window) {
  wxWindow* parent = nullptr;
  if (parent_window) {
    auto* wx_window = static_cast<WxWindow*>(parent_window);
    parent = wx_window->frame();
  }

  // path::u8string() is std::u8string in C++20; wx 3.2 takes const char*.
  auto initial_dir_u8 = initial_directory().u8string();
  wxString default_dir =
      wxString::FromUTF8(reinterpret_cast<const char*>(initial_dir_u8.c_str()),
                         initial_dir_u8.size());

  if (type() == Type::kDirectory) {
    wxDirDialog dialog(parent, wxString::FromUTF8(title()), default_dir,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) {
      std::vector<std::filesystem::path> paths;
      paths.emplace_back(PathFromWx(dialog.GetPath()));
      set_selected_files(std::move(paths));
      return true;
    }
    return false;
  }

  long style = 0;
  if (mode() == Mode::kOpen) {
    style = wxFD_OPEN | wxFD_FILE_MUST_EXIST;
    if (multi_selection()) {
      style |= wxFD_MULTIPLE;
    }
  } else {
    style = wxFD_SAVE | wxFD_OVERWRITE_PROMPT;
  }

  int result = wxID_CANCEL;
  wxString selected_path;
  wxArrayString selected_paths;

#if XE_PLATFORM_WIN32
  if (IsRunningUnderWine()) {
    wxGenericFileDialog dialog(parent, wxString::FromUTF8(title()), default_dir,
                               wxString::FromUTF8(file_name()), BuildWildcard(),
                               style);
    result = dialog.ShowModal();
    if (result == wxID_OK) {
      // wxFileDialog asserts if GetPath() is called with wxFD_MULTIPLE.
      if (multi_selection() && mode() == Mode::kOpen) {
        dialog.GetPaths(selected_paths);
      } else {
        selected_path = dialog.GetPath();
      }
    }
  } else
#endif
  {
    wxFileDialog dialog(parent, wxString::FromUTF8(title()), default_dir,
                        wxString::FromUTF8(file_name()), BuildWildcard(),
                        style);
    result = dialog.ShowModal();
    if (result == wxID_OK) {
      if (multi_selection() && mode() == Mode::kOpen) {
        dialog.GetPaths(selected_paths);
      } else {
        selected_path = dialog.GetPath();
      }
    }
  }

  if (result == wxID_OK) {
    std::vector<std::filesystem::path> paths;
    if (!selected_paths.empty()) {
      paths.reserve(selected_paths.size());
      for (const auto& p : selected_paths) {
        paths.emplace_back(PathFromWx(p));
      }
    } else {
      paths.emplace_back(PathFromWx(selected_path));
    }
    set_selected_files(std::move(paths));
    return true;
  }
  return false;
}

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<WxFilePicker>();
}

}  // namespace ui
}  // namespace xe
