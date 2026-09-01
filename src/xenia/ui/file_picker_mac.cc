/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>

#include "xenia/ui/file_picker.h"

namespace xe {
namespace ui {

class MacFilePicker : public FilePicker {
 public:
  bool Show(Window* parent_window) override { return false; }
};

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<MacFilePicker>();
}

}  // namespace ui
}  // namespace xe
