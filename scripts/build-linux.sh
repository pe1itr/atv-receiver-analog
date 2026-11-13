#!/bin/bash
# Native receiver build, with diagnostics kept in build/linux-build.log.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! command -v make >/dev/null 2>&1; then
    bash scripts/check-build-deps.sh
    exit 1
fi
mkdir -p build
log=build/linux-build.log
echo "Building Linux receiver; compiler warnings and errors are saved in $log"
if make -j2 "$@" all 2>&1 | tee "$log"; then
    echo 'Build successful. Start the demo: ./build/atv-rx --no-config --no-wizard --demo'
    if LC_ALL=C grep -Eiq 'warning:|WARNING:' "$log"; then
        echo "WARNING: the build reported warnings; review $log."
    fi
else
    echo "ERROR: build failed. Read the first error in $log and the instructions in INSTALL.md." >&2
    echo 'For missing packages, follow the installation command above. After changing dependencies, rerun: bash scripts/build-linux.sh -B' >&2
    exit 1
fi
