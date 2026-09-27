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

Flags: `-c` clean build, `-b` build the browser, `-d` portable build in Docker.

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
