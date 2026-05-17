/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_MAC_H_
#define XENIA_UI_SURFACE_MAC_H_

#include "xenia/ui/surface.h"

#ifdef __APPLE__
#include "gdk/gdk.h"
#else
#include <gdk/gdk.h>
#endif

namespace xe {
namespace ui {

inline constexpr Surface::TypeIndex kTypeIndex_MacNSView =
    static_cast<Surface::TypeIndex>(
        static_cast<uint32_t>(Surface::kTypeIndex_Win32Hwnd) + 1);
inline constexpr Surface::TypeFlags kTypeFlag_MacNSView =
    Surface::TypeFlags(1) << static_cast<uint32_t>(kTypeIndex_MacNSView);

class MacNSViewSurface : public Surface {
 public:
  MacNSViewSurface(void* nsview, void* metal_layer, GdkWindow* gdk_window)
      : nsview_(nsview), metal_layer_(metal_layer), gdk_window_(gdk_window) {}

  void* nsview() const { return nsview_; }
  void* metal_layer() const { return metal_layer_; }

  TypeIndex GetType() const override { return kTypeIndex_MacNSView; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
#ifdef __APPLE__
    width_out = gdk_window_get_width(gdk_window_);
    height_out = gdk_window_get_height(gdk_window_);
    return true;
#else
    return false;
#endif
  }

 private:
  void* nsview_;
  void* metal_layer_;
  GdkWindow* gdk_window_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_MAC_H_
