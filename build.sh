#!/usr/bin/env bash
set -euo pipefail

SCRIPTSRC=`readlink -f "$0" || echo "$0"`
RUN_PATH=`dirname "${SCRIPTSRC}" || echo .`

cd ${RUN_PATH}

BUILD_DIR="./build"

# Command line flags
CLEAN=false
BUILD_BROWSER=false

# parse arguments (allows -c and -b in any order)
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c)
            CLEAN=true
            shift
            ;;
        -b)
            BUILD_BROWSER=true
            shift
            ;;
        *)
            echo "Usage: $0 [-c] [-b]"
            echo "  -c: Clean build directory (and browser/dist) before building"
            echo "  -b: Build browser component first by invoking browser/build.sh"
            exit 1
            ;;
    esac
done

# perform clean if requested
if [[ "$CLEAN" == "true" ]]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    # also clean browser output if present
    if [[ -d "browser/dist" ]]; then
        echo "Cleaning browser/dist..."
        rm -rf "browser/dist"
    fi
fi

# If requested, build the browser component first
if [[ "$BUILD_BROWSER" == "true" ]]; then
    echo "Building browser component..."
    if [[ -x "browser/build.sh" ]]; then
        ./browser/build.sh
    else
        echo "Browser build script not found or not executable" >&2
        exit 1
    fi
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

if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$CLIENT_LIB_DIR/DarkTanos.so"
    strip --strip-unneeded "$DO_LIB_DIR/libdo_lib.so"
else
    echo "strip not found; skipping symbol stripping." >&2
fi

# if a copy script exists, execute it to move artifacts into darkbot/lib
if [[ -x "./copy.sh" ]]; then
    echo "Running copy.sh to transfer build artifacts..."
    ./copy.sh
elif [[ -f "./copy.sh" ]]; then
    echo "copy.sh exists but is not executable; please make it executable to run"
fi

