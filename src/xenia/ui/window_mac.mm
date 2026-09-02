/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/ui/surface_mac.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window_mac.h"

namespace xe {
namespace ui {

VirtualKey MapMacKeyToVirtualKey(unsigned short keyCode) {
  switch (keyCode) {
    case 0x00: return VirtualKey::kA;
    case 0x01: return VirtualKey::kS;
    case 0x02: return VirtualKey::kD;
    case 0x03: return VirtualKey::kF;
    case 0x04: return VirtualKey::kH;
    case 0x05: return VirtualKey::kG;
    case 0x06: return VirtualKey::kZ;
    case 0x07: return VirtualKey::kX;
    case 0x08: return VirtualKey::kC;
    case 0x09: return VirtualKey::kV;
    case 0x0B: return VirtualKey::kB;
    case 0x0C: return VirtualKey::kQ;
    case 0x0D: return VirtualKey::kW;
    case 0x0E: return VirtualKey::kE;
    case 0x0F: return VirtualKey::kR;
    case 0x10: return VirtualKey::kY;
    case 0x11: return VirtualKey::kT;
    case 0x12: return VirtualKey::k1;
    case 0x13: return VirtualKey::k2;
    case 0x14: return VirtualKey::k3;
    case 0x15: return VirtualKey::k4;
    case 0x16: return VirtualKey::k6;
    case 0x17: return VirtualKey::k5;
    case 0x18: return VirtualKey::kOemPlus;
    case 0x19: return VirtualKey::k9;
    case 0x1A: return VirtualKey::k7;
    case 0x1B: return VirtualKey::kOemMinus;
    case 0x1C: return VirtualKey::k8;
    case 0x1D: return VirtualKey::k0;
    case 0x1E: return VirtualKey::kOem6;
    case 0x1F: return VirtualKey::kO;
    case 0x20: return VirtualKey::kU;
    case 0x21: return VirtualKey::kOem4;
    case 0x22: return VirtualKey::kI;
    case 0x23: return VirtualKey::kP;
    case 0x24: return VirtualKey::kReturn;
    case 0x25: return VirtualKey::kL;
    case 0x26: return VirtualKey::kJ;
    case 0x27: return VirtualKey::kOem7;
    case 0x28: return VirtualKey::kK;
    case 0x29: return VirtualKey::kOem1;
    case 0x2A: return VirtualKey::kOem5;
    case 0x2B: return VirtualKey::kOemComma;
    case 0x2C: return VirtualKey::kOem2;
    case 0x2D: return VirtualKey::kN;
    case 0x2E: return VirtualKey::kM;
    case 0x2F: return VirtualKey::kOemPeriod;
    case 0x30: return VirtualKey::kTab;
    case 0x31: return VirtualKey::kSpace;
    case 0x32: return VirtualKey::kOem3;
    case 0x33: return VirtualKey::kBack;
    case 0x35: return VirtualKey::kEscape;
    case 0x37: return VirtualKey::kLWin;
    case 0x38: return VirtualKey::kShift;
    case 0x39: return VirtualKey::kCapital;
    case 0x3A: return VirtualKey::kMenu;
    case 0x3B: return VirtualKey::kControl;
    case 0x7A: return VirtualKey::kF1;
    case 0x78: return VirtualKey::kF2;
    case 0x63: return VirtualKey::kF3;
    case 0x76: return VirtualKey::kF4;
    case 0x60: return VirtualKey::kF5;
    case 0x61: return VirtualKey::kF6;
    case 0x62: return VirtualKey::kF7;
    case 0x64: return VirtualKey::kF8;
    case 0x65: return VirtualKey::kF9;
    case 0x6D: return VirtualKey::kF10;
    case 0x67: return VirtualKey::kF11;
    case 0x6F: return VirtualKey::kF12;
    case 0x73: return VirtualKey::kHome;
    case 0x74: return VirtualKey::kPrior;
    case 0x75: return VirtualKey::kDelete;
    case 0x77: return VirtualKey::kEnd;
    case 0x79: return VirtualKey::kNext;
    case 0x7B: return VirtualKey::kLeft;
    case 0x7C: return VirtualKey::kRight;
    case 0x7D: return VirtualKey::kDown;
    case 0x7E: return VirtualKey::kUp;
    default: return VirtualKey::kNone;
  }
}

}  // namespace ui
}  // namespace xe

@interface XeniaContentView : NSView
@property (nonatomic, assign) xe::ui::MacWindow* macWindow;
@property (nonatomic, strong) CAMetalLayer* metalLayer;
@property (nonatomic, strong) NSTrackingArea* trackingArea;
- (instancetype)initWithFrame:(NSRect)frameRect macWindow:(xe::ui::MacWindow*)window;
@end

@implementation XeniaContentView

- (instancetype)initWithFrame:(NSRect)frameRect macWindow:(xe::ui::MacWindow*)window {
  self = [super initWithFrame:frameRect];
  if (self) {
    _macWindow = window;
    [self setWantsLayer:YES];
    _metalLayer = [CAMetalLayer layer];
    [_metalLayer setContentsScale:[[NSScreen mainScreen] backingScaleFactor]];
    [self setLayer:_metalLayer];
  }
  return self;
}

- (BOOL)wantsUpdateLayer {
  return YES;
}

- (CALayer*)makeBackingLayer {
  return [CAMetalLayer layer];
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_trackingArea) {
    [self removeTrackingArea:_trackingArea];
  }
  NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited |
                                  NSTrackingMouseMoved |
                                  NSTrackingActiveAlways |
                                  NSTrackingInVisibleRect;
  _trackingArea = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                               options:options
                                                 owner:self
                                              userInfo:nil];
  [self addTrackingArea:_trackingArea];
}

- (NSPoint)convertLocation:(NSEvent*)event {
  NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
  NSPoint backingPoint = [self convertPointToBacking:loc];
  NSSize backingSize = [self convertSizeToBacking:[self bounds].size];
  return NSMakePoint(backingPoint.x, backingSize.height - backingPoint.y);
}

- (void)mouseDown:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseDown(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                              xe::ui::MouseEvent::Button::kLeft);
}

- (void)mouseUp:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseUp(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                            xe::ui::MouseEvent::Button::kLeft);
}

- (void)rightMouseDown:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseDown(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                              xe::ui::MouseEvent::Button::kRight);
}

- (void)rightMouseUp:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseUp(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                            xe::ui::MouseEvent::Button::kRight);
}

- (void)otherMouseDown:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseDown(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                              xe::ui::MouseEvent::Button::kMiddle);
}

- (void)otherMouseUp:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseUp(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y),
                            xe::ui::MouseEvent::Button::kMiddle);
}

- (void)mouseMoved:(NSEvent*)event {
  if (!_macWindow) return;
  if (_macWindow->is_mouse_captured()) {
    // CGGetLastMouseDelta reads the raw HID delta and stays valid regardless of
    // cursor association state (NSEvent deltaX/Y can go stale after
    // CGAssociateMouseAndMouseCursorPosition(NO)).
    int32_t hdx = 0, hdy = 0;
    CGGetLastMouseDelta(&hdx, &hdy);
    if (hdx == 0 && hdy == 0) {
      hdx = static_cast<int32_t>([event deltaX]);
      hdy = static_cast<int32_t>([event deltaY]);
    }
    _macWindow->HandleMouseMoveRelative(hdx, hdy);
    return;
  }
  NSPoint p = [self convertLocation:event];
  _macWindow->HandleMouseMove(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y));
}

- (void)mouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
  [self mouseMoved:event];
}

- (void)scrollWheel:(NSEvent*)event {
  if (!_macWindow) return;
  NSPoint p = [self convertLocation:event];
  int32_t dy = static_cast<int32_t>(event.scrollingDeltaY * 120.0);
  int32_t dx = static_cast<int32_t>(event.scrollingDeltaX * 120.0);
  _macWindow->HandleMouseWheel(static_cast<int32_t>(p.x), static_cast<int32_t>(p.y), dx, dy);
}

- (void)keyDown:(NSEvent*)event {
  if (!_macWindow) return;
  xe::ui::VirtualKey key = xe::ui::MapMacKeyToVirtualKey(event.keyCode);
  NSEventModifierFlags flags = [event modifierFlags];
  bool shift = (flags & NSEventModifierFlagShift) != 0;
  bool ctrl = (flags & NSEventModifierFlagControl) != 0;
  bool alt = (flags & NSEventModifierFlagOption) != 0;
  bool super_key = (flags & NSEventModifierFlagCommand) != 0;
  _macWindow->HandleKeyDown(key, shift, ctrl, alt, super_key);

  NSString* chars = [event characters];
  if (chars && [chars length] > 0) {
    for (NSUInteger i = 0; i < [chars length]; ++i) {
      unichar ch = [chars characterAtIndex:i];
      _macWindow->HandleKeyChar(static_cast<xe::ui::VirtualKey>(ch), shift, ctrl, alt, super_key);
    }
  }
}

- (void)keyUp:(NSEvent*)event {
  if (!_macWindow) return;
  xe::ui::VirtualKey key = xe::ui::MapMacKeyToVirtualKey(event.keyCode);
  NSEventModifierFlags flags = [event modifierFlags];
  bool shift = (flags & NSEventModifierFlagShift) != 0;
  bool ctrl = (flags & NSEventModifierFlagControl) != 0;
  bool alt = (flags & NSEventModifierFlagOption) != 0;
  bool super_key = (flags & NSEventModifierFlagCommand) != 0;
  _macWindow->HandleKeyUp(key, shift, ctrl, alt, super_key);
}

@end

@interface XeniaWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) xe::ui::MacWindow* macWindow;
- (instancetype)initWithMacWindow:(xe::ui::MacWindow*)window;
@end

@implementation XeniaWindowDelegate

- (instancetype)initWithMacWindow:(xe::ui::MacWindow*)window {
  self = [super init];
  if (self) {
    _macWindow = window;
  }
  return self;
}

- (void)windowDidResize:(NSNotification*)notification {
  if (!_macWindow) return;
  NSWindow* win = _macWindow->native_window();
  if (!win) return;
  NSView* cv = [win contentView];
  NSRect bounds = [cv bounds];
  CGFloat scale = [win backingScaleFactor];
  if (scale <= 0.0) scale = 1.0;
  uint32_t logical_w = static_cast<uint32_t>(bounds.size.width);
  uint32_t logical_h = static_cast<uint32_t>(bounds.size.height);
  uint32_t phys_w = static_cast<uint32_t>(bounds.size.width * scale);
  uint32_t phys_h = static_cast<uint32_t>(bounds.size.height * scale);

  _macWindow->HandleResize(logical_w, logical_h, phys_w, phys_h);
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
  if (!_macWindow) return;
  _macWindow->HandleFocus(true);
}

- (void)windowDidResignKey:(NSNotification*)notification {
  if (!_macWindow) return;
  _macWindow->HandleFocus(false);
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (_macWindow) {
    _macWindow->RequestClose();
  }
  return NO;
}

@end

@interface XeniaMenuDelegate : NSObject <NSMenuDelegate>
+ (XeniaMenuDelegate*)sharedDelegate;
@end

@implementation XeniaMenuDelegate
+ (XeniaMenuDelegate*)sharedDelegate {
  static XeniaMenuDelegate* instance = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    instance = [[XeniaMenuDelegate alloc] init];
  });
  return instance;
}

- (void)menuWillOpen:(NSMenu *)menu {
  [menu setAutoenablesItems:NO];
  for (NSMenuItem* item in [menu itemArray]) {
    [item setEnabled:YES];
    if ([item submenu]) {
      [[item submenu] setAutoenablesItems:NO];
      for (NSMenuItem* subItem in [[item submenu] itemArray]) {
        [subItem setEnabled:YES];
      }
    }
  }
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
  [menu setAutoenablesItems:NO];
  for (NSMenuItem* item in [menu itemArray]) {
    [item setEnabled:YES];
    if ([item submenu]) {
      [[item submenu] setAutoenablesItems:NO];
      for (NSMenuItem* subItem in [[item submenu] itemArray]) {
        [subItem setEnabled:YES];
      }
    }
  }
}
@end

@interface XeniaMenuItemActionTarget : NSObject <NSMenuItemValidation, NSUserInterfaceValidations>
@property (nonatomic, assign) xe::ui::MacMenuItem* macMenuItem;
- (IBAction)menuItemTriggered:(id)sender;
- (BOOL)validateMenuItem:(NSMenuItem*)menuItem;
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item;
@end

@implementation XeniaMenuItemActionTarget
- (IBAction)menuItemTriggered:(id)sender {
  XELOGI("Cocoa menu item triggered: {}", _macMenuItem ? _macMenuItem->text() : "unknown");
  if (_macMenuItem) {
    struct MenuItemAccessor : public xe::ui::MenuItem {
      using xe::ui::MenuItem::OnSelected;
    };
    static_cast<MenuItemAccessor*>(static_cast<xe::ui::MenuItem*>(_macMenuItem))->OnSelected();
  }
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem {
  return YES;
}

- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item {
  return YES;
}
@end

namespace xe {
namespace ui {

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
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

MacWindow::~MacWindow() {
  EnterDestructor();
  if (window_) {
    [window_ setDelegate:nil];
    [window_ close];
    window_ = nil;
  }
  delegate_ = nil;
  content_view_ = nil;
}

uint32_t MacWindow::GetLatestDpiImpl() const {
  if (window_) {
    CGFloat scale = [window_ backingScaleFactor];
    return static_cast<uint32_t>(scale * 96.0);
  }
  return 96;
}

bool MacWindow::OpenImpl() {
  @autoreleasepool {
    NSRect contentRect = NSMakeRect(0, 0, GetDesiredLogicalWidth(),
                                    GetDesiredLogicalHeight());
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable;
    window_ = [[NSWindow alloc] initWithContentRect:contentRect
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [window_ setTitle:[NSString stringWithUTF8String:GetTitle().c_str()]];
    [window_ setMinSize:NSMakeSize(320, 240)];

    delegate_ = [[XeniaWindowDelegate alloc] initWithMacWindow:this];
    [window_ setDelegate:delegate_];

    content_view_ = [[XeniaContentView alloc] initWithFrame:contentRect
                                                  macWindow:this];
    [window_ setContentView:content_view_];
    [window_ makeFirstResponder:content_view_];
    // Deliver -mouseMoved: regardless of tracking areas — needed for captured
    // (decoupled-cursor) mouse-look, where the frozen cursor never crosses a
    // tracking rect.
    [window_ setAcceptsMouseMovedEvents:YES];
    [window_ center];
    [window_ makeKeyAndOrderFront:nil];
    [window_ orderFrontRegardless];
    [NSApp activateIgnoringOtherApps:YES];

    if (GetMainMenu()) {
      ApplyNewMainMenu(nullptr);
    }

    CGFloat scale = [window_ backingScaleFactor];
    if (scale <= 0.0) scale = 1.0;
    uint32_t phys_w = static_cast<uint32_t>(GetDesiredLogicalWidth() * scale);
    uint32_t phys_h = static_cast<uint32_t>(GetDesiredLogicalHeight() * scale);

    HandleResize(GetDesiredLogicalWidth(), GetDesiredLogicalHeight(), phys_w, phys_h);
    HandleFocus(true);

    return true;
  }
}

void MacWindow::RequestCloseImpl() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (!destruction_receiver.IsWindowDestroyed()) {
    if (window_) {
      [window_ setDelegate:nil];
      [window_ close];
      window_ = nil;
    }
    OnAfterClose();
  }
}

void MacWindow::ApplyNewFullscreen() {
  if (window_) {
    [window_ toggleFullScreen:nil];
  }
}

void MacWindow::ApplyNewTitle() {
  if (window_) {
    [window_ setTitle:[NSString stringWithUTF8String:GetTitle().c_str()]];
  }
}

void MacWindow::LoadAndApplyIcon(const void* buffer, size_t size,
                                 bool can_apply_state_in_current_phase) {}

void MacWindow::ApplyNewMainMenu(MenuItem* old_main_menu) {
  @autoreleasepool {
    auto* mac_menu = dynamic_cast<MacMenuItem*>(GetMainMenu());
    if (!mac_menu || !mac_menu->native_menu()) {
      return;
    }

    NSMenu* root_menu = (__bridge NSMenu*)mac_menu->native_menu();
    [root_menu setDelegate:[XeniaMenuDelegate sharedDelegate]];
    [root_menu setAutoenablesItems:NO];

    // Check if an Application menu exists at index 0. If not, insert standard macOS App Menu.
    if ([root_menu numberOfItems] == 0 ||
        ![[[root_menu itemAtIndex:0] title] isEqualToString:@"Xenia"]) {
      NSMenuItem* appMenuItem = [[NSMenuItem alloc] initWithTitle:@"Xenia" action:nil keyEquivalent:@""];
      NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"Xenia"];
      [appMenu setDelegate:[XeniaMenuDelegate sharedDelegate]];
      [appMenu setAutoenablesItems:NO];

      // About
      NSMenuItem* aboutItem = [[NSMenuItem alloc] initWithTitle:@"About Xenia Canary"
                                                         action:@selector(orderFrontStandardAboutPanel:)
                                                  keyEquivalent:@""];
      [aboutItem setTarget:NSApp];
      [appMenu addItem:aboutItem];

      [appMenu addItem:[NSMenuItem separatorItem]];

      // Services
      NSMenuItem* servicesItem = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
      NSMenu* servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
      [servicesItem setSubmenu:servicesMenu];
      [NSApp setServicesMenu:servicesMenu];
      [appMenu addItem:servicesItem];

      [appMenu addItem:[NSMenuItem separatorItem]];

      // Hide
      NSMenuItem* hideItem = [[NSMenuItem alloc] initWithTitle:@"Hide Xenia"
                                                        action:@selector(hide:)
                                                 keyEquivalent:@"h"];
      [hideItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
      [hideItem setTarget:NSApp];
      [appMenu addItem:hideItem];

      // Hide Others
      NSMenuItem* hideOthersItem = [[NSMenuItem alloc] initWithTitle:@"Hide Others"
                                                              action:@selector(hideOtherApplications:)
                                                       keyEquivalent:@"h"];
      [hideOthersItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagOption];
      [hideOthersItem setTarget:NSApp];
      [appMenu addItem:hideOthersItem];

      // Show All
      NSMenuItem* showAllItem = [[NSMenuItem alloc] initWithTitle:@"Show All"
                                                           action:@selector(unhideAllApplications:)
                                                    keyEquivalent:@""];
      [showAllItem setTarget:NSApp];
      [appMenu addItem:showAllItem];

      [appMenu addItem:[NSMenuItem separatorItem]];

      // Quit
      NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit Xenia"
                                                        action:@selector(terminate:)
                                                 keyEquivalent:@"q"];
      [quitItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
      [quitItem setTarget:NSApp];
      [appMenu addItem:quitItem];

      [appMenuItem setSubmenu:appMenu];
      [root_menu insertItem:appMenuItem atIndex:0];
    }

    for (NSMenuItem* item in [root_menu itemArray]) {
      [item setEnabled:YES];
      if ([item submenu]) {
        [[item submenu] setDelegate:[XeniaMenuDelegate sharedDelegate]];
        [[item submenu] setAutoenablesItems:NO];
        for (NSMenuItem* subItem in [[item submenu] itemArray]) {
          [subItem setEnabled:YES];
          if ([subItem submenu]) {
            [[subItem submenu] setDelegate:[XeniaMenuDelegate sharedDelegate]];
            [[subItem submenu] setAutoenablesItems:NO];
            for (NSMenuItem* subSubItem in [[subItem submenu] itemArray]) {
              [subSubItem setEnabled:YES];
            }
          }
        }
      }
    }

    [NSApp setMainMenu:root_menu];
  }
}

void MacWindow::CompleteMainMenuItemsUpdateImpl() {
  @autoreleasepool {
    auto* mac_menu = dynamic_cast<MacMenuItem*>(GetMainMenu());
    if (mac_menu && mac_menu->native_menu()) {
      NSMenu* root_menu = (__bridge NSMenu*)mac_menu->native_menu();
      [root_menu update];
    }
  }
}

// Center of the content view in CoreGraphics global coordinates (origin at the
// top-left of the primary display) — what CGWarpMouseCursorPosition expects.
static CGPoint MacContentCenterGlobal(NSWindow* window, NSView* content_view) {
  NSRect in_window = [content_view convertRect:[content_view bounds] toView:nil];
  NSRect on_screen = [window convertRectToScreen:in_window];  // Cocoa, y-up
  CGFloat primary_h =
      NSHeight([(NSScreen*)[[NSScreen screens] firstObject] frame]);
  return CGPointMake(NSMidX(on_screen), primary_h - NSMidY(on_screen));
}

void MacWindow::ApplyNewMouseCapture() {
  if (mouse_captured_) {
    return;
  }
  mouse_captured_ = true;
  synth_mouse_x_ = 0.0;
  synth_mouse_y_ = 0.0;
  // Park the visible cursor at the center of the Xenia content area *before*
  // decoupling — a warp after CGAssociate...(NO) won't move the on-screen cursor
  // and mouseMoved: events would keep routing to wherever it was left.
  if (window_ && content_view_) {
    CGWarpMouseCursorPosition(
        MacContentCenterGlobal(window_, reinterpret_cast<NSView*>(content_view_)));
  }
  // Decouple the hardware pointer: NSEvent deltaX/Y keep flowing and never stall
  // at a display edge.
  CGAssociateMouseAndMouseCursorPosition(NO);
  if (GetCursorVisibility() != CursorVisibility::kHidden &&
      !mouse_captured_hid_cursor_) {
    [NSCursor hide];
    mouse_captured_hid_cursor_ = true;
  }
}

void MacWindow::ApplyNewMouseRelease() {
  if (!mouse_captured_) {
    return;
  }
  mouse_captured_ = false;
  CGAssociateMouseAndMouseCursorPosition(YES);
  if (mouse_captured_hid_cursor_) {
    [NSCursor unhide];
    mouse_captured_hid_cursor_ = false;
  }
}

void MacWindow::HandleMouseMoveRelative(double dx, double dy) {
  synth_mouse_x_ += dx;
  synth_mouse_y_ += dy;
  HandleMouseMove(static_cast<int32_t>(synth_mouse_x_),
                  static_cast<int32_t>(synth_mouse_y_));
}

void MacWindow::ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) {
  if (GetCursorVisibility() == CursorVisibility::kHidden) {
    [NSCursor hide];
  } else {
    [NSCursor unhide];
  }
}

void MacWindow::FocusImpl() {
  if (window_) {
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
  }
}

std::unique_ptr<Surface> MacWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!(allowed_types & Surface::kTypeFlag_MetalLayer)) {
    return nullptr;
  }
  if (!content_view_) {
    return nullptr;
  }
  return std::make_unique<MacSurface>(content_view_.metalLayer, content_view_);
}

void MacWindow::RequestPaintImpl() {
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  app_context().CallInUIThreadDeferred([this]() {
    paint_pending_.store(false, std::memory_order_release);
    OnPaint();
  });
}

void MacWindow::HandleMouseDown(int32_t x, int32_t y, MouseEvent::Button button) {
  MouseEvent e(this, button, x, y);
  WindowDestructionReceiver r(this);
  OnMouseDown(e, r);
}

void MacWindow::HandleMouseUp(int32_t x, int32_t y, MouseEvent::Button button) {
  MouseEvent e(this, button, x, y);
  WindowDestructionReceiver r(this);
  OnMouseUp(e, r);
}

void MacWindow::HandleMouseMove(int32_t x, int32_t y) {
  MouseEvent e(this, MouseEvent::Button::kNone, x, y);
  WindowDestructionReceiver r(this);
  OnMouseMove(e, r);
}

void MacWindow::HandleMouseWheel(int32_t x, int32_t y, int32_t dx, int32_t dy) {
  MouseEvent e(this, MouseEvent::Button::kNone, x, y, dx, dy);
  WindowDestructionReceiver r(this);
  OnMouseWheel(e, r);
}

void MacWindow::HandleKeyDown(VirtualKey key, bool shift, bool ctrl, bool alt, bool super) {
  KeyEvent e(this, key, 0, false, shift, ctrl, alt, super);
  WindowDestructionReceiver r(this);
  OnKeyDown(e, r);
}

void MacWindow::HandleKeyUp(VirtualKey key, bool shift, bool ctrl, bool alt, bool super) {
  KeyEvent e(this, key, 0, false, shift, ctrl, alt, super);
  WindowDestructionReceiver r(this);
  OnKeyUp(e, r);
}

void MacWindow::HandleKeyChar(VirtualKey key, bool shift, bool ctrl, bool alt, bool super) {
  KeyEvent e(this, key, 0, false, shift, ctrl, alt, super);
  WindowDestructionReceiver r(this);
  OnKeyChar(e, r);
}

void MacWindow::HandleResize(uint32_t logical_w, uint32_t logical_h, uint32_t phys_w, uint32_t phys_h) {
  OnDesiredLogicalSizeUpdate(logical_w, logical_h);
  WindowDestructionReceiver r(this);
  OnActualSizeUpdate(phys_w, phys_h, WindowResizeAction::kManual, r);
}

void MacWindow::HandleFocus(bool has_focus) {
  WindowDestructionReceiver r(this);
  OnFocusUpdate(has_focus, r);
}

static void ParseMacHotkey(const std::string& hotkey, NSString** out_key_eq, NSEventModifierFlags* out_mask) {
  if (hotkey.empty()) {
    *out_key_eq = @"";
    *out_mask = 0;
    return;
  }

  NSString* hk = [NSString stringWithUTF8String:hotkey.c_str()];
  NSEventModifierFlags mask = 0;
  NSString* key = @"";

  if ([hk containsString:@"Ctrl+"] || [hk containsString:@"Ctrl +"]) {
    mask |= NSEventModifierFlagCommand;
    hk = [hk stringByReplacingOccurrencesOfString:@"Ctrl+" withString:@""];
    hk = [hk stringByReplacingOccurrencesOfString:@"Ctrl +" withString:@""];
  }
  if ([hk containsString:@"Alt+"] || [hk containsString:@"Alt +"]) {
    mask |= NSEventModifierFlagOption;
    hk = [hk stringByReplacingOccurrencesOfString:@"Alt+" withString:@""];
    hk = [hk stringByReplacingOccurrencesOfString:@"Alt +" withString:@""];
  }
  if ([hk containsString:@"Shift+"] || [hk containsString:@"Shift +"]) {
    mask |= NSEventModifierFlagShift;
    hk = [hk stringByReplacingOccurrencesOfString:@"Shift+" withString:@""];
    hk = [hk stringByReplacingOccurrencesOfString:@"Shift +" withString:@""];
  }

  hk = [hk stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];

  if ([hk isEqualToString:@"F1"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF1FunctionKey];
  } else if ([hk isEqualToString:@"F2"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF2FunctionKey];
  } else if ([hk isEqualToString:@"F3"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF3FunctionKey];
  } else if ([hk isEqualToString:@"F4"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF4FunctionKey];
  } else if ([hk isEqualToString:@"F5"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF5FunctionKey];
  } else if ([hk isEqualToString:@"F6"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF6FunctionKey];
  } else if ([hk isEqualToString:@"F7"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF7FunctionKey];
  } else if ([hk isEqualToString:@"F8"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF8FunctionKey];
  } else if ([hk isEqualToString:@"F9"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF9FunctionKey];
  } else if ([hk isEqualToString:@"F10"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF10FunctionKey];
  } else if ([hk isEqualToString:@"F11"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF11FunctionKey];
  } else if ([hk isEqualToString:@"F12"]) {
    key = [NSString stringWithFormat:@"%C", (unichar)NSF12FunctionKey];
  } else if ([hk isEqualToString:@"Numpad *"]) {
    key = @"*";
  } else if ([hk isEqualToString:@"Numpad -"]) {
    key = @"-";
  } else if ([hk isEqualToString:@"Numpad +"]) {
    key = @"+";
  } else if ([hk isEqualToString:@"Numpad /"]) {
    key = @"/";
  } else if ([hk isEqualToString:@"Pause/Break"]) {
    key = @"b";
    mask |= NSEventModifierFlagControl;
  } else if ([hk length] == 1) {
    key = [hk lowercaseString];
  } else {
    key = @"";
  }

  *out_key_eq = key;
  *out_mask = mask;
}

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  return std::make_unique<MacMenuItem>(type, text, hotkey, std::move(callback));
}

MacMenuItem::MacMenuItem(Type type, const std::string& text,
                         const std::string& hotkey,
                         std::function<void()> callback)
    : MenuItem(type, text, hotkey, std::move(callback)) {
  @autoreleasepool {
    NSString* title = [NSString stringWithUTF8String:text.c_str()];
    title = [title stringByReplacingOccurrencesOfString:@"&" withString:@""];

    switch (type) {
      case Type::kNormal: {
        NSMenu* m = [[NSMenu alloc] initWithTitle:title];
        [m setAutoenablesItems:NO];
        menu_ = (void*)CFBridgingRetain(m);
        break;
      }
      case Type::kPopup: {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
        [sub setAutoenablesItems:NO];
        [it setSubmenu:sub];
        item_ = (void*)CFBridgingRetain(it);
        menu_ = (void*)CFBridgingRetain(sub);
        break;
      }
      case Type::kSeparator:
        item_ = (void*)CFBridgingRetain([NSMenuItem separatorItem]);
        break;
      case Type::kString: {
        XeniaMenuItemActionTarget* target = [[XeniaMenuItemActionTarget alloc] init];
        [target setMacMenuItem:this];
        target_ = (void*)CFBridgingRetain(target);
        NSString* keyEq = @"";
        NSEventModifierFlags modMask = 0;
        ParseMacHotkey(hotkey, &keyEq, &modMask);
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title
                                                    action:@selector(menuItemTriggered:)
                                             keyEquivalent:keyEq];
        [it setKeyEquivalentModifierMask:modMask];
        [it setTarget:target];
        [it setEnabled:YES];
        item_ = (void*)CFBridgingRetain(it);
        break;
      }
    }
  }
}

MacMenuItem::~MacMenuItem() {
  if (target_) {
    CFRelease(target_);
    target_ = nullptr;
  }
  if (menu_) {
    CFRelease(menu_);
    menu_ = nullptr;
  }
  if (item_) {
    CFRelease(item_);
    item_ = nullptr;
  }
}

void MacMenuItem::SetEnabled(bool enabled) {
  if (item_) {
    [(__bridge NSMenuItem*)item_ setEnabled:enabled ? YES : NO];
  }
  if (menu_) {
    NSMenu* menu = (__bridge NSMenu*)menu_;
    [menu setAutoenablesItems:NO];
    for (NSMenuItem* it in [menu itemArray]) {
      [it setEnabled:enabled ? YES : NO];
      if ([it submenu]) {
        [[it submenu] setAutoenablesItems:NO];
        for (NSMenuItem* subIt in [[it submenu] itemArray]) {
          [subIt setEnabled:enabled ? YES : NO];
        }
      }
    }
  }
  for (const auto& child : children_) {
    if (child) {
      child->SetEnabled(enabled);
    }
  }
}

void MacMenuItem::OnChildAdded(MenuItem* generic_child_item) {
  auto* child_item = dynamic_cast<MacMenuItem*>(generic_child_item);
  if (!child_item || !menu_) return;
  NSMenu* menu = (__bridge NSMenu*)menu_;
  [menu setAutoenablesItems:NO];
  if (child_item->item_) {
    NSMenuItem* it = (__bridge NSMenuItem*)child_item->item_;
    [it setEnabled:YES];
    if ([it submenu]) {
      [[it submenu] setAutoenablesItems:NO];
    }
    [menu addItem:it];
  }
}

void MacMenuItem::OnChildRemoved(MenuItem* generic_child_item) {
  auto* child_item = dynamic_cast<MacMenuItem*>(generic_child_item);
  if (!child_item || !menu_) return;
  NSMenu* menu = (__bridge NSMenu*)menu_;
  if (child_item->item_) {
    [menu removeItem:(__bridge NSMenuItem*)child_item->item_];
  }
}

}  // namespace ui
}  // namespace xe
