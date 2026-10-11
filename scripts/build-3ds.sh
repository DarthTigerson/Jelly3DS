#!/usr/bin/env bash
# Builds Jelly3DS.3dsx (and Jelly3DS.cia when makerom/bannertool are available).
# Run inside the devkitpro/devkitarm image:
#   docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm scripts/build-3ds.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
[ -f third_party/ffmpeg-3ds/lib/libavcodec.a ] || scripts/build-ffmpeg.sh
cmake -S . -B build/3ds -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build/3ds -j"$(nproc)"
scripts/make-cia.sh
