/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/icon_decode.h"

#include <cstdlib>

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/mstream.h>

namespace xe {
namespace ui {

wxBitmapBundle DecodePngIcon(const std::vector<uint8_t>& png, int size_px,
                             double scale) {
  if (png.empty()) {
    return wxBitmapBundle();
  }
  wxMemoryInputStream stream(png.data(), png.size());
  wxImage image;
  if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY)) {
    return wxBitmapBundle();
  }
  if (image.GetWidth() != size_px || image.GetHeight() != size_px) {
    image.Rescale(size_px, size_px, wxIMAGE_QUALITY_HIGH);
  }
  wxBitmap bmp(image);
  bmp.SetScaleFactor(scale);
  return wxBitmapBundle::FromBitmap(bmp);
}

wxBitmapBundle WrapRgbaIcon(const std::vector<uint8_t>& rgba, int width,
                            int height, int size_px, double scale) {
  if (rgba.empty() || width <= 0 || height <= 0 ||
      rgba.size() != static_cast<size_t>(width) * height * 4) {
    return wxBitmapBundle();
  }
  const size_t pixel_count = static_cast<size_t>(width) * height;
  auto* rgb = static_cast<unsigned char*>(std::malloc(pixel_count * 3));
  auto* alpha = static_cast<unsigned char*>(std::malloc(pixel_count));
  if (!rgb || !alpha) {
    std::free(rgb);
    std::free(alpha);
    return wxBitmapBundle();
  }
  for (size_t i = 0; i < pixel_count; ++i) {
    rgb[i * 3 + 0] = rgba[i * 4 + 0];
    rgb[i * 3 + 1] = rgba[i * 4 + 1];
    rgb[i * 3 + 2] = rgba[i * 4 + 2];
    alpha[i] = rgba[i * 4 + 3];
  }
  wxImage image(width, height, rgb, alpha);
  if (image.GetWidth() != size_px || image.GetHeight() != size_px) {
    image.Rescale(size_px, size_px, wxIMAGE_QUALITY_HIGH);
  }
  wxBitmap bmp(image);
  bmp.SetScaleFactor(scale);
  return wxBitmapBundle::FromBitmap(bmp);
}

}  // namespace ui
}  // namespace xe
