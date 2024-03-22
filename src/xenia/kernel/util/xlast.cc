/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/xlast.h"
#include "third_party/zlib/zlib.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace util {

XLast::XLast() {}

XLast::XLast(const uint8_t* compressed_xml_data,
             const uint32_t compressed_data_size,
             const uint32_t decompressed_data_size) {
  if (!compressed_data_size || !decompressed_data_size) {
    XELOGW("XLast: Current title don't have any XLast XML data!");
    return;
  }

  xlast_decompressed_xml_.resize(decompressed_data_size);

  z_stream stream;
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;
  stream.avail_in = 0;
  stream.next_in = Z_NULL;

  int ret = inflateInit2(
      &stream, 16 + MAX_WBITS);  // 16 + MAX_WBITS enables gzip decoding
  if (ret != Z_OK) {
    XELOGE("XLast: Error during Zlib stream init");
    return;
  }

  stream.avail_in = compressed_data_size;
  stream.next_in =
      reinterpret_cast<Bytef*>(const_cast<uint8_t*>(compressed_xml_data));
  stream.avail_out = decompressed_data_size;
  stream.next_out = reinterpret_cast<Bytef*>(xlast_decompressed_xml_.data());

  ret = inflate(&stream, Z_NO_FLUSH);
  if (ret == Z_STREAM_ERROR) {
    XELOGE("XLast: Error during XLast decompression");
    inflateEnd(&stream);
    return;
  }
  inflateEnd(&stream);
}

XLast::~XLast() {}

void XLast::Dump(std::string file_name) {
  if (xlast_decompressed_xml_.empty()) {
    return;
  }

  if (file_name.empty()) {
    // TODO: Read default xlast name from it and use that one.
  }

  FILE* outfile =
      xe::filesystem::OpenFile(fmt::format("{}.xml", file_name).c_str(), "ab");
  if (!outfile) {
    return;
  }

  fwrite(xlast_decompressed_xml_.data(), 1, xlast_decompressed_xml_.size(),
         outfile);
  fclose(outfile);
}

}  // namespace util
}  // namespace kernel
}  // namespace xe