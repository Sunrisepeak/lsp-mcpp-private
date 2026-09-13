#!/usr/bin/env bash
# THROWAWAY SPIKE — reproduces the experiment matrix of
# ../../2026-09-13-cxx-modules-lsp-experiments.md in a scratch directory.
#
#   CLANGD=/path/to/clangd CLANG=/path/to/llvm-22/bin/clang++ PROBE_TIMEOUT=20 ./run.sh [workdir]
#
# Requires: mcpp with toolchains gcc@16.1.0 and llvm@22.1.8 plus the
# x86_64-windows-gnu (MinGW-w64) target installed, and python3.
# PROBE_TIMEOUT bounds every LSP request; keep it small with clangd 23.1,
# which never answers some requests when a module cannot be resolved (E13).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CLANGD=${CLANGD:-clangd}
CLANG=${CLANG:-clang++}
WORK=${1:-$(mktemp -d)}
FLAGS=(--experimental-modules-support --use-dirty-headers)

# name | mcpp build arguments
CASES=(
  "gcc-linux|--toolchain gcc@16.1.0"
  "llvm-linux|--toolchain llvm@22.1.8"
  "gcc-mingw|--target x86_64-windows-gnu"
)

for case in "${CASES[@]}"; do
  name=${case%%|*}
  read -r -a build_args <<< "${case#*|}"
  P="$WORK/$name"
  rm -rf "$P" && cp -r "$HERE/fixture" "$P"
  (cd "$P" && MCPP_OFFLINE=1 mcpp build "${build_args[@]}" -q)
  python3 "$HERE/normalize_cdb.py" "$P/compile_commands.json" "$P/.lsp-mcpp/cdb" --clang "$CLANG"
  echo "===== $name: build CDB as-is"
  python3 "$HERE/lsp_probe.py" "$CLANGD" "$P" "$P" "${FLAGS[@]}" || true
  echo "===== $name: normalized CDB"
  python3 "$HERE/lsp_probe.py" "$CLANGD" "$P" "$P/.lsp-mcpp/cdb" "${FLAGS[@]}"
done
