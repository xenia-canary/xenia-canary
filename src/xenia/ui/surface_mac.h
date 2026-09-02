/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_SURFACE_MAC_H_
#define XENIA_UI_SURFACE_MAC_H_

#include "xenia/ui/surface.h"

#ifdef __OBJC__
@class CAMetalLayer;
@class NSView;
#else
typedef void CAMetalLayer;
typedef void NSView;
#endif

namespace xe {
namespace ui {

class MacSurface : public Surface {
 public:
  MacSurface(CAMetalLayer* layer, NSView* view);
  ~MacSurface() override = default;

  TypeIndex GetType() const override { return kTypeIndex_MetalLayer; }

  CAMetalLayer* metal_layer() const { return layer_; }
  NSView* view() const { return view_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  CAMetalLayer* layer_ = nullptr;
  NSView* view_ = nullptr;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_MAC_H_
