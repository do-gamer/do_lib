#!/usr/bin/env bash
set -euo pipefail

SCRIPTSRC=`readlink -f "$0" || echo "$0"`
RUN_PATH=`dirname "${SCRIPTSRC}" || echo .`

cd ${RUN_PATH}

BROWSER_DIR="./browser"
BUILD_DIR="./build"
CLIENT_LIB_DIR="$BUILD_DIR/client"
DO_LIB_DIR="$BUILD_DIR/do_lib"

COMPAT_IMAGE="darktanos-build:ubuntu20.04"
# Newest glibc symbol version the libraries may require (Ubuntu 20.04 / Mint 20 / Debian 11)
COMPAT_GLIBC="2.31"

# Command line flags
CLEAN=false
BUILD_BROWSER=false
DOCKER_BUILD=false
IN_CONTAINER=false

# parse arguments (allows -c, -b and -d in any order)
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
        -d)
            DOCKER_BUILD=true
            shift
            ;;
        --in-container)
            IN_CONTAINER=true
            shift
            ;;
        *)
            echo "Usage: $0 [-c] [-b] [-d]"
            echo "  -c: Clean build directory (and browser/dist) before building"
            echo "  -b: Build browser component first"
            echo "  -d: Build the native libraries in Docker (Ubuntu 20.04 toolchain) so they"
            echo "      run on all common distributions (recommended for release builds)"
            exit 1
            ;;
    esac
done

# Configure and compile the native libraries into $BUILD_DIR.
build_native() {
    local toolchain="$1"

    # never mix objects produced by different toolchains (host vs container)
    if [[ -f "$BUILD_DIR/CMakeCache.txt" && "$(cat "$BUILD_DIR/.toolchain" 2>/dev/null)" != "$toolchain" ]]; then
        echo "Toolchain changed, cleaning $BUILD_DIR..."
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    echo "$toolchain" > "$BUILD_DIR/.toolchain"

    cmake -S . -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden" \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -ffunction-sections -fdata-sections -fvisibility=hidden" \
        -DCMAKE_SHARED_LINKER_FLAGS_RELEASE="-Wl,--gc-sections -Wl,--as-needed -Wl,-O1"
    cmake --build "$BUILD_DIR" -j "$(nproc 2>/dev/null || echo 2)"
}

# Inside the container only the native libraries are built (called by -d).
if [[ "$IN_CONTAINER" == "true" ]]; then
    build_native "docker-ubuntu20.04"
    exit 0
fi

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

# Configure and build the native libraries with optimizations
if [[ "$DOCKER_BUILD" == "true" ]]; then
    command -v docker >/dev/null 2>&1 || { echo "❌ docker not found (required for -d)"; exit 1; }
    echo "Building toolchain image $COMPAT_IMAGE..."
    docker build -q -t "$COMPAT_IMAGE" -f docker/linux-compat.Dockerfile docker >/dev/null
    # same absolute path inside the container so CMake caches stay valid; run as the
    # current user so build outputs aren't owned by root
    docker run --rm -u "$(id -u):$(id -g)" -v "$RUN_PATH:$RUN_PATH" -w "$RUN_PATH" \
        "$COMPAT_IMAGE" ./build.sh --in-container
else
    build_native "host"
fi

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

# Report the minimum glibc the libraries need, so incompatible builds are noticed early
for lib in "$CLIENT_LIB_DIR/DarkTanos.so" "$DO_LIB_DIR/libdo_lib.so"; do
    required=$(objdump -T "$lib" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -1)
    if [[ -n "$required" ]] && [[ "$(printf '%s\n%s\n' "$COMPAT_GLIBC" "$required" | sort -V | tail -1)" != "$COMPAT_GLIBC" ]]; then
        echo "⚠️  $lib requires glibc $required: it won't load on older distributions (Ubuntu 20.04 / Mint 20 need <= $COMPAT_GLIBC)."
        echo "   Use ./build.sh -d for a portable build."
    else
        echo "✅ $lib requires glibc ${required:-?} (portable)"
    fi
done

# if a copy script exists, execute it to move artifacts into darkbot/lib
if [[ -x "./copy.sh" ]]; then
    echo "Running copy.sh to transfer build artifacts..."
    ./copy.sh
elif [[ -f "./copy.sh" ]]; then
    echo "copy.sh exists but is not executable; please make it executable to run"
fi

