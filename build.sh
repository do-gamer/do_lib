#!/usr/bin/env bash
set -euo pipefail

SCRIPTSRC=`readlink -f "$0" || echo "$0"`
RUN_PATH=`dirname "${SCRIPTSRC}" || echo .`

cd ${RUN_PATH}

BUILD_DIR="./build"

# Clean build directory if --clean or -c flag is provided
if [[ "${1:-}" == "--clean" || "${1:-}" == "-c" ]]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
elif [[ -n "${1:-}" ]]; then
    echo "Usage: $0 [--clean|-c]"
    echo "  --clean, -c: Clean build directory before building"
    exit 1
fi

cmake -S . -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
	-DCMAKE_CXX_FLAGS_RELEASE="-Os -ffunction-sections -fdata-sections -fvisibility=hidden" \
	-DCMAKE_SHARED_LINKER_FLAGS_RELEASE="-Wl,--gc-sections"
cmake --build "$BUILD_DIR"

CLIENT_LIB_DIR="$BUILD_DIR/client"
DO_LIB_DIR="$BUILD_DIR/do_lib"

if [[ -f "$CLIENT_LIB_DIR/libDarkTanos.so" ]]; then
	mv "$CLIENT_LIB_DIR/libDarkTanos.so" "$CLIENT_LIB_DIR/DarkTanos.so"
fi

if ! command -v strip >/dev/null 2>&1; then
	echo "strip not found; skipping symbol stripping." >&2
	exit 0
fi

strip --strip-unneeded "$CLIENT_LIB_DIR/DarkTanos.so"
strip --strip-unneeded "$DO_LIB_DIR/libdo_lib.so"