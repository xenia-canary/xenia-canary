/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/file_picker.h"

#import <Cocoa/Cocoa.h>
#import <dispatch/dispatch.h>

namespace xe {
namespace ui {

class MacFilePicker : public FilePicker {
 public:
  MacFilePicker() = default;
  ~MacFilePicker() override = default;

  bool Show(Window* parent_window) override {
    if (![NSThread isMainThread]) {
      __block bool result = false;
      dispatch_sync(dispatch_get_main_queue(), ^{
        result = ShowInternal(parent_window);
      });
      return result;
    }

    return ShowInternal(parent_window);
  }

 private:
  bool ShowInternal(Window* parent_window) {
    (void)parent_window;
    @autoreleasepool {
      set_selected_files({});

      if (mode() == Mode::kSave) {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setTitle:[NSString stringWithUTF8String:title().c_str()]];

        if (type() == Type::kDirectory) {
          [panel setCanCreateDirectories:YES];
        }

        const NSInteger result = [panel runModal];
        if (result != NSModalResponseOK) {
          return false;
        }

        NSURL* url = [panel URL];
        if (!url) {
          return false;
        }

        std::vector<std::filesystem::path> selected;
        selected.emplace_back(std::filesystem::path([[url path] UTF8String]));
        set_selected_files(std::move(selected));
        return true;
      }

      // Open.
      NSOpenPanel* panel = [NSOpenPanel openPanel];
      [panel setTitle:[NSString stringWithUTF8String:title().c_str()]];

      if (type() == Type::kDirectory) {
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
      } else {
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
      }

      [panel setAllowsMultipleSelection:multi_selection() ? YES : NO];

      const NSInteger result = [panel runModal];
      if (result != NSModalResponseOK) {
        return false;
      }

      NSArray<NSURL*>* urls = [panel URLs];
      if (!urls || [urls count] == 0) {
        return false;
      }

      std::vector<std::filesystem::path> selected;
      selected.reserve([urls count]);
      for (NSURL* url in urls) {
        if (!url) {
          continue;
        }
        selected.emplace_back(std::filesystem::path([[url path] UTF8String]));
      }

      set_selected_files(std::move(selected));
      return !selected_files().empty();
    }
  }
};

std::unique_ptr<FilePicker> FilePicker::Create() { return std::make_unique<MacFilePicker>(); }

}  // namespace ui
}  // namespace xe
