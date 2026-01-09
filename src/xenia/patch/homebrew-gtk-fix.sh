#!/bin/bash
# homebrew-gtk-fix.sh

set -e
cleanup() {
    echo "===STATUS: Cleaning up temporary symlinks...==="
    rm -f "$TEMP_INCLUDE_DIR/gtk"
    rm -f "$TEMP_INCLUDE_DIR/gdk"
    rm -f "$TEMP_INCLUDE_DIR/glib"
    rm -f "$TEMP_INCLUDE_DIR/gio"
    rm -f "$TEMP_INCLUDE_DIR/cairo"
    rm -f "$TEMP_INCLUDE_DIR/pango"
    rmdir "$TEMP_INCLUDE_DIR" 2>/dev/null || true
    echo "===STATUS: Cleanup complete==="
}

trap cleanup EXIT

if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "===STATUS: Not macOS, running build directly...==="
    exec "$@"
    exit $?
fi

TEMP_INCLUDE_DIR="/tmp/xenia_gtk_$$"
mkdir -p "$TEMP_INCLUDE_DIR"

echo "===STATUS: New symlinks in $TEMP_INCLUDE_DIR...==="
ln -sf /opt/homebrew/include/gtk-3.0/gtk "$TEMP_INCLUDE_DIR/gtk"
ln -sf /opt/homebrew/include/gtk-3.0/gdk "$TEMP_INCLUDE_DIR/gdk"
ln -sf /opt/homebrew/include/glib-2.0/glib "$TEMP_INCLUDE_DIR/glib"
ln -sf /opt/homebrew/include/glib-2.0/gio "$TEMP_INCLUDE_DIR/gio"
ln -sf /opt/homebrew/include/gtk-3.0/cairo "$TEMP_INCLUDE_DIR/cairo" 2>/dev/null || true
ln -sf /opt/homebrew/include/gtk-3.0/pango "$TEMP_INCLUDE_DIR/pango" 2>/dev/null || true

if [ ! -L "$TEMP_INCLUDE_DIR/gtk" ]; then
    echo "===ERROR: Failed to create GTK symlinks.==="
    exit 1
fi

echo "Symlinks created:"
ls -la "$TEMP_INCLUDE_DIR/"

export CPLUS_INCLUDE_PATH="$TEMP_INCLUDE_DIR:$CPLUS_INCLUDE_PATH"
export C_INCLUDE_PATH="$TEMP_INCLUDE_DIR:$C_INCLUDE_PATH"

echo "===STATUS: GTK paths ammended temporarily==="
echo "Command: $@"

exec "$@"