# Tanos API lib build and test guide

## Goal
Build all required artifacts with one command:

```bash
./build.sh -b
```

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

## Build libs + browser

From repo root:

```bash
./build.sh -b
```

What this does:

1. Builds browser AppImage in `browser/dist/`
2. Configures/builds CMake project in `build/`
3. Renames `build/client/libDarkTanos.so` to `build/client/DarkTanos.so`
4. Strips symbols from `DarkTanos.so` and `libdo_lib.so`
5. Runs `./copy.sh` automatically if that file exists and is executable

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
