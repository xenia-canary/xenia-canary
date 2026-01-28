#!/bin/bash
#clang_gtk_wrapper.sh

PATCHED_DIR="/tmp/xenia_gtk_patched_permanent"
ORIGINAL_COMPILER="/usr/bin/clang"

ensure_patched_headers() {
    if [ ! -d "$PATCHED_DIR/gtk-3.0" ]; then
        echo "Warning: Patched GTK headers not found at $PATCHED_DIR"
    fi
}

# Overide homebrew's GTK headers if detected
has_gtk_includes=false
for arg in "$@"; do
    if [[ "$arg" == *"gtk"* ]] || [[ "$arg" == *"GTK"* ]] || \
       [[ "$arg" == *"/opt/homebrew/include"* ]]; then
        has_gtk_includes=true
        break
    fi
done

if [ "$#" -eq 0 ]; then
    exit 0
fi

if [ "$has_gtk_includes" = false ]; then
    exec "$ORIGINAL_COMPILER" "$@"
fi

ensure_patched_headers

new_args=()
skip_next=false

# patch GTK include paths
for arg in "$@"; do
    if $skip_next; then
        skip_next=false
        if [[ "$arg" == "/opt/homebrew/include/gtk-3.0" ]] || \
           [[ "$arg" == "/opt/homebrew/include/glib-2.0" ]] || \
           [[ "$arg" == "/opt/homebrew/lib/glib-2.0/include" ]]; then
            if [[ "$arg" == "/opt/homebrew/include/gtk-3.0" ]]; then
                new_args+=("$PATCHED_DIR/gtk-3.0")
            elif [[ "$arg" == "/opt/homebrew/include/glib-2.0" ]]; then
                new_args+=("$PATCHED_DIR/glib-2.0")
            elif [[ "$arg" == "/opt/homebrew/lib/glib-2.0/include" ]]; then
                new_args+=("$PATCHED_DIR/glib-2.0/include")
            fi
            continue
        fi
        new_args+=("$arg")
        continue
    fi
    
    case "$arg" in
        -I|-iquote)
            new_args+=("$arg")
            skip_next=true
            ;;
        -I/opt/homebrew/include/gtk-3.0*)
            new_args+=("-I$PATCHED_DIR/gtk-3.0")
            ;;
        -I/opt/homebrew/include/glib-2.0*)
            new_args+=("-I$PATCHED_DIR/glib-2.0")
            ;;
        -I/opt/homebrew/lib/glib-2.0/include*)
            new_args+=("-I$PATCHED_DIR/glib-2.0/include")
            ;;
        -iquote/opt/homebrew/include/gtk-3.0*)
            new_args+=("-iquote$PATCHED_DIR/gtk-3.0")
            ;;
        -iquote/opt/homebrew/include/glib-2.0*)
            new_args+=("-iquote$PATCHED_DIR/glib-2.0")
            ;;
        -iquote/opt/homebrew/lib/glib-2.0/include*)
            new_args+=("-iquote$PATCHED_DIR/glib-2.0/include")
            ;;
        *)
            new_args+=("$arg")
            ;;
    esac
done

# Make sure we reference the patched headers instead
new_args+=("-I$PATCHED_DIR/gtk-3.0")
new_args+=("-I$PATCHED_DIR/glib-2.0")
new_args+=("-I$PATCHED_DIR/glib-2.0/include")

echo "Using patched GTK headers from: $PATCHED_DIR"
exec "$ORIGINAL_COMPILER" "${new_args[@]}"