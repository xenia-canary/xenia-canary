#!/bin/bash
# configure_xcode_gtk.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)" 
XCODE_PROJECT="$PROJECT_ROOT/build/xenia.xcodeproj"
PATCHED_DIR="/tmp/xenia_gtk_patched_permanent"

# macOS only
if [[ "$OSTYPE" != "darwin"* ]]; then
    exit 0
fi

echo "=== Configuring Xcode for GTK ==="

# avoid permissions issue by running the valid shell script with permissions
sh "$SCRIPT_DIR/setup_xcode_gtk.sh"

if [ ! -d "$PATCHED_DIR/gtk-3.0" ]; then
    echo "Error: Failed to create patched headers"
    exit 1
fi

if [ ! -d "$XCODE_PROJECT" ]; then
    echo "Xcode project not found at $XCODE_PROJECT, skipping project patch"
    exit 0
fi

echo "Updating Xcode project with patched GTK paths..."

# ammend potential Xcode project with python if it exists
cat > /tmp/update_xcode_gtk.py << 'EOF'
import os
import sys
import shutil

def update_pbxproj(filepath, patched_dir):
    if not os.path.exists(filepath):
        return False
    
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    replacements = {
        '/opt/homebrew/include/gtk-3.0': f'{patched_dir}/gtk-3.0',
        '/opt/homebrew/include/glib-2.0': f'{patched_dir}/glib-2.0',
        '/opt/homebrew/lib/glib-2.0/include': f'{patched_dir}/glib-2.0/include',
        '/opt/homebrew/include/pango-1.0': f'{patched_dir}/pango-1.0',
        '/opt/homebrew/include/cairo': f'{patched_dir}/cairo',
        '/opt/homebrew/include/atk-1.0': f'{patched_dir}/atk-1.0',
    }
    
    modified = False
    for old_path, new_path in replacements.items():
        if old_path in content:
            content = content.replace(old_path, new_path)
            print(f"Replaced: {old_path} -> {new_path}")
            modified = True
    
    if modified:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Updated: {filepath}")
    return modified

if __name__ == '__main__':
    update_pbxproj(sys.argv[1], sys.argv[2])
EOF

python3 /tmp/update_xcode_gtk.py "$XCODE_PROJECT/project.pbxproj" "$PATCHED_DIR"

# update xcconfig
find "$PROJECT_ROOT/build" -name "*.xcconfig" -type f 2>/dev/null | while read xcconfig; do
    echo "Updating xcconfig: $xcconfig"
    sed -i '' "
        s|/opt/homebrew/include/gtk-3.0|$PATCHED_DIR/gtk-3.0|g;
        s|/opt/homebrew/include/glib-2.0|$PATCHED_DIR/glib-2.0|g;
        s|/opt/homebrew/lib/glib-2.0/include|$PATCHED_DIR/glib-2.0/include|g;
    " "$xcconfig" || true
done

echo "=== Xcode configuration complete ==="