#!/usr/bin/env bash
set -euo pipefail

SCRIPTSRC=`readlink -f "$0" || echo "$0"`
RUN_PATH=`dirname "${SCRIPTSRC}" || echo .`

cd ${RUN_PATH}

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

echo "Using Node.js version from .nvmrc"
# Use or install Node.js version from .nvmrc
nvm use || nvm install

echo "Installing dependencies."
npm install

echo "Building app."
npm run dist -- --linux

echo "Build completed."
