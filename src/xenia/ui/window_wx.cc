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

#include "xenia/base/logging.h"
#include "xenia/ui/menu_item_wx.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/windowed_app_context_wx.h"

#if XE_PLATFORM_WIN32
#include "xenia/ui/surface_win.h"
#elif XE_PLATFORM_GNU_LINUX
#include <X11/Xlib-xcb.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include "xenia/ui/surface_gnulinux.h"
#endif

namespace xe {
namespace ui {

// Window::Create factory -- returns WxWindow on all desktop platforms.
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
    return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
  }

 private:
  WxWindow* owner_ = nullptr;
};
#endif

wxWindow* WxWindow::render_target() const {
  return static_cast<wxWindow*>(frame_);
}

void WxWindow::DropStartupCoverIfPresent() {
  if (startup_cover_) {
    startup_cover_->Destroy();
    startup_cover_ = nullptr;
  }
}

// --- FileDropTargetImpl ---

class WxWindow::FileDropTargetImpl : public wxFileDropTarget {
 public:
  explicit FileDropTargetImpl(WxWindow* window) : window_(window) {}

  bool OnDropFiles(wxCoord x, wxCoord y,
                   const wxArrayString& filenames) override {
    if (filenames.empty()) {
      return false;
    }
    // ToStdString() loses non-ASCII paths on Windows.
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

// --- WxWindow ---

WxWindow::WxWindow(WindowedAppContext& app_context,
                   const std::string_view title, uint32_t desired_logical_width,
                   uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width,
             desired_logical_height) {}

WxWindow::~WxWindow() {
  EnterDestructor();
#if XE_PLATFORM_WIN32
  if (usb_device_notify_) {
    UnregisterDeviceNotification(usb_device_notify_);
    usb_device_notify_ = nullptr;
  }
#endif
  if (frame_) {
#if XE_PLATFORM_WIN32
    static_cast<XeniaFrame*>(frame_)->set_owner(nullptr);
#endif
    frame_->Unbind(wxEVT_CLOSE_WINDOW, &WxWindow::OnFrameClose, this);
    frame_->Destroy();
    frame_ = nullptr;
  }
}

bool WxWindow::OpenImpl() {
  // Seed DPI from the primary display; refined to the frame's actual monitor
  // once the frame exists.
  {
    wxSize ppi = wxDisplay().GetPPI();
    dpi_ = ppi.GetWidth() > 0 ? static_cast<uint32_t>(ppi.GetWidth()) : 96;
  }

#if XE_PLATFORM_WIN32
  auto* xenia_frame =
      new XeniaFrame(nullptr, wxID_ANY, wxString::FromUTF8(GetTitle()),
                     wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE);
  xenia_frame->set_owner(this);
  frame_ = xenia_frame;
  HICON hicon = LoadIconW(GetModuleHandle(nullptr), L"MAINICON");
  if (hicon) {
    wxIcon icon;
    icon.CreateFromHICON(hicon);
    frame_->SetIcon(icon);
  }
  {
    HWND hwnd = static_cast<HWND>(frame_->GetHandle());
    DWORD class_style = GetClassLong(hwnd, GCL_STYLE);
    SetClassLong(hwnd, GCL_STYLE, class_style | CS_OWNDC);
  }
#else
  frame_ = new wxFrame(nullptr, wxID_ANY, wxString::FromUTF8(GetTitle()),
                       wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE);
#endif

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

  // Attach the main menu if one has been set.
  auto* main_menu = static_cast<WxMenuItem*>(GetMainMenu());
  if (main_menu && main_menu->GetMenuBar()) {
    frame_->SetMenuBar(main_menu->GetMenuBar());
  }

  // After SetMenuBar so the menu bar height is included. The constructor's
  // wxSize would be outer-window, leaving the client area too short.
  frame_->SetClientSize(wxSize(physical_width, physical_height));

  // Black child covering the client area until the swap chain attaches.
  startup_cover_ = new wxWindow(frame_, wxID_ANY, wxDefaultPosition,
                                frame_->GetClientSize(), wxBORDER_NONE);
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

  // Bind frame events.
  frame_->Bind(wxEVT_CLOSE_WINDOW, &WxWindow::OnFrameClose, this);
  frame_->Bind(wxEVT_SIZE, &WxWindow::OnFrameSize, this);
  frame_->Bind(wxEVT_DPI_CHANGED, &WxWindow::OnFrameDpiChanged, this);
  frame_->Bind(wxEVT_SET_FOCUS, &WxWindow::OnFrameSetFocus, this);
  frame_->Bind(wxEVT_KILL_FOCUS, &WxWindow::OnFrameKillFocus, this);
  frame_->Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent&) {
    menu_open_ = true;
    cursor_auto_hide_timer_.Stop();
    if (cursor_currently_auto_hidden_) {
      cursor_currently_auto_hidden_ = false;
      render_target()->SetCursor(wxCursor(wxCURSOR_ARROW));
    }
  });
  frame_->Bind(wxEVT_MENU_CLOSE, [this](wxMenuEvent&) { menu_open_ = false; });

  frame_->Bind(wxEVT_KEY_DOWN, &WxWindow::OnPanelKeyDown, this);
  frame_->Bind(wxEVT_KEY_UP, &WxWindow::OnPanelKeyUp, this);
  frame_->Bind(wxEVT_CHAR, &WxWindow::OnPanelChar, this);
  frame_->Bind(wxEVT_LEFT_DCLICK, &WxWindow::OnPanelDoubleClick, this);
  frame_->Bind(wxEVT_LEFT_DOWN, &WxWindow::OnPanelMouseDown, this);
  frame_->Bind(wxEVT_LEFT_UP, &WxWindow::OnPanelMouseUp, this);
  frame_->Bind(wxEVT_RIGHT_DOWN, &WxWindow::OnPanelMouseDown, this);
  frame_->Bind(wxEVT_RIGHT_UP, &WxWindow::OnPanelMouseUp, this);
  frame_->Bind(wxEVT_MIDDLE_DOWN, &WxWindow::OnPanelMouseDown, this);
  frame_->Bind(wxEVT_MIDDLE_UP, &WxWindow::OnPanelMouseUp, this);
  frame_->Bind(wxEVT_AUX1_DOWN, &WxWindow::OnPanelMouseDown, this);
  frame_->Bind(wxEVT_AUX1_UP, &WxWindow::OnPanelMouseUp, this);
  frame_->Bind(wxEVT_AUX2_DOWN, &WxWindow::OnPanelMouseDown, this);
  frame_->Bind(wxEVT_AUX2_UP, &WxWindow::OnPanelMouseUp, this);
  frame_->Bind(wxEVT_MOTION, &WxWindow::OnPanelMouseMove, this);
  frame_->Bind(wxEVT_MOUSEWHEEL, &WxWindow::OnPanelMouseWheel, this);
#if !XE_PLATFORM_WIN32
  frame_->Bind(wxEVT_PAINT, &WxWindow::OnPanelPaint, this);
  frame_->Bind(wxEVT_ERASE_BACKGROUND, &WxWindow::OnPanelEraseBackground, this);
#endif

  cursor_auto_hide_timer_.SetOwner(frame_);
  frame_->Bind(wxEVT_TIMER, &WxWindow::OnCursorAutoHideTimer, this,
               cursor_auto_hide_timer_.GetId());

  // File drag-drop.
  frame_->SetDropTarget(new FileDropTargetImpl(this));

#if XE_PLATFORM_WIN32
  // Register for USB device notifications.
  {
    DEV_BROADCAST_DEVICEINTERFACE filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    usb_device_notify_ = RegisterDeviceNotificationW(
        static_cast<HWND>(frame_->GetHandle()), &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
  }
#endif

  // Restore last position/size/maximized from wxConfig before Show().
  wxPersistenceManager::Get().RegisterAndRestore(frame_);

  // Fullscreen overrides any restored maximized state.
  if (IsFullscreen()) {
    frame_->ShowFullScreen(true);
  }

  frame_->Show(true);
  render_target()->SetFocus();

  // Report initial state.
  wxSize client_size = render_target()->GetClientSize();
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(client_size.GetWidth(), client_size.GetHeight(),
                     destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  OnFocusUpdate(frame_->HasFocus() || render_target()->HasFocus(),
                destruction_receiver);

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
  // Snapshot the pre-fullscreen rect so closing from fullscreen restores it.
  if (IsFullscreen() && !frame_->IsFullScreen()) {
    wxPersistenceManager::Get().Save(frame_);
    if (auto* config = wxConfigBase::Get(false)) {
      config->Flush();
    }
  }
  frame_->ShowFullScreen(IsFullscreen());
}

void WxWindow::ApplyNewTitle() {
  if (!frame_) return;
  frame_->SetTitle(wxString::FromUTF8(GetTitle()));
}

void WxWindow::LoadAndApplyIcon(const void* buffer, size_t size,
                                bool can_apply_state_in_current_phase) {
  if (!frame_ || !can_apply_state_in_current_phase) return;
  if (!buffer || !size) {
    // Reset to default app icon.
#if XE_PLATFORM_WIN32
    HICON hicon = LoadIconW(GetModuleHandle(nullptr), L"MAINICON");
    if (hicon) {
      wxIcon icon;
      icon.CreateFromHICON(hicon);
      frame_->SetIcon(icon);
    }
#else
    frame_->SetIcon(wxNullIcon);
#endif
    return;
  }
  // Load the icon from the .ico buffer via wxWidgets (cross-platform).
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
    // Menu will be restored when leaving fullscreen.
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
  // wxWidgets updates the menu bar automatically when items are modified.
  // No manual refresh like DrawMenuBar() is needed.
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
    // kAutoHidden: hide immediately, start timer so mouse move can reveal.
    cursor_currently_auto_hidden_ = true;
    render_target()->SetCursor(wxCursor(wxCURSOR_BLANK));
    cursor_auto_hide_timer_.StartOnce(2000);
  }
}

void WxWindow::FocusImpl() { render_target()->SetFocus(); }

std::unique_ptr<Surface> WxWindow::CreateSurfaceImpl(
    Surface::TypeFlags allowed_types) {
  if (!frame_) return nullptr;

#if XE_PLATFORM_WIN32
  if (allowed_types & Surface::kTypeFlag_Win32Hwnd) {
    HWND hwnd = static_cast<HWND>(frame_->GetHandle());
    HINSTANCE hinstance = GetModuleHandle(nullptr);
    return std::make_unique<Win32HwndSurface>(hinstance, hwnd);
  }
#elif XE_PLATFORM_GNU_LINUX
  if (allowed_types & Surface::kTypeFlag_XcbWindow) {
    GtkWidget* gtk_widget = static_cast<GtkWidget*>(frame_->GetHandle());
    if (!gtk_widget) return nullptr;
    gtk_widget_realize(gtk_widget);
    GdkWindow* gdk_window = gtk_widget_get_window(gtk_widget);
    if (!gdk_window) return nullptr;
    GdkDisplay* display = gdk_window_get_display(gdk_window);
    if (!GDK_IS_X11_DISPLAY(display)) return nullptr;
    xcb_connection_t* connection =
        XGetXCBConnection(gdk_x11_display_get_xdisplay(display));
    xcb_window_t window = gdk_x11_window_get_xid(gdk_window);
    return std::make_unique<XcbWindowSurface>(connection, window);
  }
#endif
  return nullptr;
}

void WxWindow::RequestPaintImpl() {
#if XE_PLATFORM_WIN32
  InvalidateRect(static_cast<HWND>(frame_->GetHandle()), nullptr, FALSE);
#else
  wxTheApp->CallAfter([this]() {
    if (frame_) {
      frame_->Refresh(false);
      frame_->Update();
    }
  });
#endif
}

// --- Frame event handlers ---

void WxWindow::OnFrameClose(wxCloseEvent& event) {
  // Persist while the frame is still valid; skip when fullscreen so the
  // ApplyNewFullscreen snapshot stays as the last windowed rect. Flush
  // immediately because xenia exits via std::quick_exit (xenia_main.cc),
  // which skips wxFileConfig's destructor.
  if (frame_ && !frame_->IsFullScreen()) {
    wxPersistenceManager::Get().SaveAndUnregister(frame_);
    if (auto* config = wxConfigBase::Get(false)) {
      config->Flush();
    }
  } else if (frame_) {
    // Don't let the destruction tracker resave the fullscreen rect on top of
    // the snapshot ApplyNewFullscreen wrote.
    wxPersistenceManager::Get().Unregister(frame_);
  }

  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) return;
  OnAfterClose();
  // Note: wxFrame will be destroyed as part of ~WxWindow or if the Window
  // is reopened later.
}

void WxWindow::OnFrameSize(wxSizeEvent& event) {
  event.Skip();
  wxSize size = render_target()->GetClientSize();
  if (startup_cover_) {
    startup_cover_->SetSize(size);
  }
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(size.GetWidth(), size.GetHeight(), destruction_receiver);
}

void WxWindow::OnFrameDpiChanged(wxDPIChangedEvent& event) {
  // Window dragged across a per-monitor DPI boundary. wx resizes the frame
  // itself; we just update the cached DPI and notify listeners.
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
  // Redirect focus to the render panel so it receives key events.
  if (!render_target()->HasFocus()) {
    render_target()->SetFocus();
  }
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(true, destruction_receiver);
}

void WxWindow::OnFrameKillFocus(wxFocusEvent& event) {
  event.Skip();
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(false, destruction_receiver);
}

// --- Render panel event handlers ---

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
  // Always Skip() so wxEVT_CHAR is generated for character input (ImGui text
  // fields depend on OnKeyChar which only fires if wxEVT_CHAR is produced).
  event.Skip();
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
  // For character events, pass the unicode character as the VirtualKey.
  // This matches the Win32 WM_CHAR convention -- ImGuiDrawer::OnKeyChar
  // reads e.virtual_key() and passes it to ImGui::AddInputCharacter().
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
  wxPaintDC dc(frame_);
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
  // Do nothing -- prevent flickering. The render surface handles all drawing.
}

void WxWindow::OnCursorAutoHideTimer(wxTimerEvent& event) {
  if (!cursor_currently_auto_hidden_) {
    cursor_currently_auto_hidden_ = true;
    render_target()->SetCursor(wxCursor(wxCURSOR_BLANK));
  }
}

// --- Virtual key translation ---

VirtualKey WxWindow::TranslateKeyCode(int wx_key) {
  // Map wxWidgets key codes to Xenia VirtualKey (Windows VK_* values).
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
    default:
      return VirtualKey::kNone;
  }
}

}  // namespace ui
}  // namespace xe
