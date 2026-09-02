/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <Cocoa/Cocoa.h>
#include "xenia/ui/file_picker.h"

namespace xe {
namespace ui {

class MacFilePicker : public FilePicker {
 public:
  MacFilePicker() = default;
  ~MacFilePicker() override = default;

  bool Show(Window* parent_window = nullptr) override {
    @autoreleasepool {
      std::vector<std::filesystem::path> files;
      if (mode() == Mode::kOpen) {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title().c_str()]];
        [panel setAllowsMultipleSelection:multi_selection() ? YES : NO];
        [panel setCanChooseFiles:(type() == Type::kFile) ? YES : NO];
        [panel setCanChooseDirectories:(type() == Type::kDirectory) ? YES : NO];

        if ([panel runModal] == NSModalResponseOK) {
          for (NSURL* url in [panel URLs]) {
            files.push_back(std::filesystem::path([url.path UTF8String]));
          }
          set_selected_files(std::move(files));
          return !selected_files().empty();
        }
      } else {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setTitle:[NSString stringWithUTF8String:title().c_str()]];
        if (!file_name().empty()) {
          [panel setNameFieldStringValue:[NSString stringWithUTF8String:file_name().c_str()]];
        }
        if ([panel runModal] == NSModalResponseOK) {
          files.push_back(std::filesystem::path([panel.URL.path UTF8String]));
          set_selected_files(std::move(files));
          return true;
        }
      }
      return false;
    }
  }
};

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<MacFilePicker>();
}

}  // namespace ui
}  // namespace xe
