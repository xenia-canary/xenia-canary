/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_XLAST_H_
#define XENIA_KERNEL_UTIL_XLAST_H_

#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace util {
class XLast {
 public:
  XLast();
  XLast(const uint8_t* compressed_xml_data, const uint32_t compressed_data_size,
        const uint32_t decompressed_data_size);
  ~XLast();

  void Dump(std::string file_name);

 private:
  std::vector<uint8_t> xlast_decompressed_xml_;
};

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif
