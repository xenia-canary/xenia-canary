/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/window_mac.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/ui/surface_mac.h"
#include "xenia/ui/virtual_key.h"

// Device-dependent modifier flags distinguishing the left and right keys, from
// IOKit/hidsystem/IOLLEvent.h.
static constexpr NSUInteger kDeviceLeftControlKeyMask = 0x00000001;
static constexpr NSUInteger kDeviceLeftShiftKeyMask = 0x00000002;
static constexpr NSUInteger kDeviceRightShiftKeyMask = 0x00000004;
static constexpr NSUInteger kDeviceLeftCommandKeyMask = 0x00000008;
static constexpr NSUInteger kDeviceRightCommandKeyMask = 0x00000010;
static constexpr NSUInteger kDeviceLeftAlternateKeyMask = 0x00000020;
static constexpr NSUInteger kDeviceRightAlternateKeyMask = 0x00000040;
static constexpr NSUInteger kDeviceRightControlKeyMask = 0x00002000;

@interface XeniaMetalView : NSView {
 @public
  xe::ui::MacWindow* window_;
}
@end

@implementation XeniaMetalView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    window_ = nullptr;
    self.wantsLayer = YES;
    self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
  }
  return self;
}

- (CALayer*)makeBackingLayer {
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.contentsScale = self.window ? self.window.backingScaleFactor
                                    : [NSScreen mainScreen].backingScaleFactor;
  return layer;
}

- (BOOL)wantsUpdateLayer {
  return YES;
}

- (void)updateLayer {
  if (window_) {
    window_->HandlePaint();
  }
}

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (self.window) {
    self.layer.contentsScale = self.window.backingScaleFactor;
  }
  if (window_) {
    window_->HandleDpiChange();
  }
}

- (void)setFrameSize:(NSSize)new_size {
  [super setFrameSize:new_size];
  if (window_) {
    window_->HandleSizeUpdate();
  }
}

// Keyboard - not calling super to avoid the unhandled key beep.
- (void)keyDown:(NSEvent*)event {
  if (window_) {
    window_->HandleKeyEvent((__bridge void*)event);
  }
}
- (void)keyUp:(NSEvent*)event {
  if (window_) {
    window_->HandleKeyEvent((__bridge void*)event);
  }
}
- (void)flagsChanged:(NSEvent*)event {
  if (window_) {
    window_->HandleKeyEvent((__bridge void*)event);
  }
}

// Mouse.
- (void)mouseDown:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)mouseUp:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)rightMouseDown:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)rightMouseUp:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)otherMouseDown:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)otherMouseUp:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)mouseMoved:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)mouseDragged:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)rightMouseDragged:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)otherMouseDragged:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}
- (void)scrollWheel:(NSEvent*)event {
  if (window_) {
    window_->HandleMouseEvent((__bridge void*)event);
  }
}

// File drag and drop.
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  return [sender.draggingPasteboard
             canReadObjectForClasses:@[ [NSURL class] ]
                             options:@{
                               NSPasteboardURLReadingFileURLsOnlyKey : @YES
                             }]
             ? NSDragOperationCopy
             : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  NSArray<NSURL*>* urls = [sender.draggingPasteboard
      readObjectsForClasses:@[ [NSURL class] ]
                    options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  if (!urls.count || !window_) {
    return NO;
  }
  // Like on other platforms, only one file is handled.
  window_->HandleFileDrop(
      std::filesystem::path(urls[0].fileSystemRepresentation));
  return YES;
}

@end

@interface XeniaWindowDelegate : NSObject <NSWindowDelegate> {
 @public
  xe::ui::MacWindow* window_;
}
@end

@implementation XeniaWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
  // Closing is done by MacWindow itself so the common Window can go through
  // its closing phases.
  if (window_) {
    window_->HandleClose();
  }
  return NO;
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
  if (window_) {
    window_->HandleFocusUpdate(true);
  }
}

- (void)windowDidResignKey:(NSNotification*)notification {
  if (window_) {
    window_->HandleFocusUpdate(false);
  }
}

- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  if (window_) {
    window_->HandleFullscreenUpdate(true);
  }
}

- (void)windowDidExitFullScreen:(NSNotification*)notification {
  if (window_) {
    window_->HandleFullscreenUpdate(false);
  }
}

@end

@interface XeniaMenuItemTarget : NSObject {
 @public
  xe::ui::MacMenuItem* menu_item_;
}
- (void)activate:(id)sender;
@end

@implementation XeniaMenuItemTarget

- (void)activate:(id)sender {
  if (menu_item_) {
    // The menu item might be destroyed by its callback.
    menu_item_->Activate();
  }
}

@end

namespace xe {
namespace ui {

namespace {

VirtualKey TranslateKeyCode(unsigned short key_code) {
  switch (key_code) {
    case kVK_ANSI_A:
      return VirtualKey::kA;
    case kVK_ANSI_B:
      return VirtualKey::kB;
    case kVK_ANSI_C:
      return VirtualKey::kC;
    case kVK_ANSI_D:
      return VirtualKey::kD;
    case kVK_ANSI_E:
      return VirtualKey::kE;
    case kVK_ANSI_F:
      return VirtualKey::kF;
    case kVK_ANSI_G:
      return VirtualKey::kG;
    case kVK_ANSI_H:
      return VirtualKey::kH;
    case kVK_ANSI_I:
      return VirtualKey::kI;
    case kVK_ANSI_J:
      return VirtualKey::kJ;
    case kVK_ANSI_K:
      return VirtualKey::kK;
    case kVK_ANSI_L:
      return VirtualKey::kL;
    case kVK_ANSI_M:
      return VirtualKey::kM;
    case kVK_ANSI_N:
      return VirtualKey::kN;
    case kVK_ANSI_O:
      return VirtualKey::kO;
    case kVK_ANSI_P:
      return VirtualKey::kP;
    case kVK_ANSI_Q:
      return VirtualKey::kQ;
    case kVK_ANSI_R:
      return VirtualKey::kR;
    case kVK_ANSI_S:
      return VirtualKey::kS;
    case kVK_ANSI_T:
      return VirtualKey::kT;
    case kVK_ANSI_U:
      return VirtualKey::kU;
    case kVK_ANSI_V:
      return VirtualKey::kV;
    case kVK_ANSI_W:
      return VirtualKey::kW;
    case kVK_ANSI_X:
      return VirtualKey::kX;
    case kVK_ANSI_Y:
      return VirtualKey::kY;
    case kVK_ANSI_Z:
      return VirtualKey::kZ;
    case kVK_ANSI_0:
      return VirtualKey::k0;
    case kVK_ANSI_1:
      return VirtualKey::k1;
    case kVK_ANSI_2:
      return VirtualKey::k2;
    case kVK_ANSI_3:
      return VirtualKey::k3;
    case kVK_ANSI_4:
      return VirtualKey::k4;
    case kVK_ANSI_5:
      return VirtualKey::k5;
    case kVK_ANSI_6:
      return VirtualKey::k6;
    case kVK_ANSI_7:
      return VirtualKey::k7;
    case kVK_ANSI_8:
      return VirtualKey::k8;
    case kVK_ANSI_9:
      return VirtualKey::k9;
    case kVK_ANSI_Keypad0:
      return VirtualKey::kNumpad0;
    case kVK_ANSI_Keypad1:
      return VirtualKey::kNumpad1;
    case kVK_ANSI_Keypad2:
      return VirtualKey::kNumpad2;
    case kVK_ANSI_Keypad3:
      return VirtualKey::kNumpad3;
    case kVK_ANSI_Keypad4:
      return VirtualKey::kNumpad4;
    case kVK_ANSI_Keypad5:
      return VirtualKey::kNumpad5;
    case kVK_ANSI_Keypad6:
      return VirtualKey::kNumpad6;
    case kVK_ANSI_Keypad7:
      return VirtualKey::kNumpad7;
    case kVK_ANSI_Keypad8:
      return VirtualKey::kNumpad8;
    case kVK_ANSI_Keypad9:
      return VirtualKey::kNumpad9;
    case kVK_ANSI_KeypadMultiply:
      return VirtualKey::kMultiply;
    case kVK_ANSI_KeypadPlus:
      return VirtualKey::kAdd;
    case kVK_ANSI_KeypadMinus:
      return VirtualKey::kSubtract;
    case kVK_ANSI_KeypadDecimal:
      return VirtualKey::kDecimal;
    case kVK_ANSI_KeypadDivide:
      return VirtualKey::kDivide;
    case kVK_ANSI_KeypadEnter:
      return VirtualKey::kReturn;
    case kVK_ANSI_KeypadClear:
      return VirtualKey::kClear;
    case kVK_ANSI_Semicolon:
      return VirtualKey::kOem1;
    case kVK_ANSI_Equal:
      return VirtualKey::kOemPlus;
    case kVK_ANSI_Comma:
      return VirtualKey::kOemComma;
    case kVK_ANSI_Minus:
      return VirtualKey::kOemMinus;
    case kVK_ANSI_Period:
      return VirtualKey::kOemPeriod;
    case kVK_ANSI_Slash:
      return VirtualKey::kOem2;
    case kVK_ANSI_Grave:
      return VirtualKey::kOem3;
    case kVK_ANSI_LeftBracket:
      return VirtualKey::kOem4;
    case kVK_ANSI_Backslash:
      return VirtualKey::kOem5;
    case kVK_ANSI_RightBracket:
      return VirtualKey::kOem6;
    case kVK_ANSI_Quote:
      return VirtualKey::kOem7;
    case kVK_ISO_Section:
      return VirtualKey::kOem102;
    case kVK_Return:
      return VirtualKey::kReturn;
    case kVK_Tab:
      return VirtualKey::kTab;
    case kVK_Space:
      return VirtualKey::kSpace;
    case kVK_Delete:
      return VirtualKey::kBack;
    case kVK_ForwardDelete:
      return VirtualKey::kDelete;
    case kVK_Escape:
      return VirtualKey::kEscape;
    case kVK_Help:
      return VirtualKey::kInsert;
    case kVK_Home:
      return VirtualKey::kHome;
    case kVK_End:
      return VirtualKey::kEnd;
    case kVK_PageUp:
      return VirtualKey::kPrior;
    case kVK_PageDown:
      return VirtualKey::kNext;
    case kVK_LeftArrow:
      return VirtualKey::kLeft;
    case kVK_RightArrow:
      return VirtualKey::kRight;
    case kVK_UpArrow:
      return VirtualKey::kUp;
    case kVK_DownArrow:
      return VirtualKey::kDown;
    case kVK_Shift:
      return VirtualKey::kLShift;
    case kVK_RightShift:
      return VirtualKey::kRShift;
    case kVK_Control:
      return VirtualKey::kLControl;
    case kVK_RightControl:
      return VirtualKey::kRControl;
    case kVK_Option:
      return VirtualKey::kLMenu;
    case kVK_RightOption:
      return VirtualKey::kRMenu;
    case kVK_Command:
      return VirtualKey::kLWin;
    case kVK_RightCommand:
      return VirtualKey::kRWin;
    case kVK_CapsLock:
      return VirtualKey::kCapital;
    case kVK_F1:
      return VirtualKey::kF1;
    case kVK_F2:
      return VirtualKey::kF2;
    case kVK_F3:
      return VirtualKey::kF3;
    case kVK_F4:
      return VirtualKey::kF4;
    case kVK_F5:
      return VirtualKey::kF5;
    case kVK_F6:
      return VirtualKey::kF6;
    case kVK_F7:
      return VirtualKey::kF7;
    case kVK_F8:
      return VirtualKey::kF8;
    case kVK_F9:
      return VirtualKey::kF9;
    case kVK_F10:
      return VirtualKey::kF10;
    case kVK_F11:
      return VirtualKey::kF11;
    case kVK_F12:
      return VirtualKey::kF12;
    case kVK_F13:
      return VirtualKey::kF13;
    case kVK_F14:
      return VirtualKey::kF14;
    case kVK_F15:
      return VirtualKey::kF15;
    case kVK_F16:
      return VirtualKey::kF16;
    case kVK_F17:
      return VirtualKey::kF17;
    case kVK_F18:
      return VirtualKey::kF18;
    case kVK_F19:
      return VirtualKey::kF19;
    case kVK_F20:
      return VirtualKey::kF20;
    case kVK_Mute:
      return VirtualKey::kVolumeMute;
    case kVK_VolumeDown:
      return VirtualKey::kVolumeDown;
    case kVK_VolumeUp:
      return VirtualKey::kVolumeUp;
    default:
      return VirtualKey::kNone;
  }
}

// The device-dependent modifier flag for a modifier key, or 0 if not a
// modifier key with a pressed state.
NSUInteger GetModifierKeyMask(unsigned short key_code) {
  switch (key_code) {
    case kVK_Shift:
      return kDeviceLeftShiftKeyMask;
    case kVK_RightShift:
      return kDeviceRightShiftKeyMask;
    case kVK_Control:
      return kDeviceLeftControlKeyMask;
    case kVK_RightControl:
      return kDeviceRightControlKeyMask;
    case kVK_Option:
      return kDeviceLeftAlternateKeyMask;
    case kVK_RightOption:
      return kDeviceRightAlternateKeyMask;
    case kVK_Command:
      return kDeviceLeftCommandKeyMask;
    case kVK_RightCommand:
      return kDeviceRightCommandKeyMask;
    case kVK_CapsLock:
      return NSEventModifierFlagCapsLock;
    default:
      return 0;
  }
}

// The application menu, which is always the first item of the menu bar.
NSMenuItem* CreateApplicationMenuItem() {
  NSString* app_name = [NSProcessInfo processInfo].processName;
  NSMenu* app_menu = [[NSMenu alloc] initWithTitle:app_name];
  [app_menu addItemWithTitle:[@"Hide " stringByAppendingString:app_name]
                      action:@selector(hide:)
               keyEquivalent:@"h"];
  NSMenuItem* hide_others_item =
      [app_menu addItemWithTitle:@"Hide Others"
                          action:@selector(hideOtherApplications:)
                   keyEquivalent:@"h"];
  hide_others_item.keyEquivalentModifierMask =
      NSEventModifierFlagOption | NSEventModifierFlagCommand;
  [app_menu addItemWithTitle:@"Show All"
                      action:@selector(unhideAllApplications:)
               keyEquivalent:@""];
  [app_menu addItem:[NSMenuItem separatorItem]];
  [app_menu addItemWithTitle:[@"Quit " stringByAppendingString:app_name]
                      action:@selector(terminate:)
               keyEquivalent:@"q"];
  NSMenuItem* app_menu_item = [[NSMenuItem alloc] initWithTitle:app_name
                                                         action:nil
                                                  keyEquivalent:@""];
  app_menu_item.submenu = app_menu;
  return app_menu_item;
}

// Removes the Windows-style & mnemonic markers from a menu item title.
NSString* ToMenuTitle(const std::string& text) {
  std::string title;
  title.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '&') {
      if (i + 1 < text.size() && text[i + 1] == '&') {
        title.push_back('&');
        ++i;
      }
      continue;
    }
    title.push_back(text[i]);
  }
  return [NSString stringWithUTF8String:title.c_str()];
}

}  // namespace

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<MacWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

MacWindow::MacWindow(WindowedAppContext& app_context,
                     const std::string_view title,
                     uint32_t desired_logical_width,
                     uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width,
             desired_logical_height) {}

MacWindow::~MacWindow() {
  EnterDestructor();
  DestroyNativeWindow();
}

uint32_t MacWindow::GetLatestDpiImpl() const {
  CGFloat scale;
  if (ns_window_) {
    scale = ((__bridge NSWindow*)ns_window_).backingScaleFactor;
  } else {
    scale = [NSScreen mainScreen].backingScaleFactor;
  }
  return uint32_t(std::lround(GetMediumDpi() * scale));
}

bool MacWindow::OpenImpl() {
  @autoreleasepool {
    NSRect content_rect =
        NSMakeRect(0, 0, GetDesiredLogicalWidth(), GetDesiredLogicalHeight());
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:content_rect
                                    styleMask:NSWindowStyleMaskTitled |
                                              NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskMiniaturizable |
                                              NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    if (!window) {
      XELOGE("MacWindow: Failed to create an NSWindow");
      return false;
    }
    // Lifetime is managed by MacWindow.
    window.releasedWhenClosed = NO;
    window.title = [NSString stringWithUTF8String:GetTitle().c_str()];
    window.acceptsMouseMovedEvents = YES;
    window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;

    XeniaWindowDelegate* delegate = [[XeniaWindowDelegate alloc] init];
    delegate->window_ = this;
    window.delegate = delegate;

    XeniaMetalView* view = [[XeniaMetalView alloc] initWithFrame:content_rect];
    view->window_ = this;
    window.contentView = view;
    [window makeFirstResponder:view];

    ns_window_ = const_cast<void*>(CFBridgingRetain(window));
    ns_view_ = const_cast<void*>(CFBridgingRetain(view));
    ns_window_delegate_ = const_cast<void*>(CFBridgingRetain(delegate));

    [window center];
    [window makeKeyAndOrderFront:nil];

    InstallMainMenu();
    ApplyCursorVisibility();

    WindowDestructionReceiver destruction_receiver(this);

    NSRect backing_bounds = [view convertRectToBacking:view.bounds];
    OnActualSizeUpdate(uint32_t(backing_bounds.size.width),
                       uint32_t(backing_bounds.size.height),
                       WindowResizeAction::kManual, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return true;
    }

    if (window.isKeyWindow) {
      OnFocusUpdate(true, destruction_receiver);
      if (destruction_receiver.IsWindowDestroyedOrClosed()) {
        return true;
      }
    }

    // Enter fullscreen after the initial windowed layout, the transition is
    // asynchronous.
    if (IsFullscreen()) {
      [window toggleFullScreen:nil];
    }
  }
  return true;
}

void MacWindow::RequestCloseImpl() { HandleClose(); }

void MacWindow::DestroyNativeWindow() {
  if (cursor_hidden_) {
    [NSCursor unhide];
    cursor_hidden_ = false;
  }
  if (ns_view_) {
    XeniaMetalView* view = (XeniaMetalView*)CFBridgingRelease(ns_view_);
    ns_view_ = nullptr;
    view->window_ = nullptr;
  }
  if (ns_window_delegate_) {
    XeniaWindowDelegate* delegate =
        (XeniaWindowDelegate*)CFBridgingRelease(ns_window_delegate_);
    ns_window_delegate_ = nullptr;
    delegate->window_ = nullptr;
  }
  if (ns_window_) {
    NSWindow* window = (NSWindow*)CFBridgingRelease(ns_window_);
    ns_window_ = nullptr;
    window.delegate = nil;
    [window orderOut:nil];
    [window close];
  }
}

void MacWindow::HandleClose() {
  if (!ns_window_) {
    return;
  }
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    // The destructor has destroyed the native window.
    return;
  }
  DestroyNativeWindow();
  OnAfterClose();
}

void MacWindow::ApplyNewFullscreen() {
  NSWindow* window = (__bridge NSWindow*)ns_window_;
  bool is_fullscreen = (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
  if (is_fullscreen != IsFullscreen()) {
    // Asynchronous - completion is reported to HandleFullscreenUpdate.
    [window toggleFullScreen:nil];
  }
}

void MacWindow::ApplyNewTitle() {
  ((__bridge NSWindow*)ns_window_).title =
      [NSString stringWithUTF8String:GetTitle().c_str()];
}

void MacWindow::ApplyNewMainMenu(MenuItem* old_main_menu) { InstallMainMenu(); }

void MacWindow::CompleteMainMenuItemsUpdateImpl() {
  if (ns_window_ && ((__bridge NSWindow*)ns_window_).isKeyWindow) {
    [[NSApp mainMenu] update];
  }
}

void MacWindow::InstallMainMenu() {
  if (!ns_window_ || !((__bridge NSWindow*)ns_window_).isKeyWindow) {
    // Installed when the window becomes the key window.
    return;
  }
  const auto* main_menu = dynamic_cast<const MacMenuItem*>(GetMainMenu());
  if (main_menu && main_menu->handle()) {
    NSApp.mainMenu = (__bridge NSMenu*)main_menu->handle();
  } else {
    NSMenu* default_menu = [[NSMenu alloc] init];
    [default_menu addItem:CreateApplicationMenuItem()];
    NSApp.mainMenu = default_menu;
  }
}

void MacWindow::ApplyNewCursorVisibility(
    CursorVisibility old_cursor_visibility) {
  ApplyCursorVisibility();
}

void MacWindow::ApplyCursorVisibility() {
  bool hide = GetCursorVisibility() == CursorVisibility::kHidden;
  if (hide == cursor_hidden_) {
    return;
  }
  // [NSCursor hide] and unhide calls must be balanced.
  if (hide) {
    [NSCursor hide];
  } else {
    [NSCursor unhide];
  }
  cursor_hidden_ = hide;
}

void MacWindow::FocusImpl() {
  [NSApp activateIgnoringOtherApps:YES];
  [(__bridge NSWindow*)ns_window_ makeKeyAndOrderFront:nil];
}

std::unique_ptr<Surface> MacWindow::CreateSurfaceImpl(
    Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_MetalLayer) || !ns_view_) {
    return nullptr;
  }
  NSView* view = (__bridge NSView*)ns_view_;
  if (![view.layer isKindOfClass:[CAMetalLayer class]]) {
    XELOGE("MacWindow: The view is not backed by a CAMetalLayer");
    return nullptr;
  }
  return std::make_unique<MetalLayerSurface>((__bridge void*)view.layer);
}

void MacWindow::RequestPaintImpl() {
  // May be called from any thread. The block keeps the view alive, and
  // XeniaMetalView ignores painting once detached from the MacWindow.
  XeniaMetalView* view = (__bridge XeniaMetalView*)ns_view_;
  if (!view) {
    return;
  }
  if ([NSThread isMainThread]) {
    view.needsDisplay = YES;
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    view.needsDisplay = YES;
  });
}

void MacWindow::HandleSizeUpdate() {
  if (!ns_view_) {
    return;
  }
  NSView* view = (__bridge NSView*)ns_view_;
  NSRect backing_bounds = [view convertRectToBacking:view.bounds];
  NSWindow* window = (__bridge NSWindow*)ns_window_;
  if (!(window.styleMask & NSWindowStyleMaskFullScreen) && !window.zoomed) {
    NSSize content_size = window.contentLayoutRect.size;
    OnDesiredLogicalSizeUpdate(uint32_t(content_size.width),
                               uint32_t(content_size.height));
  }
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(uint32_t(backing_bounds.size.width),
                     uint32_t(backing_bounds.size.height),
                     WindowResizeAction::kManual, destruction_receiver);
}

void MacWindow::HandleFocusUpdate(bool has_focus) {
  if (has_focus) {
    InstallMainMenu();
  }
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(has_focus, destruction_receiver);
}

void MacWindow::HandleFullscreenUpdate(bool is_fullscreen) {
  OnDesiredFullscreenUpdate(is_fullscreen);
}

void MacWindow::HandleDpiChange() {
  WindowDestructionReceiver destruction_receiver(this);
  UISetupEvent e(this);
  OnDpiChanged(e, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) {
    return;
  }
  // The physical size changes with the backing scale factor.
  HandleSizeUpdate();
}

void MacWindow::HandlePaint() { OnPaint(); }

void MacWindow::HandleKeyEvent(void* event_ptr) {
  NSEvent* event = (__bridge NSEvent*)event_ptr;
  unsigned short key_code = event.keyCode;
  NSEventModifierFlags modifiers = event.modifierFlags;
  bool shift_pressed = modifiers & NSEventModifierFlagShift;
  bool ctrl_pressed = modifiers & NSEventModifierFlagControl;
  bool alt_pressed = modifiers & NSEventModifierFlagOption;
  bool capital_pressed = modifiers & NSEventModifierFlagCapsLock;
  VirtualKey vk = TranslateKeyCode(key_code);

  WindowDestructionReceiver destruction_receiver(this);

  if (event.type == NSEventTypeFlagsChanged) {
    // Modifier keys only report the new state of the modifier flags.
    NSUInteger key_mask = GetModifierKeyMask(key_code);
    uint64_t old_modifier_flags = last_modifier_flags_;
    last_modifier_flags_ = modifiers;
    if (!key_mask || vk == VirtualKey::kNone) {
      return;
    }
    bool is_down = (modifiers & key_mask) != 0;
    bool was_down = (old_modifier_flags & key_mask) != 0;
    if (is_down == was_down) {
      return;
    }
    KeyEvent e(this, vk, 1, was_down, shift_pressed, ctrl_pressed, alt_pressed,
               capital_pressed);
    if (is_down) {
      OnKeyDown(e, destruction_receiver);
    } else {
      OnKeyUp(e, destruction_receiver);
    }
    return;
  }

  if (event.type == NSEventTypeKeyDown) {
    KeyEvent e(this, vk, 1, event.ARepeat, shift_pressed, ctrl_pressed,
               alt_pressed, capital_pressed);
    OnKeyDown(e, destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return;
    }
    NSString* characters = event.characters;
    if (characters.length == 1) {
      unichar c = [characters characterAtIndex:0];
      // Skip control characters and the private use area that AppKit uses for
      // function keys.
      if (c >= 0x20 && c != 0x7F && (c < 0xF700 || c > 0xF8FF)) {
        e.set_unicode(uint16_t(c));
        OnKeyChar(e, destruction_receiver);
      }
    }
  } else if (event.type == NSEventTypeKeyUp) {
    KeyEvent e(this, vk, 1, true, shift_pressed, ctrl_pressed, alt_pressed,
               capital_pressed);
    OnKeyUp(e, destruction_receiver);
  }
}

void MacWindow::HandleMouseEvent(void* event_ptr) {
  NSEvent* event = (__bridge NSEvent*)event_ptr;
  NSView* view = (__bridge NSView*)ns_view_;
  if (!view) {
    return;
  }
  // The view is flipped, so the origin is at the top left like on other
  // platforms. Positions are in physical pixels, like the size.
  NSPoint location = [view convertPoint:event.locationInWindow fromView:nil];
  CGFloat scale = view.window ? view.window.backingScaleFactor : 1.0;
  int32_t x = int32_t(std::floor(location.x * scale));
  int32_t y = int32_t(std::floor(location.y * scale));

  MouseEvent::Button button = MouseEvent::Button::kNone;
  switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
      button = MouseEvent::Button::kLeft;
      break;
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
      button = MouseEvent::Button::kRight;
      break;
    case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp:
      switch (event.buttonNumber) {
        case 2:
          button = MouseEvent::Button::kMiddle;
          break;
        case 3:
          button = MouseEvent::Button::kX1;
          break;
        case 4:
          button = MouseEvent::Button::kX2;
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }

  int32_t scroll_x = 0;
  int32_t scroll_y = 0;
  if (event.type == NSEventTypeScrollWheel) {
    double delta_x = event.scrollingDeltaX;
    double delta_y = event.scrollingDeltaY;
    if (event.hasPreciseScrollingDeltas) {
      // Trackpads report in points - roughly 10 points per wheel detent.
      delta_x *= 0.1;
      delta_y *= 0.1;
    }
    // Positive is up (away from the user) in both AppKit and Xenia, but
    // positive horizontal scrolling in AppKit is to the left.
    scroll_x = int32_t(-delta_x * MouseEvent::kScrollPerDetent);
    scroll_y = int32_t(delta_y * MouseEvent::kScrollPerDetent);
  }

  WindowDestructionReceiver destruction_receiver(this);
  MouseEvent e(this, button, x, y, scroll_x, scroll_y);
  switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
      OnMouseDown(e, destruction_receiver);
      break;
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp:
      OnMouseUp(e, destruction_receiver);
      break;
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
      OnMouseMove(e, destruction_receiver);
      break;
    case NSEventTypeScrollWheel:
      OnMouseWheel(e, destruction_receiver);
      break;
    default:
      break;
  }
}

void MacWindow::HandleFileDrop(const std::filesystem::path& path) {
  WindowDestructionReceiver destruction_receiver(this);
  FileDropEvent e(this, path);
  OnFileDrop(e, destruction_receiver);
}

std::unique_ptr<ui::MenuItem> MenuItem::Create(Type type,
                                               const std::string& text,
                                               const std::string& hotkey,
                                               std::function<void()> callback) {
  return std::make_unique<MacMenuItem>(type, text, hotkey, callback);
}

MacMenuItem::MacMenuItem(Type type, const std::string& text,
                         const std::string& hotkey,
                         std::function<void()> callback)
    : MenuItem(type, text, hotkey, std::move(callback)) {
  // Hotkeys are handled by the window's key listeners rather than as key
  // equivalents, so they would be handled twice otherwise.
  switch (type) {
    case Type::kNormal:
    default: {
      // The menu bar - the first item is always the application menu.
      NSMenu* menu = [[NSMenu alloc] init];
      menu.autoenablesItems = NO;
      [menu addItem:CreateApplicationMenuItem()];
      handle_ = const_cast<void*>(CFBridgingRetain(menu));
    } break;
    case Type::kPopup: {
      NSString* title = ToMenuTitle(text);
      NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                    action:nil
                                             keyEquivalent:@""];
      NSMenu* submenu = [[NSMenu alloc] initWithTitle:title];
      submenu.autoenablesItems = NO;
      item.submenu = submenu;
      handle_ = const_cast<void*>(CFBridgingRetain(item));
    } break;
    case Type::kSeparator:
      handle_ = const_cast<void*>(CFBridgingRetain([NSMenuItem separatorItem]));
      break;
    case Type::kString: {
      XeniaMenuItemTarget* target = [[XeniaMenuItemTarget alloc] init];
      target->menu_item_ = this;
      NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:ToMenuTitle(text)
                                                    action:@selector(activate:)
                                             keyEquivalent:@""];
      item.target = target;
      target_ = const_cast<void*>(CFBridgingRetain(target));
      handle_ = const_cast<void*>(CFBridgingRetain(item));
    } break;
  }
}

MacMenuItem::~MacMenuItem() {
  if (target_) {
    XeniaMenuItemTarget* target =
        (XeniaMenuItemTarget*)CFBridgingRelease(target_);
    target->menu_item_ = nullptr;
    target_ = nullptr;
  }
  if (handle_) {
    id handle = CFBridgingRelease(handle_);
    handle_ = nullptr;
    if ([handle isKindOfClass:[NSMenuItem class]]) {
      NSMenuItem* item = handle;
      if (item.menu) {
        [item.menu removeItem:item];
      }
    }
  }
}

void MacMenuItem::SetEnabled(bool enabled) {
  id handle = (__bridge id)handle_;
  if ([handle isKindOfClass:[NSMenuItem class]]) {
    ((NSMenuItem*)handle).enabled = enabled ? YES : NO;
  }
}

void MacMenuItem::OnChildAdded(MenuItem* generic_child_item) {
  auto* child_item = dynamic_cast<MacMenuItem*>(generic_child_item);
  if (!child_item || !child_item->handle()) {
    return;
  }
  NSMenuItem* child = (__bridge NSMenuItem*)child_item->handle();
  if (![child isKindOfClass:[NSMenuItem class]]) {
    return;
  }
  id handle = (__bridge id)handle_;
  NSMenu* menu;
  if ([handle isKindOfClass:[NSMenu class]]) {
    menu = handle;
  } else {
    menu = ((NSMenuItem*)handle).submenu;
  }
  if (menu) {
    [menu addItem:child];
  }
}

void MacMenuItem::OnChildRemoved(MenuItem* generic_child_item) {
  auto* child_item = dynamic_cast<MacMenuItem*>(generic_child_item);
  if (!child_item || !child_item->handle()) {
    return;
  }
  NSMenuItem* child = (__bridge NSMenuItem*)child_item->handle();
  if ([child isKindOfClass:[NSMenuItem class]] && child.menu) {
    [child.menu removeItem:child];
  }
}

}  // namespace ui
}  // namespace xe
