/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/file_picker.h"

#import <Cocoa/Cocoa.h>

namespace xe {
namespace ui {

class MacFilePicker : public FilePicker {
 public:
  MacFilePicker() = default;
  ~MacFilePicker() override = default;

  bool Show(Window* parent_window) override;
};

bool MacFilePicker::Show(Window* parent_window) {
  @autoreleasepool {
    if (mode() == Mode::kOpen) {
      NSOpenPanel* panel = [NSOpenPanel openPanel];
      panel.canChooseFiles = (type() == Type::kFile);
      panel.canChooseDirectories = (type() == Type::kDirectory);
      panel.allowsMultipleSelection = multi_selection();
      if (!title().empty()) {
        panel.title = [NSString stringWithUTF8String:title().c_str()];
      }
      if (NSModalResponseOK != [panel runModal]) {
        return false;
      }
      std::vector<std::filesystem::path> selected;
      for (NSURL* url in panel.URLs) {
        selected.emplace_back(url.fileSystemRepresentation);
      }
      set_selected_files(std::move(selected));
      return true;
    } else {
      NSSavePanel* panel = [NSSavePanel savePanel];
      if (!title().empty()) {
        panel.title = [NSString stringWithUTF8String:title().c_str()];
      }
      if (NSModalResponseOK != [panel runModal]) {
        return false;
      }
      std::vector<std::filesystem::path> selected;
      selected.emplace_back(panel.URL.fileSystemRepresentation);
      set_selected_files(std::move(selected));
      return true;
    }
  }
}

std::unique_ptr<FilePicker> FilePicker::Create() { return std::make_unique<MacFilePicker>(); }

}  // namespace ui
}  // namespace xe
