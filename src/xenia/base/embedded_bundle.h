/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_EMBEDDED_BUNDLE_H_
#define XENIA_BASE_EMBEDDED_BUNDLE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace xe {

// Decompresses a zlib-compressed concat blob produced by tools/build/
// embed_bundle.py and exposes a callback iteration over (name, data) entries.
// Holds the inflated bytes; emitted string_views point into it, so the
// EmbeddedBundle must outlive any view captured during ForEach.
class EmbeddedBundle {
 public:
  EmbeddedBundle(const unsigned char* compressed_data, size_t compressed_size);

  bool ok() const { return !raw_.empty(); }

  using VisitFn =
      std::function<void(std::string_view name, std::string_view data)>;
  void ForEach(const VisitFn& visit) const;

 private:
  std::vector<uint8_t> raw_;
};

}  // namespace xe

#endif  // XENIA_BASE_EMBEDDED_BUNDLE_H_
