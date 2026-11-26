# Tanos API lib build and test guide

## Goal
Build all required artifacts with one command:

```bash
./build.sh -b -d
```

`-d` builds the native libraries in Docker with an Ubuntu 20.04 toolchain so they run on
all common distributions (see [Portability](#portability)). Without `-d` the libraries are
built with the host toolchain and may require a newer glibc than your users have; the build
prints a warning in that case.

Expected output files:

- `browser/dist/darkbot_browser_linux.AppImage`
- `build/client/DarkTanos.so`
- `build/do_lib/libdo_lib.so`

## Prerequisites (Linux)

- `cmake` and a C++ toolchain (`gcc/g++`, `make`)
- `strip` (usually from `binutils`)
- `nvm` installed (`~/.nvm/nvm.sh` must exist)
- Node.js version from `browser/.nvmrc` (the script runs `nvm use || nvm install`)
- `npm`
- `docker` (only for `-d`, recommended for builds you distribute)

## Build libs + browser

From repo root:

```bash
./build.sh -b
```

What this does:

1. Builds browser AppImage in `browser/dist/` (`-b`)
2. Configures/builds CMake project in `build/` (optimized `-O2`, LTO; in Docker with `-d`)
3. Renames `build/client/libDarkTanos.so` to `build/client/DarkTanos.so`
4. Strips symbols from `DarkTanos.so` and `libdo_lib.so`
5. Reports the minimum glibc version the libraries need
6. Runs `./copy.sh` automatically if that file exists and is executable

Flags: `-c` clean build, `-b` build the browser, `-d` portable build in Docker,
`--no-copy` skip `copy.sh` (e.g. test builds).

## Set executable permission for AppImage

```bash
chmod +x browser/dist/darkbot_browser_linux.AppImage
```

## Quick verification / smoke test

Check artifacts exist:

```bash
ls -lh \
  browser/dist/darkbot_browser_linux.AppImage \
  build/client/DarkTanos.so \
  build/do_lib/libdo_lib.so
```

Check library dependencies resolve:

```bash
ldd build/client/DarkTanos.so
ldd build/do_lib/libdo_lib.so
```

Optional AppImage smoke check:

```bash
./browser/dist/darkbot_browser_linux.AppImage --appimage-version
```

## Tests

```bash
# native tests: flash IPC protocol, /proc + memory utilities, browser command channel,
# command server fuzzing
tests/run_tests.sh

# end-to-end: real JVM + DarkTanos.so + AppImage + pepper flash under Xvfb (Docker)
tests/integration/run.sh              # without FUSE (AppImage extract-and-run mode)
tests/integration/run.sh "" fuse      # with FUSE

# the built libraries load on Ubuntu 20.04-24.04, Mint 21-22, Debian 11-12, Fedora, Arch
tests/distro/run.sh
```

Integration test options (environment variables for `tests/integration/run.sh`):

| Variable | Effect |
|---|---|
| `TEST_SWF=/path/main.swf` | load a real game client instead of the generated test SWF |
| `SOAK_SECONDS=N` | keep the client running N seconds, report validity and memory scan speed |
| `STRESS_SECONDS=N` | busy-bot simulation: concurrent API use, periodic refresh/crash injection, resource stats per minute |
| `JAVA_PRELOAD=...libasan.so.6` + `ASAN_OPTIONS=...` | run with a sanitizer-instrumented `DarkTanos.so` |

Sanitizer builds of the native tests: configure with
`-DBUILD_TESTS=ON -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"` (or `thread`) into a
`build-*` directory.

The native tests use the production IPC server (`do_lib/ipc.cpp`) and client
(`client/flash_ipc_client.cpp`) against a simulated game thread, and the production
browser command server (`browser/src/command_server.js`) in Node.

### Display power test (screen off / locked)

An unattended bot runs with the session locked and the display powered off. The game must
keep its speed then: with GPU vsync, Xorg paces every frame at 1 Hz while no display is on
(the game thread ticked every 1-3 s, ping climbed and IPC calls timed out until the screen
woke up). The browser therefore runs with `--disable-gpu-vsync`. This test guards it; it
must run on the real desktop (X11 with DPMS: Xvfb has no display power states):

```bash
tests/display/run.sh                # test SWF via the real bot path: JVM + DarkTanos.so + AppImage + do_lib
tests/display/run.sh ../bot/lib     # the same with the libs from another directory
tests/display/run.sh --game         # the game of the bot running right now (logged in), nothing is launched
```

Phases: visible, display off, lock screen, lock screen + display off (the overnight state),
display on. The screen goes dark during the test; don't touch mouse or keyboard (that
wakes the display, those seconds don't count; Ctrl+C restores the display). Each phase must
keep >= 80% of the visible frame rate with no gap over 250 ms (500 ms with `--game`). The
test SWF renders through Stage3D in `wmode=direct` like the game and reports every frame
of the player's main thread; `--game` reads do_lib's game-thread heartbeat from its shared
memory (read-only). Expected result with vsync (the bug) vs. the fix:

```
phase           display  frames/s  worst gap        phase           display  frames/s  worst gap
display_off     off             1    1998 ms        display_off     off            60      18 ms
lock_screen+off off             1    1999 ms        lock_screen+off off            60      18 ms
```

| Variable | Effect |
|---|---|
| `PHASE_SECONDS=20` / `OFF_SECONDS=20` | phase lengths; e.g. `OFF_SECONDS=1800` for a 30 minute soak with the display off |
| `REAL_LOCK=1` | lock with the real screen locker (`LOCK_CMD=...` to override) instead of a covering window; unlock yourself at the end |
| `GAP_LIMIT_MS=...` | longest allowed gap between two game-thread frames |
| `KEEP_WORK=1` | keep the logs also when the test passes |

The test also warns when closing the lid or idling suspends the laptop: a suspended laptop
stops the bot completely, no browser setting can help there (set the lid action to blank /
do nothing).

Without vsync, frames are paced by Chromium alone: with the window visible the player gets
~59 instead of 60 frames/s (one ~35 ms frame per second); CPU use is unchanged.

## Portability

- Libraries built with `-d` need glibc 2.18+ and link libstdc++/libgcc statically; the only
  runtime dependencies are `libX11` and `libXext` (present on any desktop).
- The AppImage normally mounts itself with FUSE 2. Distributions that don't ship
  `libfuse.so.2` (Ubuntu 22.04+, Mint 21+, Fedora, ...) are detected and the browser runs in
  extract-and-run mode automatically; the same fallback kicks in if mounting fails.
- The browser is started with `--no-sandbox` (required where unprivileged user namespaces
  are restricted, e.g. Ubuntu 24.04).
- X11 or XWayland is required for window control and mouse input (`DISPLAY` set).

## Diagnostics

Logs are written to `logs/<start time>_TanosApi.log` (bot client, browser and flash
process share one file). Set `TANOS_DEBUG=1` in the bot's environment to also log flash
hook activity (JIT verifications, GC chunk frees).

do_lib works around two memory leaks of the game client: tooltips of discarded action bar
elements and dead ships kept alive by their HUD's lock listener. Both cleaners run on the
game thread every 15 s and log `[tooltip] cleaner ready` / `[ships] cleaner ready` at
start, then what they removed (`[tooltip] removed ...`, `[ships] released ...`). If a
game update changes the classes they rely on, they log `cleaner disabled: <reason>` and
stay off.

Leftover shared memory segments from older versions (killed flash processes) can be
listed with `ipcs -m` (1024 byte segments with `nattch 0`) and removed with `ipcrm -m <id>`.

## DarkBot build requirements

Your DarkBot base must be:

- Latest changes from: <https://github.com/darkbot-reloaded/DarkBot>
- Including PR: <https://github.com/darkbot-reloaded/DarkBot/pull/449>
- Including PR: <https://github.com/darkbot-reloaded/DarkBot/pull/448>
- Including PR: <https://github.com/darkbot-reloaded/DarkBot/pull/453>

After building, copy these files into your DarkBot `lib` directory:

- `browser/dist/darkbot_browser_linux.AppImage`
- `build/client/DarkTanos.so`
- `build/do_lib/libdo_lib.so`

You can copy them manually, or use `copy.sh` as described below.

### Important: prevent lib overwrite in DarkBot

In DarkBot `LibSetup`, disable/skip logic that overwrites these files in the bot `lib` directory:

- `DarkTanos.so`
- `libdo_lib.so`

This is required so your locally built versions remain in use.

## Optional copy helper

If you want automatic copy to your DarkBot `lib` folder after each build:

1. Copy `copy.sh.example` to `copy.sh`
2. Set destination path (`DEST`) in `copy.sh`
3. Make it executable:

```bash
cp copy.sh.example copy.sh
chmod +x copy.sh
```

Then `./build.sh -b` will copy artifacts automatically.
