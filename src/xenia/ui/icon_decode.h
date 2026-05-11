/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_ICON_DECODE_H_
#define XENIA_UI_ICON_DECODE_H_

#include <cstdint>
#include <vector>

#include <wx/bmpbndl.h>

namespace xe {
namespace ui {

// Decode PNG bytes via wxImage; rescale to size_px x size_px if needed; tag
// with the given DPI scale so wxWidgets renders correctly on hi-DPI displays.
// Returns an empty bundle on empty input or decode failure.
wxBitmapBundle DecodePngIcon(const std::vector<uint8_t>& png, int size_px,
                             double scale = 1.0);

// Wrap pre-decoded 8-bit RGBA pixels (row-major, width*height*4 bytes) into
// a wxBitmapBundle. wxImage takes ownership of the split RGB/alpha buffers
// (allocated with malloc, freed with free per the wxWidgets contract).
wxBitmapBundle WrapRgbaIcon(const std::vector<uint8_t>& rgba, int width,
                            int height, int size_px, double scale = 1.0);

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_ICON_DECODE_H_
