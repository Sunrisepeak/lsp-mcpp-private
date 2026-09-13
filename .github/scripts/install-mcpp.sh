#!/usr/bin/env bash
# Installs the pinned mcpp through xlings and selects the pinned LLVM toolchain.
# Retries because the index and the release mirrors are reached over the network.
set -euo pipefail
: "${MCPP_VERSION:?pin the mcpp version in the workflow}"
: "${LLVM_VERSION:?pin the LLVM toolchain version in the workflow}"

for attempt in 1 2 3 4 5; do
    xlings update > /dev/null 2>&1 || true
    if xlings install "mcpp@$MCPP_VERSION" -y -g; then break; fi
    if [ "$attempt" = 5 ]; then
        echo "::error::mcpp@$MCPP_VERSION could not be installed"
        exit 1
    fi
    sleep 30
done
mcpp --version
mcpp self config --mirror GLOBAL

for attempt in 1 2 3; do
    if mcpp toolchain install llvm "$LLVM_VERSION"; then break; fi
    if [ "$attempt" = 3 ]; then exit 1; fi
    sleep 30
done
mcpp toolchain default "llvm@$LLVM_VERSION"
