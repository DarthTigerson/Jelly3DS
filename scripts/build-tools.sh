#!/usr/bin/env bash
# Builds makerom (Project_CTR) and bannertool from source into build/tools/bin (needed for the .cia).
#   docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm scripts/build-tools.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/tools"
mkdir -p "$WORK/bin"
cd "$WORK"
command -v g++ >/dev/null || (apt-get update -qq && apt-get install -y -qq build-essential >/dev/null)

if [ ! -x bin/makerom ]; then
    rm -rf Project_CTR
    git clone --depth 1 --branch makerom-v0.18.4 https://github.com/3DSGuy/Project_CTR.git
    make -C Project_CTR/makerom deps >/dev/null
    make -C Project_CTR/makerom program -j"$(nproc)" >/dev/null
    cp Project_CTR/makerom/bin/makerom bin/
fi

if [ ! -x bin/bannertool ]; then
    rm -rf bannertool
    git clone --depth 1 --recurse-submodules https://github.com/carstene1ns/3ds-bannertool.git bannertool
    cmake -S bannertool -B bannertool/build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build bannertool/build -j"$(nproc)" >/dev/null
    cp bannertool/build/bannertool bin/
fi
ls -la bin
