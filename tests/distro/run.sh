#!/usr/bin/env bash
# Checks that the built libraries load on common distributions (glibc/X11 ABI).
#   tests/distro/run.sh [image...]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGES=("$@")
if [[ ${#IMAGES[@]} -eq 0 ]]; then
    IMAGES=(ubuntu:20.04 ubuntu:22.04 ubuntu:24.04 linuxmintd/mint21.3-amd64 linuxmintd/mint22-amd64
            debian:11 debian:12 fedora:40 archlinux:latest)
fi

WORK="$(mktemp -d /tmp/tanos_distro.XXXX)"
LIBS="${LIBS:-$ROOT/build}"
cp "$LIBS"/client/DarkTanos.so "$LIBS"/do_lib/libdo_lib.so "$WORK/" 2>/dev/null \
    || cp "$LIBS"/DarkTanos.so "$LIBS"/libdo_lib.so "$WORK/"
# build the loader with the old toolchain so it runs on every target
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:$ROOT:ro" -v "$WORK:$WORK" darktanos-build:ubuntu20.04 \
    gcc -O2 -o "$WORK/loader" "$ROOT/tests/distro/loader.c" -ldl

failed=0
for image in "${IMAGES[@]}"; do
    install='true'
    case "$image" in
        ubuntu*|debian*|linuxmint*) install='apt-get update -qq >/dev/null && apt-get install -y -qq libx11-6 libxext6 >/dev/null' ;;
        fedora*) install='dnf install -y -q libX11 libXext >/dev/null' ;;
        archlinux*) install='pacman -Sy --noconfirm --needed libx11 libxext >/dev/null' ;;
    esac
    docker pull -q "$image" >/dev/null 2>&1
    out=$(docker run --rm -v "$WORK:$WORK:ro" "$image" bash -c "
        $install
        glibc=\$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')
        echo \"glibc \$glibc\"
        $WORK/loader $WORK/DarkTanos.so && LD_PRELOAD=$WORK/libdo_lib.so $WORK/loader $WORK/DarkTanos.so | tail -1
    " 2>&1)
    status=$?
    printf '%-28s %s\n' "$image" "$(echo "$out" | grep -E '^(glibc|OK|FAIL)' | tr '\n' ' ' | sed 's/  */ /g')"
    [[ $status -ne 0 || "$out" == *FAIL* ]] && failed=1
done
rm -rf "$WORK"
exit $failed
