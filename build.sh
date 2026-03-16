#!/usr/bin/env bash
set -euo pipefail

SCRIPTSRC=`readlink -f "$0" || echo "$0"`
RUN_PATH=`dirname "${SCRIPTSRC}" || echo .`

cd ${RUN_PATH}

BROWSER_DIR="./browser"
BUILD_DIR="./build"
CLIENT_LIB_DIR="$BUILD_DIR/client"
DO_LIB_DIR="$BUILD_DIR/do_lib"

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

# Perform clean if requested
if [[ "$CLEAN" == "true" ]]; then
    echo "Cleaning $BUILD_DIR directory..."
    rm -rf "$BUILD_DIR"
    # also clean browser output if building browser
    if [[ "$BUILD_BROWSER" == "true" ]]; then
        echo "Cleaning $BROWSER_DIR/dist directory..."
        rm -rf "$BROWSER_DIR/dist"
    fi
fi

# If requested, build the browser component first
if [[ "$BUILD_BROWSER" == "true" ]]; then
    cd ${BROWSER_DIR}
    echo "Building browser component..."

    # Load nvm (required)
    export NVM_DIR="${NVM_DIR:-$HOME/.nvm}"
    # This loads nvm (if installed via standard install script)
    if [ -s "$NVM_DIR/nvm.sh" ]; then
        # shellcheck disable=SC1090
        . "$NVM_DIR/nvm.sh"
    else
        echo "❌ nvm not found. Please install NVM first: https://github.com/nvm-sh/nvm"
        return 1 2>/dev/null || exit 1
    fi

    # Use or install Node.js version from .nvmrc
    nvm use || nvm install

    echo "Installing dependencies."
    npm install

    echo "Building browser app."
    npm run dist -- --linux

    echo "Browser build completed."
    cd ${RUN_PATH}
fi

# Configure and build the main project with optimizations
cmake -S . -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
	-DCMAKE_CXX_FLAGS_RELEASE="-Os -ffunction-sections -fdata-sections -fvisibility=hidden" \
	-DCMAKE_SHARED_LINKER_FLAGS_RELEASE="-Wl,--gc-sections"
cmake --build "$BUILD_DIR"

# Rename the client library to match what darkbot expects
if [[ -f "$CLIENT_LIB_DIR/libDarkTanos.so" ]]; then
	mv "$CLIENT_LIB_DIR/libDarkTanos.so" "$CLIENT_LIB_DIR/DarkTanos.so"
fi

# Strip unneeded symbols from the shared libraries to reduce size
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

