/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/file_picker.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <string>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"

namespace xe {
namespace ui {

class MacFilePicker : public FilePicker {
 public:
  MacFilePicker();
  ~MacFilePicker() override;

  bool Show(Window* parent_window) override;

 private:
};

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<MacFilePicker>();
}

MacFilePicker::MacFilePicker() = default;

MacFilePicker::~MacFilePicker() = default;

bool MacFilePicker::Show(Window* parent_window) {
  @autoreleasepool {
    NSSavePanel* panel = nil;
    
    switch (mode()) {
      case Mode::kOpen: {
        NSOpenPanel* open_panel = [NSOpenPanel openPanel];
        [open_panel setCanChooseFiles:(type() == Type::kFile)];
        [open_panel setCanChooseDirectories:(type() == Type::kDirectory)];
        [open_panel setAllowsMultipleSelection:multi_selection()];
        panel = open_panel;
        break;
      }
      case Mode::kSave: {
        panel = [NSSavePanel savePanel];
        break;
      }
      default:
        XELOGE("MacFilePicker::Show: Unhandled mode: {}, Type: {}",
               static_cast<int>(mode()), static_cast<int>(type()));
        assert_always();
        return false;
    }

    // Set title
    if (!title().empty()) {
      [panel setTitle:[NSString stringWithUTF8String:title().c_str()]];
    }

    // Set default filename for save dialogs
    if (mode() == Mode::kSave && !file_name().empty()) {
      [panel setNameFieldStringValue:[NSString stringWithUTF8String:file_name().c_str()]];
    }

    // Set allowed file types (extensions)
    if (!extensions().empty() && type() == Type::kFile) {
      NSMutableArray* allowed_types = [NSMutableArray array];
      for (const auto& ext_pair : extensions()) {
        // ext_pair.second contains the pattern like "*.iso" or "*.xex" 
        std::string pattern = ext_pair.second;
        
        // Parse patterns like "*.iso;*.xex;*.zar" 
        size_t pos = 0;
        while (pos != std::string::npos) {
          size_t next_pos = pattern.find(';', pos);
          std::string single_pattern;
          if (next_pos == std::string::npos) {
            single_pattern = pattern.substr(pos);
            pos = std::string::npos;
          } else {
            single_pattern = pattern.substr(pos, next_pos - pos);
            pos = next_pos + 1;
          }
          
          // Remove "*." prefix if present
          if (single_pattern.find("*.") == 0) {
            single_pattern = single_pattern.substr(2);
          }
          
          // Skip "*.*" wildcard
          if (single_pattern == "*") {
            continue;
          }
          
          // Add to allowed types
          if (!single_pattern.empty()) {
            [allowed_types addObject:[NSString stringWithUTF8String:single_pattern.c_str()]];
          }
        }
      }
      
      if ([allowed_types count] > 0) {
        [panel setAllowedFileTypes:allowed_types];
      }
    }

    // Run the panel modally
    NSModalResponse response = [panel runModal];
    
    if (response == NSModalResponseOK) {
      std::vector<std::filesystem::path> selected_files;
      
      if (mode() == Mode::kOpen && multi_selection()) {
        // Multiple files selected
        NSOpenPanel* open_panel = (NSOpenPanel*)panel;
        NSArray* urls = [open_panel URLs];
        for (NSURL* url in urls) {
          std::string path_str = [[url path] UTF8String];
          selected_files.push_back(xe::to_path(path_str));
        }
      } else {
        // Single file or directory selected
        NSURL* url = [panel URL];
        if (url) {
          std::string path_str = [[url path] UTF8String];
          selected_files.push_back(xe::to_path(path_str));
        }
      }
      
      set_selected_files(selected_files);
      return true;
    }
    
    return false;
  }
}

}  // namespace ui
}  // namespace xe
