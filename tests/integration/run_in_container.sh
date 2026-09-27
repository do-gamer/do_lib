#!/usr/bin/env bash
# Runs one integration scenario inside the container.
#   $1 = directory with DarkTanos.so + libdo_lib.so, $2 = AppImage, $3 = label
set -u
LIBDIR="$1"; APPIMAGE="$2"; LABEL="$3"
SRC="$(cd "$(dirname "$0")" && pwd)"

# browser processes: the AppImage runtime and everything started from its mount/extraction dir
count_browser() {
    ps -eo args= | grep -cE '^(lib/darkbot_browser_linux\.AppImage|/tmp/(appimage_extracted_|\.mount_darkbo)[^ ]*)'
}
# a space in the install path must not break LD_PRELOAD
WORK="$(mktemp -d "/tmp/tanos it.XXXX")"
export HOME="$WORK/home"; mkdir -p "$HOME" "$WORK/lib" "$WORK/classes"

cp "$LIBDIR/libdo_lib.so" "$WORK/lib/"
cp "$APPIMAGE" "$WORK/lib/darkbot_browser_linux.AppImage"
if [[ -n "${TEST_SWF:-}" ]]; then
    cp "$TEST_SWF" "$WORK/test.swf" # e.g. the real game client
else
    python3 "$SRC/make_swf.py" "$WORK/test.swf" >/dev/null
fi
python3 "$SRC/server.py" "$WORK" 8000 &
SERVER=$!

Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:99
sleep 1
openbox >/dev/null 2>&1 &
WM=$!
sleep 1

javac -d "$WORK/classes" "$SRC"/java/eu/darkbot/api/DarkTanos.java "$SRC"/java/TanosIT.java || exit 2

echo "### $LABEL"
cd "$WORK"
shm_before=$(ipcs -m | grep -c '^0x')
sem_before=$(ipcs -s | grep -c '^0x')
TANOS_DEBUG=1 timeout ${TEST_TIMEOUT:-900} ${JAVA_PRELOAD:+env LD_PRELOAD=$JAVA_PRELOAD} java ${JAVA_OPTS:-} -Dtanos.lib="$LIBDIR/DarkTanos.so" -cp "$WORK/classes" TanosIT http://127.0.0.1:8000/ "$WORK/test.swf" ${SOAK_SECONDS:+--soak $SOAK_SECONDS}${STRESS_SECONDS:+--stress $STRESS_SECONDS}
status=$?
sleep 2

echo "RESULT leftover_browser_processes_after_jvm_exit $(count_browser)"
pkill -9 -f '^(lib/darkbot_browser_linux|/tmp/(appimage_extracted_|\.mount_darkbo))' 2>/dev/null; sleep 1

# bot killed with SIGKILL: no destructors run, the browser must notice and exit by itself
java -Dtanos.lib="$LIBDIR/DarkTanos.so" -cp "$WORK/classes" TanosIT http://127.0.0.1:8000/ "$WORK/test.swf" --launch-and-wait > "$WORK/launch.log" 2>&1 &
JPID=$!
for i in $(seq 1 90); do grep -q launch_only_valid_ms "$WORK/launch.log" && break; sleep 1; done
grep RESULT "$WORK/launch.log"
echo "RESULT browser_processes_while_running $(count_browser)"
kill -9 $JPID; sleep 5
echo "RESULT leftover_browser_processes_after_jvm_kill9 $(count_browser)"
echo "RESULT shm_segments_leaked $(( $(ipcs -m | grep -c '^0x') - shm_before ))"
echo "RESULT semaphores_leaked $(( $(ipcs -s | grep -c '^0x') - sem_before ))"
echo "--- lib log (filtered)"
echo "RESULT do_lib_hooks_installed $(grep -h 'Flash hooks installed' logs/*.log 2>/dev/null | wc -l)"
grep -hE "\[\+\]|\[-\]|Flash hooks|trampolines|\[debug\]|extract-and-run|restarting|Flash\] found|FlashIpc|exited|killed by|Failed|failed|!" logs/*.log 2>/dev/null | sed -E 's/^\[[^]]+\] //' | sort | uniq -c | sort -rn | head -40

echo "RESULT leftover_extracted_dirs $(ls -d /tmp/appimage_extracted_* 2>/dev/null | wc -l)"
kill $WM $XVFB $SERVER 2>/dev/null
pkill -9 -f '^(lib/darkbot_browser_linux|/tmp/(appimage_extracted_|\.mount_darkbo))' 2>/dev/null
exit $status
