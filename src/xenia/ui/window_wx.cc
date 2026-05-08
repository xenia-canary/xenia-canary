/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/window_wx.h"

#include <wx/config.h>
#include <wx/persist.h>
#include <wx/persist/toplevel.h>

#include <cstring>
#include <vector>

#include "xenia/base/cvar.h"

DEFINE_bool(disable_doubleclick_fullscreen, false,
            "Allows the user to disable the behavior where a fast double-click "
            "causes Xenia to enter fullscreen mode.",
            "General");

#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/mstream.h>

#include "embedded_app_icon.h"
#include "xenia/base/logging.h"
#include "xenia/ui/menu_item_wx.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/windowed_app_context_wx.h"

#if XE_PLATFORM_WIN32
#include "xenia/ui/surface_win.h"
#elif XE_PLATFORM_LINUX
#include <gtk/gtk.h>
#include "xenia/ui/surface_gnulinux.h"
#elif XE_PLATFORM_MAC
#include "xenia/ui/surface_mac.h"
#endif

namespace xe {
namespace ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<WxWindow>(app_context, title, desired_logical_width,
                                    desired_logical_height);
}

#if XE_PLATFORM_WIN32
class WxWindow::XeniaFrame : public wxFrame {
 public:
  using wxFrame::wxFrame;
};

class WxWindow::RenderPanel : public wxWindow {
 public:
  using wxWindow::wxWindow;

  void set_owner(WxWindow* owner) { owner_ = owner; }

  WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam,
                          WXLPARAM lParam) override {
    switch (nMsg) {
      case WM_PAINT:
        ValidateRect(GetHWND(), nullptr);
        if (owner_) {
          owner_->OnPaint(true);
          if (owner_->HasSurface()) {
            owner_->DropStartupCoverIfPresent();
          }
        }
        return 0;
      case WM_ERASEBKGND:
        if (owner_ && owner_->HasSurface()) {
          return 0;
        }
        break;
    }
    return wxWindow::MSWWindowProc(nMsg, wParam, lParam);
  }

 private:
  WxWindow* owner_ = nullptr;
};
#endif

wxWindow* WxWindow::render_target() const { return render_panel_; }

void WxWindow::DropStartupCoverIfPresent() {
  if (startup_cover_) {
    startup_cover_->Destroy();
    startup_cover_ = nullptr;
  }
}

namespace {
constexpr char kGeometryKeyX[] = "/window/x";
constexpr char kGeometryKeyY[] = "/window/y";
constexpr char kGeometryKeyMaximized[] = "/window/maximized";
}  // namespace

static wxPoint ReadInitialFramePositionFromConfig() {
  auto* config = wxConfigBase::Get();
  if (!config) return wxDefaultPosition;
  long x = 0, y = 0;
  if (config->Read(kGeometryKeyX, &x) && config->Read(kGeometryKeyY, &y)) {
    return wxPoint(int(x), int(y));
  }
  return wxDefaultPosition;
}

void WxWindow::SaveGeometryToConfig() {
  if (!frame_ || frame_->IsFullScreen()) return;
  auto* config = wxConfigBase::Get();
  if (!config) return;
  bool maximized = frame_->IsMaximized();
  config->Write(kGeometryKeyMaximized, maximized);
  if (!maximized) {
    wxPoint pos = frame_->GetPosition();
    config->Write(kGeometryKeyX, pos.x);
    config->Write(kGeometryKeyY, pos.y);
  }
  config->Flush();
}

void WxWindow::RestoreGeometryFromConfig() {
  // Position is applied at frame construction; only maximized state remains.
  if (!frame_) return;
  auto* config = wxConfigBase::Get();
  if (!config) return;
  bool maximized = false;
  if (config->Read(kGeometryKeyMaximized, &maximized) && maximized) {
    frame_->Maximize(true);
  }
}

class WxWindow::FileDropTargetImpl : public wxFileDropTarget {
 public:
  explicit FileDropTargetImpl(WxWindow* window) : window_(window) {}

  bool OnDropFiles(wxCoord x, wxCoord y,
                   const wxArrayString& filenames) override {
    if (filenames.empty()) {
      return false;
    }
    for (const auto& fn : filenames) {
#if XE_PLATFORM_WIN32
      std::filesystem::path path(fn.wc_str());
#else
      std::filesystem::path path(fn.utf8_string());
#endif
      FileDropEvent drop_event(window_, std::move(path));
      WindowDestructionReceiver destruction_receiver(window_);
      window_->OnFileDrop(drop_event, destruction_receiver);
      if (destruction_receiver.IsWindowDestroyed()) break;
    }
    return true;
  }

 private:
  WxWindow* window_;
};

WxWindow::WxWindow(WindowedAppContext& app_context,
                   const std::string_view title, uint32_t desired_logical_width,
                   uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width,
             desired_logical_height) {}

WxWindow::~WxWindow() {
  EnterDestructor();
#if XE_PLATFORM_LINUX
  if (pending_paint_source_) {
    g_source_remove(pending_paint_source_);
    pending_paint_source_ = 0;
  }
#endif
#if XE_PLATFORM_WIN32
  if (usb_device_notify_) {
    UnregisterDeviceNotification(usb_device_notify_);
    usb_device_notify_ = nullptr;
  }
  if (render_panel_) {
    static_cast<RenderPanel*>(render_panel_)->set_owner(nullptr);
  }
#endif
  // frame_->Destroy() is deferred; queued events between here and actual
  // destruction would otherwise dispatch into a freed WxWindow.
  UnbindFrameAndPanelEvents();
  aui_manager_.UnInit();
  if (frame_) {
    // Drop target captures `this`; clearing it deletes the FileDropTargetImpl.
    frame_->SetDropTarget(nullptr);
    frame_->Destroy();
    frame_ = nullptr;
    render_panel_ = nullptr;
  }
}

bool WxWindow::OpenImpl() {
  // Seed DPI from the primary display; refined to the frame's actual monitor
  // once the frame exists.
  {
    wxSize ppi = wxDisplay().GetPPI();
    dpi_ = ppi.GetWidth() > 0 ? static_cast<uint32_t>(ppi.GetWidth()) : 96;
  }

  const wxPoint initial_pos = ReadInitialFramePositionFromConfig();
  // Seed the size so the OS doesn't show the window at its default size
  // first; EnsureInitialRenderSurfaceSize tightens it up later.
  const wxSize initial_size(
      ConvertSizeDpi(GetDesiredLogicalWidth(), dpi_, GetMediumDpi()),
      ConvertSizeDpi(GetDesiredLogicalHeight(), dpi_, GetMediumDpi()) + 160);
#if XE_PLATFORM_WIN32
  frame_ = new XeniaFrame(nullptr, wxID_ANY, wxString::FromUTF8(GetTitle()),
                          initial_pos, initial_size, wxDEFAULT_FRAME_STYLE);
  ::ShowWindow(static_cast<HWND>(frame_->GetHandle()), SW_HIDE);
  HICON hicon = LoadIconW(GetModuleHandle(nullptr), L"MAINICON");
  if (hicon) {
    wxIcon icon;
    icon.CreateFromHICON(hicon);
    frame_->SetIcon(icon);
  }
#else
  frame_ = new wxFrame(nullptr, wxID_ANY, wxString::FromUTF8(GetTitle()),
                       initial_pos, initial_size, wxDEFAULT_FRAME_STYLE);
  {
    wxMemoryInputStream stream(embedded_app_icon::_256_png_data,
                               embedded_app_icon::_256_png_size);
    wxImage image(stream);
    if (image.IsOk()) {
      wxIcon icon;
      icon.CopyFromBitmap(wxBitmap(image));
      frame_->SetIcon(icon);
    }
  }
#endif

  // Render panel: child of the frame, hosts the swap chain. Mouse/key/paint
  // events bind here. Wraps a custom MSWWindowProc on Windows so WM_PAINT /
  // WM_ERASEBKGND don't fight the swap chain.
#if XE_PLATFORM_WIN32
  auto* render_panel =
      new RenderPanel(frame_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                      wxBORDER_NONE | wxWANTS_CHARS);
  render_panel->set_owner(this);
  render_panel_ = render_panel;
#else
  render_panel_ = new wxWindow(frame_, wxID_ANY, wxDefaultPosition,
                               wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS);
#endif

  aui_manager_.SetManagedWindow(frame_);

  // Refine DPI in case the frame landed on a non-primary monitor.
  {
    wxSize ppi = frame_->GetDPI();
    if (ppi.GetWidth() > 0) {
      dpi_ = static_cast<uint32_t>(ppi.GetWidth());
    }
  }

  uint32_t physical_width =
      ConvertSizeDpi(GetDesiredLogicalWidth(), dpi_, GetMediumDpi());
  uint32_t physical_height =
      ConvertSizeDpi(GetDesiredLogicalHeight(), dpi_, GetMediumDpi());

  aui_manager_.AddPane(
      render_panel_,
      wxAuiPaneInfo().Name("render").CenterPane().PaneBorder(false));
  aui_manager_.Update();

  auto* main_menu = static_cast<WxMenuItem*>(GetMainMenu());
  if (main_menu && main_menu->GetMenuBar()) {
    frame_->SetMenuBar(main_menu->GetMenuBar());
  }

  frame_->SetClientSize(wxSize(physical_width, physical_height));

  startup_cover_ = new wxWindow(render_panel_, wxID_ANY, wxDefaultPosition,
                                render_panel_->GetClientSize(), wxBORDER_NONE);
  startup_cover_->SetBackgroundColour(*wxBLACK);
#if XE_PLATFORM_WIN32
  // Centered app icon, painted directly on the cover so there's no
  // wxStaticBitmap child to leave an occluded hole on destroy.
  if (HICON hicon = static_cast<HICON>(
          ::LoadImageW(GetModuleHandle(nullptr), L"MAINICON", IMAGE_ICON, 256,
                       256, LR_DEFAULTCOLOR))) {
    wxIcon icon;
    icon.CreateFromHICON(hicon);
    wxBitmap bitmap(icon);
    auto* cover = startup_cover_;
    cover->Bind(wxEVT_PAINT, [bitmap, cover](wxPaintEvent&) {
      wxPaintDC dc(cover);
      wxSize win_size = cover->GetClientSize();
      wxSize bmp_size = bitmap.GetSize();
      dc.DrawBitmap(bitmap, (win_size.GetWidth() - bmp_size.GetWidth()) / 2,
                    (win_size.GetHeight() - bmp_size.GetHeight()) / 2, true);
    });
  }
#endif

  frame_->Bind(wxEVT_CLOSE_WINDOW, &WxWindow::OnFrameClose, this);
  // Render area resizes are driven by the panel (AUI sizes it after the
  // frame's WM_SIZE), so bind size tracking there.
  render_panel_->Bind(wxEVT_SIZE, &WxWindow::OnFrameSize, this);
  frame_->Bind(wxEVT_DPI_CHANGED, &WxWindow::OnFrameDpiChanged, this);
  frame_->Bind(wxEVT_SET_FOCUS, &WxWindow::OnFrameSetFocus, this);
  frame_->Bind(wxEVT_KILL_FOCUS, &WxWindow::OnFrameKillFocus, this);
  render_panel_->Bind(wxEVT_SET_FOCUS, &WxWindow::OnPanelSetFocus, this);
  render_panel_->Bind(wxEVT_KILL_FOCUS, &WxWindow::OnPanelKillFocus, this);
  frame_->Bind(wxEVT_MENU_OPEN, &WxWindow::OnMenuOpen, this);
  frame_->Bind(wxEVT_MENU_CLOSE, &WxWindow::OnMenuClose, this);

  render_panel_->Bind(wxEVT_KEY_DOWN, &WxWindow::OnPanelKeyDown, this);
  render_panel_->Bind(wxEVT_KEY_UP, &WxWindow::OnPanelKeyUp, this);
  render_panel_->Bind(wxEVT_CHAR, &WxWindow::OnPanelChar, this);
  render_panel_->Bind(wxEVT_LEFT_DCLICK, &WxWindow::OnPanelDoubleClick, this);
  render_panel_->Bind(wxEVT_LEFT_DOWN, &WxWindow::OnPanelMouseDown, this);
  render_panel_->Bind(wxEVT_LEFT_UP, &WxWindow::OnPanelMouseUp, this);
  render_panel_->Bind(wxEVT_RIGHT_DOWN, &WxWindow::OnPanelMouseDown, this);
  render_panel_->Bind(wxEVT_RIGHT_UP, &WxWindow::OnPanelMouseUp, this);
  render_panel_->Bind(wxEVT_MIDDLE_DOWN, &WxWindow::OnPanelMouseDown, this);
  render_panel_->Bind(wxEVT_MIDDLE_UP, &WxWindow::OnPanelMouseUp, this);
  render_panel_->Bind(wxEVT_AUX1_DOWN, &WxWindow::OnPanelMouseDown, this);
  render_panel_->Bind(wxEVT_AUX1_UP, &WxWindow::OnPanelMouseUp, this);
  render_panel_->Bind(wxEVT_AUX2_DOWN, &WxWindow::OnPanelMouseDown, this);
  render_panel_->Bind(wxEVT_AUX2_UP, &WxWindow::OnPanelMouseUp, this);
  render_panel_->Bind(wxEVT_MOTION, &WxWindow::OnPanelMouseMove, this);
  render_panel_->Bind(wxEVT_MOUSEWHEEL, &WxWindow::OnPanelMouseWheel, this);
#if !XE_PLATFORM_WIN32
  render_panel_->Bind(wxEVT_PAINT, &WxWindow::OnPanelPaint, this);
  render_panel_->Bind(wxEVT_ERASE_BACKGROUND, &WxWindow::OnPanelEraseBackground,
                      this);
#endif

  cursor_auto_hide_timer_.SetOwner(frame_);
  frame_->Bind(wxEVT_TIMER, &WxWindow::OnCursorAutoHideTimer, this,
               cursor_auto_hide_timer_.GetId());
  resize_debounce_timer_.SetOwner(frame_);
  frame_->Bind(wxEVT_TIMER, &WxWindow::OnResizeDebounceTimer, this,
               resize_debounce_timer_.GetId());

  frame_->SetDropTarget(new FileDropTargetImpl(this));

#if XE_PLATFORM_WIN32
  {
    DEV_BROADCAST_DEVICEINTERFACE filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    usb_device_notify_ = RegisterDeviceNotificationW(
        static_cast<HWND>(frame_->GetHandle()), &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
  }
#endif

  RestoreGeometryFromConfig();

  if (IsFullscreen()) {
    frame_->ShowFullScreen(true);
  }

#if XE_PLATFORM_MAC
  // Without this wx uses the legacy fullscreen path, which leaves the
  // title-bar bit in the styleMask and produces a short content view.
  frame_->EnableFullScreenView(true);
#endif

  frame_->Show(true);
  render_target()->SetFocus();

  wxSize client_size = render_target()->GetClientSize();
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(client_size.GetWidth(), client_size.GetHeight(),
                     destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  OnFocusUpdate(render_target()->HasFocus(), destruction_receiver);

  return true;
}

void WxWindow::RequestCloseImpl() {
  if (frame_) {
    frame_->Close();
  }
}

uint32_t WxWindow::GetLatestDpiImpl() const { return dpi_; }

void WxWindow::ApplyNewFullscreen() {
  if (!frame_) return;
  bool fullscreen = IsFullscreen();
  if (fullscreen && !frame_->IsFullScreen()) {
    SaveGeometryToConfig();
  }
  frame_->ShowFullScreen(fullscreen);
  if (content_visibility_callback_) {
    content_visibility_callback_();
  }
}

void WxWindow::SetContentVisibilityCallback(std::function<void()> cb) {
  content_visibility_callback_ = std::move(cb);
}

void WxWindow::EnsureInitialRenderSurfaceSize() {
  if (!frame_ || !render_panel_) return;
  if (frame_->IsMaximized() || frame_->IsFullScreen()) return;
  // Match render surface 1:1 with the cvar resolution (no DPI scaling).
  uint32_t target_w = GetDesiredLogicalWidth();
  uint32_t target_h = GetDesiredLogicalHeight();
  wxSize render_size = render_panel_->GetClientSize();
  wxSize client_size = frame_->GetClientSize();
  int new_client_w = int(target_w) + (client_size.x - render_size.x);
  int new_client_h = int(target_h) + (client_size.y - render_size.y);
  if (new_client_w != client_size.x || new_client_h != client_size.y) {
    frame_->SetClientSize(new_client_w, new_client_h);
  }
}

void WxWindow::ApplyNewTitle() {
  if (!frame_) return;
  frame_->SetTitle(wxString::FromUTF8(GetTitle()));
}

void WxWindow::LoadAndApplyIcon(const void* buffer, size_t size,
                                bool can_apply_state_in_current_phase) {
  if (!frame_ || !can_apply_state_in_current_phase) return;
  if (!buffer || !size) {
#if XE_PLATFORM_WIN32
    HICON hicon = LoadIconW(GetModuleHandle(nullptr), L"MAINICON");
    if (hicon) {
      wxIcon icon;
      icon.CreateFromHICON(hicon);
      frame_->SetIcon(icon);
    }
    return;
#else
    buffer = embedded_app_icon::_256_png_data;
    size = embedded_app_icon::_256_png_size;
#endif
  }
  wxMemoryInputStream stream(buffer, size);
  wxImage image(stream);
  if (image.IsOk()) {
    wxBitmap bitmap(image);
    wxIcon icon;
    icon.CopyFromBitmap(bitmap);
    frame_->SetIcon(icon);
  }
}

void WxWindow::ApplyNewMainMenu(MenuItem* old_main_menu) {
  if (!frame_) return;
  if (IsFullscreen()) {
    return;
  }
  auto* main_menu = static_cast<WxMenuItem*>(GetMainMenu());
  if (main_menu && main_menu->GetMenuBar()) {
    frame_->SetMenuBar(main_menu->GetMenuBar());
  } else {
    frame_->SetMenuBar(nullptr);
  }
}

void WxWindow::CompleteMainMenuItemsUpdateImpl() {
  // wxWidgets refreshes the menu bar automatically; nothing to do.
}

void WxWindow::ApplyNewMouseCapture() {
  if (!render_target()->HasCapture()) {
    render_target()->CaptureMouse();
  }
}

void WxWindow::ApplyNewMouseRelease() {
  if (render_target()->HasCapture()) {
    render_target()->ReleaseMouse();
  }
}

void WxWindow::ApplyNewCursorVisibility(CursorVisibility old_visibility) {
  if (!frame_) return;

  if (GetCursorVisibility() == CursorVisibility::kVisible) {
    cursor_auto_hide_timer_.Stop();
    if (cursor_currently_auto_hidden_) {
      cursor_currently_auto_hidden_ = false;
      render_target()->SetCursor(wxCursor(wxCURSOR_ARROW));
    }
  } else {
    cursor_currently_auto_hidden_ = true;
    render_target()->SetCursor(wxCursor(wxCURSOR_BLANK));
    cursor_auto_hide_timer_.StartOnce(2000);
  }
}

void WxWindow::FocusImpl() { render_target()->SetFocus(); }

std::unique_ptr<Surface> WxWindow::CreateSurfaceImpl(
    Surface::TypeFlags allowed_types) {
  if (!render_panel_) return nullptr;

#if XE_PLATFORM_WIN32
  if (allowed_types & Surface::kTypeFlag_Win32Hwnd) {
    HWND hwnd = static_cast<HWND>(render_panel_->GetHandle());
    HINSTANCE hinstance = GetModuleHandle(nullptr);
    return std::make_unique<Win32HwndSurface>(hinstance, hwnd);
  }
#elif XE_PLATFORM_LINUX
  if (auto surface = gtk_surface_factory_.Create(render_panel_->GetHandle(),
                                                 allowed_types)) {
    DropStartupCoverIfPresent();
    return surface;
  }
#elif XE_PLATFORM_MAC
  if (allowed_types & Surface::kTypeFlag_MacNSView) {
    // wx's WXWidget and our forward-declared NSView are unrelated opaque
    // structs at the C++ level; both alias the same underlying Obj-C pointer.
    NSView* view = reinterpret_cast<NSView*>(render_panel_->GetHandle());
    if (!view) return nullptr;
    DropStartupCoverIfPresent();
    return std::make_unique<MacNSViewSurface>(view);
  }
#endif
  return nullptr;
}

void WxWindow::RequestPaintImpl() {
#if XE_PLATFORM_WIN32
  if (render_panel_) {
    InvalidateRect(static_cast<HWND>(render_panel_->GetHandle()), nullptr,
                   FALSE);
  }
#elif XE_PLATFORM_LINUX
  // Schedule via glib's main loop (not wx's pending-event queue) so X11 input
  // events at higher priority preempt back-to-back paints.
  if (pending_paint_source_) {
    return;
  }
  pending_paint_source_ = g_idle_add_full(
      G_PRIORITY_LOW,
      [](gpointer user_data) -> gboolean {
        auto* self = static_cast<WxWindow*>(user_data);
        self->pending_paint_source_ = 0;
        if (self->render_panel_) {
          self->OnPaint(true);
        }
        return G_SOURCE_REMOVE;
      },
      this, nullptr);
#elif XE_PLATFORM_MAC
  wxTheApp->CallAfter([this]() {
    if (render_panel_) {
      render_panel_->Refresh(false);
      render_panel_->Update();
    }
  });
#endif
}

void WxWindow::OnFrameClose(wxCloseEvent& event) {
  if (frame_ && !frame_->IsFullScreen()) {
    SaveGeometryToConfig();
  }

  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) return;
  OnAfterClose();
}

void WxWindow::OnFrameSize(wxSizeEvent& event) {
  event.Skip();
  wxSize size = render_target()->GetClientSize();
  if (startup_cover_) {
    startup_cover_->SetSize(size);
  }
#if XE_PLATFORM_LINUX
  gtk_surface_factory_.OnResize(render_panel_->GetHandle());
#endif
  // Coalesce presenter resizes — a drag at compositor frame rate fires
  // wxEVT_SIZE per pixel, and one swapchain rebuild per event has tripped
  // VK_ERROR_DEVICE_LOST under stress.
  pending_resize_size_ = size;
  resize_debounce_timer_.StartOnce(50);
}

void WxWindow::OnResizeDebounceTimer(wxTimerEvent& event) {
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(pending_resize_size_.GetWidth(),
                     pending_resize_size_.GetHeight(), destruction_receiver);
}

void WxWindow::OnFrameDpiChanged(wxDPIChangedEvent& event) {
  wxSize new_dpi = event.GetNewDPI();
  if (new_dpi.GetWidth() > 0) {
    dpi_ = static_cast<uint32_t>(new_dpi.GetWidth());
  }
  WindowDestructionReceiver destruction_receiver(this);
  UISetupEvent e(this);
  OnDpiChanged(e, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) return;
  event.Skip();
}

void WxWindow::OnFrameSetFocus(wxFocusEvent& event) {
  event.Skip();
  // The render panel is what should hold focus; bounce the frame's focus to
  // it. Focus tracking happens on the panel (OnPanelSetFocus / KillFocus).
  if (!render_target()->HasFocus()) {
    render_target()->SetFocus();
  }
}

void WxWindow::OnFrameKillFocus(wxFocusEvent& event) { event.Skip(); }

void WxWindow::OnPanelSetFocus(wxFocusEvent& event) {
  event.Skip();
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(true, destruction_receiver);
}

void WxWindow::OnPanelKillFocus(wxFocusEvent& event) {
  event.Skip();
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(false, destruction_receiver);
}

void WxWindow::OnPanelKeyDown(wxKeyEvent& event) {
  VirtualKey vk = TranslateKeyCode(event.GetKeyCode());
  if (vk == VirtualKey::kNone) {
    event.Skip();
    return;
  }
  KeyEvent key_event(this, vk, 1, false, event.ShiftDown(), event.ControlDown(),
                     event.AltDown(), false);
  WindowDestructionReceiver destruction_receiver(this);
  OnKeyDown(key_event, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
#if XE_PLATFORM_MAC
  // Don't Skip(): wxOSX routes via -interpretKeyEvents:/-insertText: which
  // NSBeeps for every keystroke not consumed by an NSText control. Synthesize
  // the ImGui char event ourselves; loses IME composition we don't use.
  int uc = event.GetUnicodeKey();
  if (uc > 0 && uc < 0x10000) {
    // GetUnicodeKey() on KEY_DOWN returns the raw (uppercase) key, not the
    // shifted character — apply shift for ASCII letters so ImGui text fields
    // see lowercase by default.
    if (uc >= 'A' && uc <= 'Z' && !event.ShiftDown()) {
      uc += 'a' - 'A';
    }
    KeyEvent char_event(this, static_cast<VirtualKey>(uc), 1, false,
                        event.ShiftDown(), event.ControlDown(), event.AltDown(),
                        false);
    OnKeyChar(char_event, destruction_receiver);
  }
#else
  // Skip() so wxEVT_CHAR fires for ImGui text input.
  event.Skip();
#endif
}

void WxWindow::OnPanelKeyUp(wxKeyEvent& event) {
  VirtualKey vk = TranslateKeyCode(event.GetKeyCode());
  if (vk == VirtualKey::kNone) {
    event.Skip();
    return;
  }
  KeyEvent key_event(this, vk, 0, true, event.ShiftDown(), event.ControlDown(),
                     event.AltDown(), false);
  WindowDestructionReceiver destruction_receiver(this);
  OnKeyUp(key_event, destruction_receiver);
  event.Skip();
}

void WxWindow::OnPanelChar(wxKeyEvent& event) {
  int uc = event.GetUnicodeKey();
  if (uc == WXK_NONE) {
    event.Skip();
    return;
  }
  // VirtualKey here carries the unicode codepoint, matching the WM_CHAR
  // convention ImGuiDrawer::OnKeyChar consumes.
  KeyEvent key_event(this, static_cast<VirtualKey>(uc), 1, false,
                     event.ShiftDown(), event.ControlDown(), event.AltDown(),
                     false);
  WindowDestructionReceiver destruction_receiver(this);
  OnKeyChar(key_event, destruction_receiver);
  if (!key_event.is_handled()) {
    event.Skip();
  }
}

static MouseEvent::Button WxMouseButtonToXenia(const wxMouseEvent& event) {
  if (event.LeftIsDown() || event.GetEventType() == wxEVT_LEFT_DOWN ||
      event.GetEventType() == wxEVT_LEFT_UP)
    return MouseEvent::Button::kLeft;
  if (event.RightIsDown() || event.GetEventType() == wxEVT_RIGHT_DOWN ||
      event.GetEventType() == wxEVT_RIGHT_UP)
    return MouseEvent::Button::kRight;
  if (event.MiddleIsDown() || event.GetEventType() == wxEVT_MIDDLE_DOWN ||
      event.GetEventType() == wxEVT_MIDDLE_UP)
    return MouseEvent::Button::kMiddle;
  if (event.Aux1IsDown() || event.GetEventType() == wxEVT_AUX1_DOWN ||
      event.GetEventType() == wxEVT_AUX1_UP)
    return MouseEvent::Button::kX1;
  if (event.Aux2IsDown() || event.GetEventType() == wxEVT_AUX2_DOWN ||
      event.GetEventType() == wxEVT_AUX2_UP)
    return MouseEvent::Button::kX2;
  return MouseEvent::Button::kNone;
}

void WxWindow::OnPanelMouseDown(wxMouseEvent& event) {
  MouseEvent mouse_event(this, WxMouseButtonToXenia(event), event.GetX(),
                         event.GetY());
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseDown(mouse_event, destruction_receiver);
}

void WxWindow::OnPanelMouseUp(wxMouseEvent& event) {
  MouseEvent mouse_event(this, WxMouseButtonToXenia(event), event.GetX(),
                         event.GetY());
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseUp(mouse_event, destruction_receiver);
}

void WxWindow::OnPanelMouseMove(wxMouseEvent& event) {
  MouseEvent mouse_event(this, MouseEvent::Button::kNone, event.GetX(),
                         event.GetY());
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseMove(mouse_event, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) return;

  if (cursor_currently_auto_hidden_) {
    cursor_currently_auto_hidden_ = false;
    render_target()->SetCursor(wxCursor(wxCURSOR_ARROW));
  }
  cursor_auto_hide_timer_.StartOnce(2000);
}

void WxWindow::OnPanelMouseWheel(wxMouseEvent& event) {
  int delta = event.GetWheelRotation();
  MouseEvent mouse_event(this, MouseEvent::Button::kNone, event.GetX(),
                         event.GetY(), 0, delta);
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseWheel(mouse_event, destruction_receiver);
}

void WxWindow::OnPanelPaint(wxPaintEvent& event) {
  wxPaintDC dc(render_panel_);
  OnPaint(true);
}

void WxWindow::OnPanelDoubleClick(wxMouseEvent& event) {
  if (cvars::disable_doubleclick_fullscreen) {
    return;
  }
  bool entering_fullscreen = !IsFullscreen();
  SetCursorVisibility(entering_fullscreen ? CursorVisibility::kAutoHidden
                                          : CursorVisibility::kVisible);
  SetFullscreen(entering_fullscreen);
}

void WxWindow::OnPanelEraseBackground(wxEraseEvent& event) {
  // No-op: prevent flicker; the render surface drives all drawing.
}

void WxWindow::OnCursorAutoHideTimer(wxTimerEvent& event) {
  if (!cursor_currently_auto_hidden_) {
    cursor_currently_auto_hidden_ = true;
    render_target()->SetCursor(wxCursor(wxCURSOR_BLANK));
  }
}

void WxWindow::OnMenuOpen(wxMenuEvent& event) {
  menu_open_ = true;
  cursor_auto_hide_timer_.Stop();
  if (cursor_currently_auto_hidden_) {
    cursor_currently_auto_hidden_ = false;
    render_target()->SetCursor(wxCursor(wxCURSOR_ARROW));
  }
}

void WxWindow::OnMenuClose(wxMenuEvent& event) { menu_open_ = false; }

void WxWindow::UnbindFrameAndPanelEvents() {
  if (frame_) {
    frame_->Unbind(wxEVT_CLOSE_WINDOW, &WxWindow::OnFrameClose, this);
    frame_->Unbind(wxEVT_DPI_CHANGED, &WxWindow::OnFrameDpiChanged, this);
    frame_->Unbind(wxEVT_SET_FOCUS, &WxWindow::OnFrameSetFocus, this);
    frame_->Unbind(wxEVT_KILL_FOCUS, &WxWindow::OnFrameKillFocus, this);
    frame_->Unbind(wxEVT_MENU_OPEN, &WxWindow::OnMenuOpen, this);
    frame_->Unbind(wxEVT_MENU_CLOSE, &WxWindow::OnMenuClose, this);
    frame_->Unbind(wxEVT_TIMER, &WxWindow::OnCursorAutoHideTimer, this,
                   cursor_auto_hide_timer_.GetId());
    frame_->Unbind(wxEVT_TIMER, &WxWindow::OnResizeDebounceTimer, this,
                   resize_debounce_timer_.GetId());
  }
  if (render_panel_) {
    render_panel_->Unbind(wxEVT_SIZE, &WxWindow::OnFrameSize, this);
    render_panel_->Unbind(wxEVT_SET_FOCUS, &WxWindow::OnPanelSetFocus, this);
    render_panel_->Unbind(wxEVT_KILL_FOCUS, &WxWindow::OnPanelKillFocus, this);
    render_panel_->Unbind(wxEVT_KEY_DOWN, &WxWindow::OnPanelKeyDown, this);
    render_panel_->Unbind(wxEVT_KEY_UP, &WxWindow::OnPanelKeyUp, this);
    render_panel_->Unbind(wxEVT_CHAR, &WxWindow::OnPanelChar, this);
    render_panel_->Unbind(wxEVT_LEFT_DCLICK, &WxWindow::OnPanelDoubleClick,
                          this);
    render_panel_->Unbind(wxEVT_LEFT_DOWN, &WxWindow::OnPanelMouseDown, this);
    render_panel_->Unbind(wxEVT_LEFT_UP, &WxWindow::OnPanelMouseUp, this);
    render_panel_->Unbind(wxEVT_RIGHT_DOWN, &WxWindow::OnPanelMouseDown, this);
    render_panel_->Unbind(wxEVT_RIGHT_UP, &WxWindow::OnPanelMouseUp, this);
    render_panel_->Unbind(wxEVT_MIDDLE_DOWN, &WxWindow::OnPanelMouseDown, this);
    render_panel_->Unbind(wxEVT_MIDDLE_UP, &WxWindow::OnPanelMouseUp, this);
    render_panel_->Unbind(wxEVT_AUX1_DOWN, &WxWindow::OnPanelMouseDown, this);
    render_panel_->Unbind(wxEVT_AUX1_UP, &WxWindow::OnPanelMouseUp, this);
    render_panel_->Unbind(wxEVT_AUX2_DOWN, &WxWindow::OnPanelMouseDown, this);
    render_panel_->Unbind(wxEVT_AUX2_UP, &WxWindow::OnPanelMouseUp, this);
    render_panel_->Unbind(wxEVT_MOTION, &WxWindow::OnPanelMouseMove, this);
    render_panel_->Unbind(wxEVT_MOUSEWHEEL, &WxWindow::OnPanelMouseWheel, this);
#if !XE_PLATFORM_WIN32
    render_panel_->Unbind(wxEVT_PAINT, &WxWindow::OnPanelPaint, this);
    render_panel_->Unbind(wxEVT_ERASE_BACKGROUND,
                          &WxWindow::OnPanelEraseBackground, this);
#endif
  }
}

VirtualKey WxWindow::TranslateKeyCode(int wx_key) {
  if (wx_key >= 'A' && wx_key <= 'Z') return static_cast<VirtualKey>(wx_key);
  if (wx_key >= '0' && wx_key <= '9') return static_cast<VirtualKey>(wx_key);

  switch (wx_key) {
    case WXK_BACK:
      return VirtualKey::kBack;
    case WXK_TAB:
      return VirtualKey::kTab;
    case WXK_RETURN:
      return VirtualKey::kReturn;
    case WXK_SHIFT:
      return VirtualKey::kShift;
    case WXK_CONTROL:
      return VirtualKey::kControl;
    case WXK_ALT:
      return VirtualKey::kMenu;
    case WXK_PAUSE:
      return VirtualKey::kPause;
    case WXK_CAPITAL:
      return VirtualKey::kCapital;
    case WXK_ESCAPE:
      return VirtualKey::kEscape;
    case WXK_SPACE:
      return VirtualKey::kSpace;
    case WXK_PAGEUP:
      return VirtualKey::kPrior;
    case WXK_PAGEDOWN:
      return VirtualKey::kNext;
    case WXK_END:
      return VirtualKey::kEnd;
    case WXK_HOME:
      return VirtualKey::kHome;
    case WXK_LEFT:
      return VirtualKey::kLeft;
    case WXK_UP:
      return VirtualKey::kUp;
    case WXK_RIGHT:
      return VirtualKey::kRight;
    case WXK_DOWN:
      return VirtualKey::kDown;
    case WXK_PRINT:
      return VirtualKey::kPrint;
    case WXK_INSERT:
      return VirtualKey::kInsert;
    case WXK_DELETE:
      return VirtualKey::kDelete;
    case WXK_NUMPAD0:
      return VirtualKey::kNumpad0;
    case WXK_NUMPAD1:
      return VirtualKey::kNumpad1;
    case WXK_NUMPAD2:
      return VirtualKey::kNumpad2;
    case WXK_NUMPAD3:
      return VirtualKey::kNumpad3;
    case WXK_NUMPAD4:
      return VirtualKey::kNumpad4;
    case WXK_NUMPAD5:
      return VirtualKey::kNumpad5;
    case WXK_NUMPAD6:
      return VirtualKey::kNumpad6;
    case WXK_NUMPAD7:
      return VirtualKey::kNumpad7;
    case WXK_NUMPAD8:
      return VirtualKey::kNumpad8;
    case WXK_NUMPAD9:
      return VirtualKey::kNumpad9;
    case WXK_NUMPAD_MULTIPLY:
      return VirtualKey::kMultiply;
    case WXK_NUMPAD_ADD:
      return VirtualKey::kAdd;
    case WXK_NUMPAD_SUBTRACT:
      return VirtualKey::kSubtract;
    case WXK_NUMPAD_DECIMAL:
      return VirtualKey::kDecimal;
    case WXK_NUMPAD_DIVIDE:
      return VirtualKey::kDivide;
    case WXK_F1:
      return VirtualKey::kF1;
    case WXK_F2:
      return VirtualKey::kF2;
    case WXK_F3:
      return VirtualKey::kF3;
    case WXK_F4:
      return VirtualKey::kF4;
    case WXK_F5:
      return VirtualKey::kF5;
    case WXK_F6:
      return VirtualKey::kF6;
    case WXK_F7:
      return VirtualKey::kF7;
    case WXK_F8:
      return VirtualKey::kF8;
    case WXK_F9:
      return VirtualKey::kF9;
    case WXK_F10:
      return VirtualKey::kF10;
    case WXK_F11:
      return VirtualKey::kF11;
    case WXK_F12:
      return VirtualKey::kF12;
    case WXK_NUMLOCK:
      return VirtualKey::kNumLock;
    case WXK_SCROLL:
      return VirtualKey::kScroll;
    case WXK_NUMPAD_ENTER:
      return VirtualKey::kReturn;
    case WXK_WINDOWS_LEFT:
      return VirtualKey::kLWin;
    case WXK_WINDOWS_RIGHT:
      return VirtualKey::kRWin;
    // Punctuation keys map to Win32 VK_OEM_* codes since the keyboard
    // bindings (keyboard_binding_table.inc) are expressed in those.
    case ';':
    case ':':
      return VirtualKey::kOem1;
    case '=':
    case '+':
      return VirtualKey::kOemPlus;
    case ',':
    case '<':
      return VirtualKey::kOemComma;
    case '-':
    case '_':
      return VirtualKey::kOemMinus;
    case '.':
    case '>':
      return VirtualKey::kOemPeriod;
    case '/':
    case '?':
      return VirtualKey::kOem2;
    case '`':
    case '~':
      return VirtualKey::kOem3;
    case '[':
    case '{':
      return VirtualKey::kOem4;
    case '\\':
    case '|':
      return VirtualKey::kOem5;
    case ']':
    case '}':
      return VirtualKey::kOem6;
    case '\'':
    case '"':
      return VirtualKey::kOem7;
    default:
      return VirtualKey::kNone;
  }
}

}  // namespace ui
}  // namespace xe
