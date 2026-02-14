#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
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