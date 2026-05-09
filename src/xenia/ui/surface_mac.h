/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_MAC_H_
#define XENIA_UI_SURFACE_MAC_H_

#include <cstdint>

#include "xenia/ui/surface.h"

#ifdef __OBJC__
@class NSView;
@class CAMetalLayer;
#else
// Match wx/defs.h's DECLARE_WXCOCOA_OBJC_CLASS pattern so non-Obj-C TUs that
// include both headers don't hit a typedef/struct redefinition.
struct NSView;
#ifndef VULKAN_METAL_H_
struct CAMetalLayer;
#endif
#endif

namespace xe {
namespace ui {

class MacNSViewSurface final : public Surface {
 public:
  explicit MacNSViewSurface(NSView* view) : view_(view) {}
  TypeIndex GetType() const override { return kTypeIndex_MacNSView; }
  NSView* view() const { return view_; }
  double GetBackingScale() const;
  void ConfigureMetalLayer(uint32_t drawable_width, uint32_t drawable_height,
                           double contents_scale) const;
  CAMetalLayer* GetOrCreateMetalLayer() const;

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  NSView* view_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_MAC_H_
