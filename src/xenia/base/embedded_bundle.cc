/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/embedded_bundle.h"

#include <cstring>

#include "third_party/zlib-ng/zlib-ng.h"
#include "xenia/base/logging.h"

namespace xe {

EmbeddedBundle::EmbeddedBundle(const unsigned char* compressed_data,
                               size_t compressed_size) {
  if (!compressed_data || compressed_size == 0) {
    return;
  }
  // Text patches compress ~5x; start at 4x and grow on demand.
  raw_.resize(compressed_size * 4);

  zng_stream stream{};
  stream.next_in = const_cast<unsigned char*>(compressed_data);
  stream.avail_in = static_cast<unsigned int>(compressed_size);
  stream.next_out = raw_.data();
  stream.avail_out = static_cast<unsigned int>(raw_.size());

  if (zng_inflateInit(&stream) != Z_OK) {
    raw_.clear();
    return;
  }

  while (true) {
    int ret = zng_inflate(&stream, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret == Z_OK && stream.avail_out == 0) {
      size_t old_size = raw_.size();
      raw_.resize(old_size * 2);
      stream.next_out = raw_.data() + old_size;
      stream.avail_out = static_cast<unsigned int>(raw_.size() - old_size);
      continue;
    }
    if (ret != Z_OK) {
      XELOGE("EmbeddedBundle: inflate failed ({})", ret);
      zng_inflateEnd(&stream);
      raw_.clear();
      return;
    }
  }

  raw_.resize(stream.total_out);
  zng_inflateEnd(&stream);
}

void EmbeddedBundle::ForEach(const VisitFn& visit) const {
  if (raw_.size() < sizeof(uint32_t)) {
    return;
  }
  const uint8_t* p = raw_.data();
  const uint8_t* end = p + raw_.size();

  uint32_t count;
  std::memcpy(&count, p, sizeof(count));
  p += sizeof(count);

  for (uint32_t i = 0; i < count; ++i) {
    if (p + sizeof(uint32_t) > end) {
      return;
    }
    uint32_t name_len;
    std::memcpy(&name_len, p, sizeof(name_len));
    p += sizeof(name_len);
    if (p + name_len > end) {
      return;
    }
    std::string_view name(reinterpret_cast<const char*>(p), name_len);
    p += name_len;

    if (p + sizeof(uint32_t) > end) {
      return;
    }
    uint32_t data_len;
    std::memcpy(&data_len, p, sizeof(data_len));
    p += sizeof(data_len);
    if (p + data_len > end) {
      return;
    }
    std::string_view data(reinterpret_cast<const char*>(p), data_len);
    p += data_len;

    visit(name, data);
  }
}

}  // namespace xe
