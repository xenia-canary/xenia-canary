#!/bin/bash
#xcode_build_wrapper.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== Xcode Build Wrapper ==="

sh "$SCRIPT_DIR/configure_xcode_gtk.sh"

if [ "$#" -eq 0 ]; then
    echo "No arguments passed to xcodebuild, exiting wrapper."
    exit 0
fi

echo "Running xcodebuild..."
exec xcodebuild "$@"