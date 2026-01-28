#!/bin/bash
# setup_xcode_gtk.sh

set -e

PATCHED_DIR="/tmp/xenia_gtk_patched_permanent"
GTK_SOURCE_DIR="/opt/homebrew/include"

echo "=== Setting up Xcode for GTK ==="

if [ -d "$GTK_SOURCE_DIR/gtk-3.0" ]; then
    echo "Creating permanent patched GTK headers..."
    
    mkdir -p "$PATCHED_DIR"
    rm -rf "$PATCHED_DIR"/*
    
    find "/opt/homebrew/include/gtk-3.0/" \
         "/opt/homebrew/include/glib-2.0/" \
         "/opt/homebrew/include/pango-1.0/" \
         "/opt/homebrew/include/cairo/" \
         "/opt/homebrew/include/atk-1.0/" \
         "/opt/homebrew/include/gdk-pixbuf-2.0/" \
         "/opt/homebrew/include/harfbuzz/" \
        -type f -name "*.h" 2>/dev/null | while read header; do
        
        if [[ "$header" == "/opt/homebrew/include/gtk-3.0/"* ]]; then
            rel="${header#/opt/homebrew/include/gtk-3.0/}"
            dest="$PATCHED_DIR/gtk-3.0/$rel"
        elif [[ "$header" == "/opt/homebrew/include/glib-2.0/"* ]]; then
            rel="${header#/opt/homebrew/include/glib-2.0/}"
            dest="$PATCHED_DIR/glib-2.0/$rel"
        elif [[ "$header" == "/opt/homebrew/include/pango-1.0/"* ]]; then
            rel="${header#/opt/homebrew/include/pango-1.0/}"
            dest="$PATCHED_DIR/pango-1.0/$rel"
        elif [[ "$header" == "/opt/homebrew/include/cairo/"* ]]; then
            rel="${header#/opt/homebrew/include/cairo/}"
            dest="$PATCHED_DIR/cairo/$rel"
        elif [[ "$header" == "/opt/homebrew/include/atk-1.0/"* ]]; then
            rel="${header#/opt/homebrew/include/atk-1.0/}"
            dest="$PATCHED_DIR/atk-1.0/$rel"
        elif [[ "$header" == "/opt/homebrew/include/gdk-pixbuf-2.0/"* ]]; then
            rel="${header#/opt/homebrew/include/gdk-pixbuf-2.0/}"
            dest="$PATCHED_DIR/gdk-pixbuf-2.0/$rel"
        elif [[ "$header" == "/opt/homebrew/include/harfbuzz/"* ]]; then
            rel="${header#/opt/homebrew/include/harfbuzz/}"
            dest="$PATCHED_DIR/harfbuzz/$rel"
        else
            continue
        fi
        
        mkdir -p "$(dirname "$dest")"
        
        sed '
            s|#include <\(gtk/[^>]*\)>|#include "\1"|g;
            s|#include <\(gdk/[^>]*\)>|#include "\1"|g;
            s|#include <\(glib/[^>]*\)>|#include "\1"|g;
            s|#include <\(gio/[^>]*\)>|#include "\1"|g;
            s|#include <\(gobject/[^>]*\)>|#include "\1"|g;
            s|#include <\(gmodule\.h\)>|#include "\1"|g;
            s|#include <\(glib\.h\)>|#include "\1"|g;
            s|#include <\(glibconfig\.h\)>|#include "\1"|g;
            s|#include <\(pango/[^>]*\)>|#include "\1"|g;
            s|#include <\(cairo/[^>]*\)>|#include "\1"|g;
            s|#include <\(atk/[^>]*\)>|#include "\1"|g;
            s|#include <\(harfbuzz/[^>]*\)>|#include "\1"|g;
        ' "$header" > "$dest"
    done
    
    # Fixing glibconfig.h under our patched glib
    if [ -f "/opt/homebrew/lib/glib-2.0/include/glibconfig.h" ]; then
        mkdir -p "$PATCHED_DIR/glib-2.0/include"
        sed 's|#include <\(glib/gmacros\.h\)>|#include "\1"|g' \
            "/opt/homebrew/lib/glib-2.0/include/glibconfig.h" > \
            "$PATCHED_DIR/glib-2.0/include/glibconfig.h"
    fi
    
    echo "Patched headers created at: $PATCHED_DIR"
    
    # Generate temp xcconfig file
    cat > "$PATCHED_DIR/gtk.xcconfig" << EOF
// Xcode configuration for patched GTK headers
HEADER_SEARCH_PATHS = \$(inherited) "$PATCHED_DIR/gtk-3.0" "$PATCHED_DIR/glib-2.0" "$PATCHED_DIR/glib-2.0/include"
USER_HEADER_SEARCH_PATHS = \$(inherited) "$PATCHED_DIR/gtk-3.0" "$PATCHED_DIR/glib-2.0" "$PATCHED_DIR/glib-2.0/include"
GCC_PREPROCESSOR_DEFINITIONS = \$(inherited) USE_PATCHED_GTK=1
EOF
    
else
    echo "GTK not found at $GTK_SOURCE_DIR"
    exit 1
fi