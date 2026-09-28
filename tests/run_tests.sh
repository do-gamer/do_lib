#!/usr/bin/env bash
# Builds and runs the native tests (no browser/flash needed).
#   tests/run_tests.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-tests"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON >/dev/null
cmake --build "$BUILD" -j "$(nproc 2>/dev/null || echo 2)" --target flash_ipc_test proc_util_test sock_ipc_test >/dev/null

# the sock test runs the browser's command server in Node (Electron 11 ships Node 12)
if ! command -v node >/dev/null 2>&1 && [[ -s "${NVM_DIR:-$HOME/.nvm}/nvm.sh" ]]; then
    . "${NVM_DIR:-$HOME/.nvm}/nvm.sh" >/dev/null
fi

cd "$(mktemp -d)" # tests write logs/ into the working directory
status=0
"$BUILD/tests/flash_ipc_test" || status=1
"$BUILD/tests/proc_util_test" || status=1
"$BUILD/tests/sock_ipc_test" || status=1
echo "== browser command server fuzzing"
node --expose-gc "$ROOT/tests/command_server_fuzz.js" || status=1
exit $status
