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

namespace xe {
namespace ui {

// A CAMetalLayer, for VK_EXT_metal_surface (MoltenVK).
class MetalLayerSurface final : public Surface {
 public:
  // Takes a strong reference to the CAMetalLayer.
  explicit MetalLayerSurface(void* layer);
  ~MetalLayerSurface() override;
  TypeIndex GetType() const override { return kTypeIndex_MetalLayer; }
  // CAMetalLayer*.
  void* layer() const { return layer_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  void* layer_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_SURFACE_MAC_H_
