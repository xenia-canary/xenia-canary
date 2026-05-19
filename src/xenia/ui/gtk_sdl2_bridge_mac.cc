/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/gtk_sdl2_bridge_mac.h"
#include "xenia/ui/windowed_app_context_sdl2.h"
#include "xenia/base/logging.h"

#ifdef __APPLE__
#include "SDL.h"
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace xe {
namespace ui {

GTKSDL2Bridge::GTKSDL2Bridge()
    : sdl2_context_(nullptr),
      initialized_(false),
      quit_requested_(false) {
  fprintf(stderr, "[GTKSDL2Bridge] Constructor\n");
}

GTKSDL2Bridge::~GTKSDL2Bridge() {
  fprintf(stderr, "[GTKSDL2Bridge] Destructor\n");
  if (initialized_) {
    DestroyWindow();
    PlatformSpecificCleanup();
  }
}

bool GTKSDL2Bridge::Initialize(SDL2WindowedAppContext* sdl2_context) {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (initialized_) {
    fprintf(stderr, "[GTKSDL2Bridge] Already initialized\n");
    return true;
  }

  if (!sdl2_context) {
    fprintf(stderr, "[GTKSDL2Bridge] ERROR: SDL2 context is null\n");
    return false;
  }

  sdl2_context_ = sdl2_context;

  // Platform-specific initialization
  PlatformSpecificInitialization();

  initialized_ = true;
  fprintf(stderr, "[GTKSDL2Bridge] Initialized successfully\n");
  return true;
}

bool GTKSDL2Bridge::CreateWindow(const char* title, int width, int height) {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (!initialized_) {
    fprintf(stderr, "[GTKSDL2Bridge] ERROR: Bridge not initialized\n");
    return false;
  }

  if (window_) {
    fprintf(stderr, "[GTKSDL2Bridge] Window already exists, destroying first\n");
    DestroyWindow();
  }

  window_ = sdl2_context_->CreateWindow(title, width, height);
  if (!window_) {
    fprintf(stderr, "[GTKSDL2Bridge] ERROR: Failed to create SDL2 window\n");
    return false;
  }

  // Setup Metal surface if supported
  if (!SetupMetalSurface()) {
    fprintf(stderr, "[GTKSDL2Bridge] WARNING: Failed to setup Metal surface\n");
  }

  window_->Show();
  fprintf(stderr, "[GTKSDL2Bridge] Window created successfully: '%s' (%dx%d)\n", 
          title, width, height);
  return true;
}

void GTKSDL2Bridge::DestroyWindow() {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (window_) {
    fprintf(stderr, "[GTKSDL2Bridge] Destroying window\n");
    window_.reset();
  }
}

void* GTKSDL2Bridge::GetNativeWindow() const {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (!window_) {
    return nullptr;
  }

  return window_->GetNativeWindowHandle();
}

void* GTKSDL2Bridge::GetNativeView() const {
#ifdef __APPLE__
  void* native_window = GetNativeWindow();
  if (!native_window) {
    return nullptr;
  }

  // Get NSView from NSWindow
  typedef void* id_val;
  typedef void* SEL_val;
  typedef id_val (*content_view_fn)(id_val, SEL_val);
  
  id_val ns_window = reinterpret_cast<id_val>(native_window);
  return reinterpret_cast<content_view_fn>(objc_msgSend)(
      ns_window, sel_registerName("contentView"));
#else
  return GetNativeWindow();
#endif
}

SDL_Window* GTKSDL2Bridge::GetSDLWindow() const {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  return window_ ? window_->GetSDLWindow() : nullptr;
}

void GTKSDL2Bridge::ProcessEvents() {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (sdl2_context_) {
    sdl2_context_->ProcessEvents();
  }
}

bool GTKSDL2Bridge::ShouldQuit() const {
  return quit_requested_;
}

void* GTKSDL2Bridge::GetMetalLayer() const {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (!window_ || !sdl2_context_) {
    return nullptr;
  }

  void* native_window = GetNativeWindow();
  if (!native_window) {
    return nullptr;
  }

  return sdl2_context_->GetMetalLayer(native_window);
}

bool GTKSDL2Bridge::IsMetalSupported() const {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  return sdl2_context_ ? sdl2_context_->IsMetalSupported() : false;
}

void GTKSDL2Bridge::GetSize(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (window_) {
    window_->GetSize(width, height);
  } else {
    if (width) *width = 0;
    if (height) *height = 0;
  }
}

void GTKSDL2Bridge::SetSize(int width, int height) {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (window_) {
    window_->SetSize(width, height);
  }
}

void GTKSDL2Bridge::Show() {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (window_) {
    window_->Show();
  }
}

void GTKSDL2Bridge::Hide() {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  if (window_) {
    window_->Hide();
  }
}

bool GTKSDL2Bridge::IsValid() const {
  std::lock_guard<std::mutex> lock(bridge_mutex_);
  
  return initialized_ && window_ && window_->IsValid();
}

bool GTKSDL2Bridge::SetupMetalSurface() {
#ifdef __APPLE__
  if (!IsMetalSupported()) {
    fprintf(stderr, "[GTKSDL2Bridge] Metal not supported\n");
    return false;
  }

  void* metal_layer = GetMetalLayer();
  if (!metal_layer) {
    fprintf(stderr, "[GTKSDL2Bridge] Failed to get Metal layer\n");
    return false;
  }

  fprintf(stderr, "[GTKSDL2Bridge] Metal surface setup successful\n");
  return true;
#else
  fprintf(stderr, "[GTKSDL2Bridge] Metal surface not needed on this platform\n");
  return true;
#endif
}

void GTKSDL2Bridge::PlatformSpecificInitialization() {
#ifdef __APPLE__
  fprintf(stderr, "[GTKSDL2Bridge] Platform-specific initialization for macOS\n");
  // Additional macOS-specific setup if needed
#endif
}

void GTKSDL2Bridge::PlatformSpecificCleanup() {
#ifdef __APPLE__
  fprintf(stderr, "[GTKSDL2Bridge] Platform-specific cleanup for macOS\n");
  // Additional macOS-specific cleanup if needed
#endif
}

}  // namespace ui
}  // namespace xe
