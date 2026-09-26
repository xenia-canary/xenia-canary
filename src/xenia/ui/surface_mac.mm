/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/surface_mac.h"

#import <QuartzCore/CAMetalLayer.h>

#include <cmath>

namespace xe {
namespace ui {

MetalLayerSurface::MetalLayerSurface(void* layer)
    : layer_(
          const_cast<void*>(CFBridgingRetain((__bridge CAMetalLayer*)layer))) {}

MetalLayerSurface::~MetalLayerSurface() {
  if (layer_) {
    CFRelease(layer_);
  }
}

bool MetalLayerSurface::GetSizeImpl(uint32_t& width_out,
                                    uint32_t& height_out) const {
  CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_;
  CGSize size = layer.bounds.size;
  CGFloat scale = layer.contentsScale;
  width_out = uint32_t(std::lround(size.width * scale));
  height_out = uint32_t(std::lround(size.height * scale));
  return true;
}

}  // namespace ui
}  // namespace xe
