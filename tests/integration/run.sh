#!/usr/bin/env bash
# End-to-end test: real JVM + DarkTanos.so + AppImage browser + pepper flash under Xvfb.
#
#   tests/integration/run.sh [LIB_DIR] [fuse|nofuse]
#
# LIB_DIR must contain DarkTanos.so, libdo_lib.so and darkbot_browser_linux.AppImage
# (default: stage them from build/ and browser/dist/).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LIBDIR="${1:-}"
MODE="${2:-nofuse}"

if [[ -z "$LIBDIR" ]]; then
    LIBDIR="$(mktemp -d /tmp/tanos_libs.XXXX)"
    cp "$ROOT/build/client/DarkTanos.so" "$ROOT/build/do_lib/libdo_lib.so" \
       "$ROOT/browser/dist/darkbot_browser_linux.AppImage" "$LIBDIR/"
fi
LIBDIR="$(cd "$LIBDIR" && pwd)"

docker build -q -t darktanos-it:ubuntu22.04 "$ROOT/tests/integration" >/dev/null

EXTRA=()
if [[ -n "${TEST_SWF:-}" ]]; then
    TEST_SWF="$(readlink -f "$TEST_SWF")"
    EXTRA+=(-v "$(dirname "$TEST_SWF"):$(dirname "$TEST_SWF"):ro")
fi
if [[ "$MODE" == "fuse" ]]; then
    EXTRA+=(--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined)
fi

# run as a regular user that exists in /etc/passwd (fusermount needs a user name)
docker run --rm --shm-size=1g "${EXTRA[@]}" \
    -v "$ROOT:$ROOT:ro" -v "$LIBDIR:$LIBDIR:ro" -w "$ROOT" \
    darktanos-it:ubuntu22.04 bash -c "
        useradd -m -u $(id -u) tester 2>/dev/null || true
        su tester -c 'TEST_SWF=\"${TEST_SWF:-}\" SOAK_SECONDS=\"${SOAK_SECONDS:-}\" STRESS_SECONDS=\"${STRESS_SECONDS:-}\" CLICK_TEST=\"${CLICK_TEST:-}\" TEST_TIMEOUT=\"${TEST_TIMEOUT:-}\" JAVA_PRELOAD=\"${JAVA_PRELOAD:-}\" ASAN_OPTIONS=\"${ASAN_OPTIONS:-}\" UBSAN_OPTIONS=\"${UBSAN_OPTIONS:-}\" JAVA_OPTS=\"${JAVA_OPTS:-}\" tests/integration/run_in_container.sh \"$LIBDIR\" \"$LIBDIR/darkbot_browser_linux.AppImage\" \"$MODE\"'
    "
