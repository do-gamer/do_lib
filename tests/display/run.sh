#!/usr/bin/env bash
# Display power test: does the game keep running at full speed while the display is off and
# the session is locked (an unattended bot)? Runs on the real desktop: it needs X11 with
# DPMS (Xvfb has no display power states, the problem can't be reproduced there).
#
#   tests/display/run.sh [LIB_DIR]   a test SWF that renders through Stage3D like the game,
#                                    launched through the real bot path (JVM + DarkTanos.so +
#                                    AppImage + do_lib). LIB_DIR holds DarkTanos.so,
#                                    libdo_lib.so and darkbot_browser_linux.AppImage
#                                    (default: the current build, see ./build.sh -b)
#   tests/display/run.sh --game      the game of the bot running right now (logged in), seen
#                                    through do_lib's game-thread heartbeat; launches nothing
#
# Phases: visible -> display off -> lock screen -> lock screen + display off (the overnight
# state) -> display on. The screen goes dark during the test: don't touch mouse or keyboard
# (that wakes the display; those seconds are excluded). Ctrl+C restores the display.
#
# Environment:
#   PHASE_SECONDS=20   length of the display-on phases
#   OFF_SECONDS=20     length of the display-off phases (e.g. 1800 for a 30 minute soak)
#   REAL_LOCK=1        lock the session with the real screen locker instead of covering the
#                      screen with a window (unlock it yourself; the result is also saved)
#   LOCK_CMD=...       lock command for REAL_LOCK (default: cinnamon-screensaver-command
#                      --lock, else loginctl lock-session)
#   GAP_LIMIT_MS=...   longest allowed gap between two game-thread frames (default 250;
#                      500 with --game, the game's GUI timer ticks less evenly than frames)
#   KEEP_WORK=1        keep the work directory (logs) also when the test passes
#
# Exit code: 0 pass, 1 fail, 2 inconclusive or not runnable here.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/tests/display"
IT="$ROOT/tests/integration"

MODE=swf
LIBDIR=""
for arg in "$@"; do
    case "$arg" in
        --game) MODE=game ;;
        -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) LIBDIR="$arg" ;;
    esac
done

PHASE=${PHASE_SECONDS:-20}
OFF=${OFF_SECONDS:-$PHASE}
GAP_LIMIT=${GAP_LIMIT_MS:-$([[ $MODE == game ]] && echo 500 || echo 250)}

# inherited from VS Code's terminal: makes the browser (Electron) start as plain Node and exit
unset ELECTRON_RUN_AS_NODE ELECTRON_NO_ATTACH_CONSOLE

now_ms() { date +%s%3N; }
monitor_state() { xset q 2>/dev/null | sed -n 's/.*Monitor is \([A-Za-z]*\).*/\1/p'; }
die() { echo "error: $*" >&2; exit 2; }

# --- preflight -------------------------------------------------------------------------
[[ -n "${DISPLAY:-}" ]] || die "no X display: run this in your desktop session"
[[ "${XDG_SESSION_TYPE:-x11}" == "wayland" ]] && die "Wayland session: DPMS can't be driven through X here"
xset q 2>/dev/null | grep -q "DPMS is" || die "the X server has no DPMS (Xvfb?): display power can't be tested here"
[[ -n "$(monitor_state)" ]] || die "can't read the display power state (xset q)"
command -v python3 >/dev/null || die "python3 not found"

# Things that stop the bot completely and that no browser setting can help with.
warn_suspend() {
    command -v gsettings >/dev/null || return 0
    local schema key v
    for schema in org.cinnamon.settings-daemon.plugins.power org.gnome.settings-daemon.plugins.power; do
        gsettings list-keys "$schema" >/dev/null 2>&1 || continue
        for key in lid-close-ac-action lid-close-battery-action; do
            v=$(gsettings get "$schema" "$key" 2>/dev/null) || continue
            case "$v" in
                *suspend*|*hibernate*)
                    echo "  note: $key = $v: closing the lid suspends the laptop and the bot stops" \
                         "completely (not something the bot can survive; use 'blank' or 'nothing')" ;;
            esac
        done
        for key in sleep-inactive-ac sleep-inactive-battery; do
            v=$(gsettings get "$schema" "$key-timeout" 2>/dev/null | awk '{print $NF}') || continue
            [[ -z "$v" || "$v" == 0 ]] && continue
            [[ "$(gsettings get "$schema" "$key-type" 2>/dev/null)" == "'nothing'" ]] && continue
            echo "  note: $key-timeout = ${v}s: the laptop suspends after that long idle and the bot stops"
        done
        return 0
    done
}
warn_suspend

WORK="$(mktemp -d /tmp/tanos_display.XXXX)"
STOP="$WORK/stop"
PIDS=()
COVER=""
JAVA=""
xset q | grep -q "DPMS is Enabled" && DPMS_WAS_ENABLED=1 || DPMS_WAS_ENABLED=0

cleanup() {
    trap - EXIT INT TERM
    touch "$STOP"
    [[ -n "$COVER" ]] && kill "$COVER" 2>/dev/null
    xset dpms force on 2>/dev/null
    [[ $DPMS_WAS_ENABLED == 0 ]] && xset -dpms 2>/dev/null
    if [[ -n "$JAVA" ]] && kill -0 "$JAVA" 2>/dev/null; then
        for _ in $(seq 1 50); do kill -0 "$JAVA" 2>/dev/null || break; sleep 0.2; done
        kill "$JAVA" 2>/dev/null
    fi
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    [[ "${REMOVE_WORK:-}" == 1 ]] && rm -rf "$WORK"
}
trap cleanup EXIT
trap 'echo; echo "interrupted (display restored)"; exit 130' INT TERM

# a few seconds of steady frames before the test starts
wait_steady() {
    for _ in $(seq 1 90); do
        [[ -f "$WORK/samples.log" ]] && \
            [[ $(grep '^second ' "$WORK/samples.log" | tail -3 | grep -c 'frames=[1-9]') == 3 ]] && return 0
        [[ -n "$JAVA" ]] && ! kill -0 "$JAVA" 2>/dev/null && return 1
        sleep 1
    done
    return 1
}

# Whether the browser's GPU process runs with vsync (informational).
gpu_vsync_info() {
    local pid cmd found=""
    for pid in $(pgrep -f -- '--type=gpu-process'); do
        cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null) || continue
        [[ "$cmd" == *darkbot_browser* || "$cmd" == *.mount_darkbo* || "$cmd" == *appimage_extracted_* ]] || continue
        [[ "$cmd" == *--disable-gpu-vsync* ]] && found="vsync off (--disable-gpu-vsync)" || found="vsync ON"
    done
    echo "${found:-not found}"
}

# --- start what we measure ---------------------------------------------------------------
if [[ $MODE == swf ]]; then
    if [[ -z "$LIBDIR" ]]; then
        LIBDIR="$WORK/libs"
        mkdir -p "$LIBDIR"
        cp "$ROOT/build/client/DarkTanos.so" "$ROOT/build/do_lib/libdo_lib.so" \
           "$ROOT/browser/dist/darkbot_browser_linux.AppImage" "$LIBDIR/" 2>/dev/null \
            || die "build artifacts missing, run ./build.sh -b first (or pass LIB_DIR)"
    fi
    LIBDIR="$(cd "$LIBDIR" && pwd)" || die "no such directory: $LIBDIR"
    for f in DarkTanos.so libdo_lib.so darkbot_browser_linux.AppImage; do
        [[ -f "$LIBDIR/$f" ]] || die "$LIBDIR/$f missing"
    done
    if ! command -v javac >/dev/null && [[ -x "$HOME/.sdkman/candidates/java/current/bin/javac" ]]; then
        PATH="$HOME/.sdkman/candidates/java/current/bin:$PATH"
    fi
    command -v javac >/dev/null || die "javac (JDK 11+) not found"

    # same layout as the bot: the client starts lib/darkbot_browser_linux.AppImage from its cwd
    mkdir -p "$WORK/lib" "$WORK/classes"
    cp "$LIBDIR/libdo_lib.so" "$LIBDIR/darkbot_browser_linux.AppImage" "$WORK/lib/"
    chmod +x "$WORK/lib/darkbot_browser_linux.AppImage"
    python3 "$IT/make_swf.py" "$WORK/test.swf" --stage3d >/dev/null || die "make_swf.py failed"
    javac -d "$WORK/classes" "$IT/java/eu/darkbot/api/DarkTanos.java" "$IT/java/TanosIT.java" || die "javac failed"
    ln -s events.log "$WORK/samples.log"

    PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    DISPLAY_TEST=1 python3 "$IT/server.py" "$WORK" "$PORT" &
    PIDS+=($!)
    echo "launching the browser through DarkTanos.so like the bot ($LIBDIR)"
    (cd "$WORK" && exec java -Dtanos.lib="$LIBDIR/DarkTanos.so" -cp "$WORK/classes" TanosIT \
        "http://127.0.0.1:$PORT/" "$WORK/test.swf" --hold "$STOP" 86400) > "$WORK/java.log" 2>&1 &
    JAVA=$!

    for _ in $(seq 1 120); do
        grep -q '^stage3d ' "$WORK/events.log" 2>/dev/null && break
        kill -0 "$JAVA" 2>/dev/null || die "the client exited: $(tail -3 "$WORK/java.log") (logs: $WORK)"
        sleep 1
    done
    DRIVER=$(sed -n 's/^stage3d //p' "$WORK/events.log" 2>/dev/null | head -1)
    [[ -n "$DRIVER" ]] || die "the test SWF got no Stage3D context in 120 s (logs: $WORK)"
    echo "  Stage3D driver: $DRIVER"
    [[ "$DRIVER" == *oftware* ]] && die "Stage3D renders in software: not the game's GPU path, the test would mean nothing"
    TITLE="test SWF, Stage3D $DRIVER, libs from $LIBDIR"
else
    python3 "$HERE/heartbeat.py" "$WORK/samples.log" "$STOP" &
    PIDS+=($!)
    for _ in $(seq 1 10); do grep -q '^attached ' "$WORK/samples.log" 2>/dev/null && break; sleep 1; done
    grep -q '^attached ' "$WORK/samples.log" 2>/dev/null \
        || die "no running game with do_lib's hooks found: start the bot and log in first"
    echo "watching the running bot's game thread ($(grep -m1 '^attached ' "$WORK/samples.log"))"
    TITLE="running bot's game thread (do_lib heartbeat)"
fi

wait_steady || die "the game thread isn't ticking steadily (logs: $WORK)"
echo "  browser GPU process: $(gpu_vsync_info)"

( while [[ ! -e "$STOP" ]]; do echo "$(now_ms) $(monitor_state)"; sleep 0.5; done ) >> "$WORK/dpms.log" &
PIDS+=($!)

# --- phases --------------------------------------------------------------------------------
run_phase() {  # name, expected display power (on|off), seconds
    local name=$1 expect=$2 secs=$3 start deadline state
    start=$(now_ms)
    deadline=$(( start + secs * 1000 ))
    printf '  %s  %-18s %5ss  display %s\n' "$(date +%T)" "$name" "$secs" "$expect"
    while (( $(now_ms) < deadline )); do
        state=$(monitor_state)
        if [[ $expect == off && $state == On ]]; then
            echo "      display woke up (input?), turning it off again"
            xset dpms force off
        elif [[ $expect == on && $state != On ]]; then
            xset dpms force on
        fi
        sleep 1
    done
    echo "phase $name $start $(now_ms) $expect" >> "$WORK/phases.log"
}

lock() {
    if [[ "${REAL_LOCK:-}" == 1 ]]; then
        if [[ -n "${LOCK_CMD:-}" ]]; then eval "$LOCK_CMD"
        elif command -v cinnamon-screensaver-command >/dev/null; then cinnamon-screensaver-command --lock
        else loginctl lock-session; fi
    else
        python3 "$HERE/cover.py" &
        COVER=$!
    fi
    sleep 1
}

unlock() {
    [[ -n "$COVER" ]] && { kill "$COVER" 2>/dev/null; wait "$COVER" 2>/dev/null; COVER=""; }
}

TOTAL=$(( 3 * PHASE + 2 * OFF ))
echo
echo "The test takes about ${TOTAL}s; the screen goes dark in between."
echo "Don't touch mouse or keyboard until it ends. Ctrl+C aborts (and restores the display)."
sleep 3
[[ $DPMS_WAS_ENABLED == 0 ]] && xset +dpms

run_phase visible on "$PHASE"
xset dpms force off
run_phase display_off off "$OFF"
xset dpms force on
lock
run_phase lock_screen on "$PHASE"
xset dpms force off
run_phase lock_screen+off off "$OFF"
xset dpms force on
unlock
run_phase display_on on "$PHASE"

touch "$STOP"
if [[ -n "$JAVA" ]]; then
    for _ in $(seq 1 100); do kill -0 "$JAVA" 2>/dev/null || break; sleep 0.2; done
    grep -E '^RESULT|^EVENT|FAIL' "$WORK/java.log" | sed 's/^/  client: /'
fi
echo
python3 "$HERE/analyze.py" "$WORK" --gap-limit "$GAP_LIMIT" --title "$TITLE"
status=$?
[[ "${REAL_LOCK:-}" == 1 ]] && echo "(session locked by the test: unlock it; result saved in $WORK/result.txt)"
if [[ $status == 0 && "${KEEP_WORK:-}" != 1 && "${REAL_LOCK:-}" != 1 ]]; then
    REMOVE_WORK=1  # by cleanup, after everything writing to it has stopped
else
    echo "logs: $WORK"
fi
exit $status
