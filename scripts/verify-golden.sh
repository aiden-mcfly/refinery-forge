#!/usr/bin/env bash
# Enforce doc/code parity (SUBSTRATE-LAYOUT.md): default embedded canon must match
# repository golden.marker.bin byte-for-byte after a clean forge build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build
clang -std=c11 -O2 -c third_party/xxhash/xxhash.c -I third_party/xxhash -o build/xxhash.o
clang++ -std=c++20 -O2 -Wall -Wextra -I include -I third_party/xxhash \
  src/forge/bitmask_generator.cpp src/forge/ledger.cpp \
  src/forge/substrate_emit.cpp src/forge/tank.cpp \
  build/xxhash.o -o build/refinery-forge
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
./build/refinery-forge --out "$TMP"
if ! cmp -s golden.marker.bin "$TMP"; then
  echo "verify-golden: golden.marker.bin drift — regenerate with:" >&2
  echo "  ./build/refinery-forge --out golden.marker.bin" >&2
  exit 1
fi
echo "verify-golden: OK (golden.marker.bin matches default canon output)"
