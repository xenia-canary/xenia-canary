/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/surface_mac.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace xe {
namespace ui {

MacSurface::MacSurface(CAMetalLayer* layer, NSView* view)
    : layer_(layer), view_(view) {}

bool MacSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  if (!view_) {
    width_out = 0;
    height_out = 0;
    return false;
  }
  @autoreleasepool {
    NSRect bounds = [view_ bounds];
    CGFloat scale = [[view_ window] backingScaleFactor];
    if (scale <= 0.0) {
      scale = 1.0;
    }
    width_out = static_cast<uint32_t>(bounds.size.width * scale);
    height_out = static_cast<uint32_t>(bounds.size.height * scale);
    return width_out > 0 && height_out > 0;
  }
}

}  // namespace ui
}  // namespace xe
