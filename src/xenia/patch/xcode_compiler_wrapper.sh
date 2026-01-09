#!/bin/bash
#xcode_compiler_wrapper.sh

ORIGINAL_CLANG="/usr/bin/clang"
ORIGINAL_CLANGXX="/usr/bin/clang++"
PATCHED_GTK_DIR="/tmp/xenia_gtk_patched_permanent"

# Check whether or not this is a C or C++ compilation
is_cpp_compilation() {
    local next_is_lang=false
    for arg in "$@"; do
        if [ "$next_is_lang" = true ]; then
            if [[ "$arg" == "c++" ]]; then
                return 0
            fi
            next_is_lang=false
            continue
        fi
        if [[ "$arg" == "-x" ]]; then
            next_is_lang=true
            continue
        fi
        if [[ "$arg" == *.cc ]] || [[ "$arg" == *.cpp ]] || [[ "$arg" == *.cxx ]]; then
            return 0
        fi
    done
    return 1
}

# Set the appropriate compiler
if is_cpp_compilation "$@"; then
    COMPILER="$ORIGINAL_CLANGXX"
else
    COMPILER="$ORIGINAL_CLANG"
fi

needs_gtk_filter=false
for arg in "$@"; do
    if [[ "$arg" == *"gtk"* ]] || [[ "$arg" == *"GTK"* ]] || \
       [[ "$arg" == *"windowed_app_main_posix"* ]] || \
       [[ "$arg" == *"/opt/homebrew/include"* ]]; then
        needs_gtk_filter=true
        break
    fi
done

if [ "$#" -eq 0 ]; then
    exit 0
fi

if [ "$needs_gtk_filter" = true ]; then
    echo "=== Xenia GTK Filter Active ==="
    echo "Original command: $COMPILER $*"
    
    if [ ! -d "$PATCHED_GTK_DIR/gtk-3.0" ]; then
        echo "Error: Patched GTK headers not found at $PATCHED_GTK_DIR. Please run setup_xcode_gtk.sh first."
        exit 1
    fi
    # Filter the arguments
    filtered_args=()
    skip_next=false
    for arg in "$@"; do
        if $skip_next; then
            skip_next=false
            if [[ "$arg" == "/opt/homebrew/include/gtk-3.0" ]] || \
               [[ "$arg" == "/opt/homebrew/include/glib-2.0" ]] || \
               [[ "$arg" == "/opt/homebrew/lib/glib-2.0/include" ]]; then
                if [[ "$arg" == "/opt/homebrew/include/gtk-3.0" ]]; then
                    filtered_args+=("$PATCHED_GTK_DIR/gtk-3.0")
                elif [[ "$arg" == "/opt/homebrew/include/glib-2.0" ]]; then
                    filtered_args+=("$PATCHED_GTK_DIR/glib-2.0")
                elif [[ "$arg" == "/opt/homebrew/lib/glib-2.0/include" ]]; then
                    filtered_args+=("$PATCHED_GTK_DIR/glib-2.0/include")
                fi
                continue
            fi
            filtered_args+=("$arg")
            continue
        fi
        
        case "$arg" in
            -iquote)
                filtered_args+=("$arg")
                skip_next=true
                ;;
            -I)
                filtered_args+=("$arg")
                skip_next=true
                ;;
            -iquote/opt/homebrew/include/gtk-3.0*)
                filtered_args+=("-iquote$PATCHED_GTK_DIR/gtk-3.0")
                ;;
            -iquote/opt/homebrew/include/glib-2.0*)
                filtered_args+=("-iquote$PATCHED_GTK_DIR/glib-2.0")
                ;;
            -iquote/opt/homebrew/lib/glib-2.0/include*)
                filtered_args+=("-iquote$PATCHED_GTK_DIR/glib-2.0/include")
                ;;
            -I/opt/homebrew/include/gtk-3.0*)
                filtered_args+=("-I$PATCHED_GTK_DIR/gtk-3.0")
                ;;
            -I/opt/homebrew/include/glib-2.0*)
                filtered_args+=("-I$PATCHED_GTK_DIR/glib-2.0")
                ;;
            -I/opt/homebrew/lib/glib-2.0/include*)
                filtered_args+=("-I$PATCHED_GTK_DIR/glib-2.0/include")
                ;;
            *)
                filtered_args+=("$arg")
                ;;
        esac
    done
    
    filtered_args+=("-I$PATCHED_GTK_DIR/gtk-3.0")
    filtered_args+=("-I$PATCHED_GTK_DIR/glib-2.0")
    filtered_args+=("-I$PATCHED_GTK_DIR/glib-2.0/include")
    
    echo "Filtered command: $COMPILER ${filtered_args[*]}"
    exec "$COMPILER" "${filtered_args[@]}"
else
    exec "$COMPILER" "$@"
fi