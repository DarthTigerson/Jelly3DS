#!/usr/bin/env bash
# Packs build/3ds/jelly3ds.elf into Jelly3DS.cia (needs makerom + bannertool, built by scripts/build-tools.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/build/tools/bin"
OUT="$ROOT/build/3ds"
if [ ! -x "$TOOLS/makerom" ] || [ ! -x "$TOOLS/bannertool" ]; then
    echo "makerom/bannertool missing: run scripts/build-tools.sh to also produce a .cia"
    exit 0
fi
"$TOOLS/bannertool" makebanner -i "$ROOT/resources/banner.png" -a "$ROOT/resources/banner.wav" -o "$OUT/banner.bnr"
"$TOOLS/bannertool" makesmdh -s "Jelly3DS" -l "Jellyfin player with offline downloads" -p "Thomas Bonnici" \
    -i "$ROOT/resources/icon.png" -o "$OUT/icon.icn"
"$TOOLS/makerom" -f cia -o "$OUT/Jelly3DS.cia" -target t -exefslogo \
    -elf "$OUT/jelly3ds.elf" -rsf "$ROOT/resources/jelly3ds.rsf" \
    -icon "$OUT/icon.icn" -banner "$OUT/banner.bnr" \
    -DAPP_ROMFS="$ROOT/romfs" -ver 1
echo "Built $OUT/Jelly3DS.cia"
