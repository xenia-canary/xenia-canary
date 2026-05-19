/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GTK_SDL2_BRIDGE_MAC_H_
#define XENIA_UI_GTK_SDL2_BRIDGE_MAC_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

struct SDL_Window;

namespace xe {
namespace ui {

class SDL2WindowedAppContext;
class SDL2Window;

// Bridge between GTK UI and SDL2 window management
// GTK handles menus and UI elements, SDL2 handles the main window and Metal surface
class GTKSDL2Bridge {
 public:
  GTKSDL2Bridge();
  ~GTKSDL2Bridge();

  // Initialize the bridge with both GTK and SDL2 contexts
  bool Initialize(SDL2WindowedAppContext* sdl2_context);
  
  // Window management
  bool CreateWindow(const char* title, int width, int height);
  void DestroyWindow();
  
  // Get native window handles for different backends
  void* GetNativeWindow() const;
  void* GetNativeView() const;  // NSView on macOS
  SDL_Window* GetSDLWindow() const;
  
  // Event handling coordination
  void ProcessEvents();
  bool ShouldQuit() const;
  
  // Metal surface management
  void* GetMetalLayer() const;
  bool IsMetalSupported() const;
  
  // Size management
  void GetSize(int* width, int* height) const;
  void SetSize(int width, int height);
  
  // Visibility
  void Show();
  void Hide();
  
  // State checking
  bool IsValid() const;
  bool IsInitialized() const { return initialized_; }

 private:
  SDL2WindowedAppContext* sdl2_context_;
  std::unique_ptr<SDL2Window> window_;
  bool initialized_;
  bool quit_requested_;
  mutable std::mutex bridge_mutex_;
  
  // Platform-specific helper methods
  bool SetupMetalSurface();
  void PlatformSpecificInitialization();
  void PlatformSpecificCleanup();
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GTK_SDL2_BRIDGE_MAC_H_
