/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/file_picker.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>
#include <string_view>

#include "xenia/base/string.h"

namespace xe {
namespace ui {

class MacFilePicker : public FilePicker {
 public:
  bool Show(Window* parent_window) override;
};

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<MacFilePicker>();
}

namespace {

// Converts Windows-style filters ("*.iso;*.xex") to content types. Returns an
// empty array if any extension, such as "*.*", allows any file.
NSArray<UTType*>* GetAllowedContentTypes(
    const std::vector<std::pair<std::string, std::string>>& extensions) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (const auto& extension_pair : extensions) {
    std::string_view patterns = extension_pair.second;
    while (!patterns.empty()) {
      size_t separator = patterns.find(';');
      std::string_view pattern = patterns.substr(0, separator);
      patterns = separator == std::string_view::npos
                     ? std::string_view()
                     : patterns.substr(separator + 1);
      if (pattern.starts_with("*.")) {
        pattern.remove_prefix(2);
      }
      if (pattern.empty() || pattern == "*") {
        return @[];
      }
      NSString* extension =
          [[NSString alloc] initWithBytes:pattern.data()
                                   length:pattern.size()
                                 encoding:NSUTF8StringEncoding];
      UTType* type =
          extension ? [UTType typeWithFilenameExtension:extension] : nil;
      if (type) {
        [types addObject:type];
      }
    }
  }
  return types;
}

}  // namespace

bool MacFilePicker::Show(Window* parent_window) {
  @autoreleasepool {
    NSSavePanel* panel;
    if (mode() == Mode::kSave) {
      NSSavePanel* save_panel = [NSSavePanel savePanel];
      if (!file_name().empty()) {
        save_panel.nameFieldStringValue =
            [NSString stringWithUTF8String:file_name().c_str()];
      }
      panel = save_panel;
    } else {
      NSOpenPanel* open_panel = [NSOpenPanel openPanel];
      bool directory = type() == Type::kDirectory;
      open_panel.canChooseFiles = !directory;
      open_panel.canChooseDirectories = directory;
      open_panel.allowsMultipleSelection = multi_selection();
      panel = open_panel;
    }
    panel.message = [NSString stringWithUTF8String:title().c_str()];
    if (type() == Type::kFile) {
      NSArray<UTType*>* content_types = GetAllowedContentTypes(extensions());
      if (content_types.count) {
        panel.allowedContentTypes = content_types;
      }
    }

    // Modal to the application - the caller expects a synchronous result.
    if ([panel runModal] != NSModalResponseOK) {
      return false;
    }

    std::vector<std::filesystem::path> selected_files;
    if (mode() == Mode::kSave) {
      if (panel.URL) {
        selected_files.emplace_back(panel.URL.fileSystemRepresentation);
      }
    } else {
      for (NSURL* url in ((NSOpenPanel*)panel).URLs) {
        selected_files.emplace_back(url.fileSystemRepresentation);
      }
    }
    if (selected_files.empty()) {
      return false;
    }
    set_selected_files(selected_files);
    return true;
  }
}

}  // namespace ui
}  // namespace xe
