# Jelly3DS

A Jellyfin client for the Nintendo 3DS family, built for **offline viewing**: download episodes at home,
watch them anywhere (office, train) with headphones, and your watch progress flows back to Jellyfin the
next time the console can reach your server.

Inspired by [switchfin](https://github.com/dragonflylee/switchfin); rewritten for the 3DS (libctru + citro2d + FFmpeg).

## Features

- One `.cia` / `.3dsx` for **every model**: the app detects New vs Old 3DS at startup.
- **Your server converts everything first**: downloads and streams arrive as MPEG-TS with H.264 Constrained Baseline + AAC stereo:
  - New 3DS / New 2DS XL: up to 400×240, 30 fps, decoded by the **hardware H.264 decoder (MVD)**
  - Old 3DS / 2DS: up to 320×192, 20 fps, software decoding
  - Override in *Settings → Video quality*
- **Downloads**: episode, whole season (`Y` in a season) or whole series (`Y` in a series). The queue persists across restarts.
- **Audio / subtitles chosen per download**; subtitles are burned into the video by the server. Your choice is remembered (by language) as the default.
- **Offline progress**: resume position and watched state are saved on the SD card every 10 s and on pause/stop/sleep,
  then synced to Jellyfin with the real time you watched. If you watched further on another device later, the server's newer state wins.
- Server progress is pulled for your downloads too (watch on the TV at home, continue on the 3DS).
- **Streaming** over Wi-Fi with the same conversion (seeking restarts the conversion at the new point).
- **Up next**: when an episode ends, the next downloaded episode plays after 5 s.
- **Headphone mode**: `X` in the player turns the bottom screen off.

## Install

**With FBI:** open FBI → *Remote Install* → *Scan QR Code* and scan:

<img src="docs/install-qr.png" alt="QR code: install Jelly3DS with FBI" width="200">

This installs the latest `Jelly3DS.cia` from the [releases page](https://github.com/DarthTigerson/Jelly3DS/releases/latest).

**Manually:**

1. Download `Jelly3DS.cia` from the [releases page](https://github.com/DarthTigerson/Jelly3DS/releases/latest), copy it to the SD card and install it with FBI (or copy `Jelly3DS.3dsx` to `sdmc:/3ds/` for the Homebrew Launcher).
2. Sound needs the DSP firmware dump at `sdmc:/3ds/dspfirm.cdc`. Most homebrew setups already have it; otherwise run *DSP1* once.
3. Start Jelly3DS, enter your server address (e.g. `192.168.1.20:8096`), username and password.

Data lives in `sdmc:/3ds/Jelly3DS/` (`config.json`, `progress.json`, `downloads/<itemId>/video.ts`).

## Controls

| Where | Keys |
|---|---|
| Lists | D-pad move / page, `A` open, `B` back, `L`/`R` switch home tabs, touch the bottom buttons |
| Episode list | `A` actions (resume, play, download, delete), `X` watched / unwatched, `Y` download season |
| Downloads tab | `A` play, `Y` more, `X` delete |
| Player | `A` pause, D-pad ◀▶ ±10 s, `L`/`R` ±60 s, `Y` show info, `X` bottom screen off, `B` stop, touch the seek bar |

## Building

Everything builds in Docker with the official devkitPro image; the core logic also builds natively for tests.

```sh
# FFmpeg (h264/aac decoders + mpegts/mov demuxers) for the 3DS, once
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm scripts/build-ffmpeg.sh
# makerom + bannertool for the .cia, once
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm scripts/build-tools.sh
# App: build/3ds/Jelly3DS.3dsx and build/3ds/Jelly3DS.cia
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm scripts/build-3ds.sh

# Host: unit tests + CLI (same download/sync code as the console)
cmake -S . -B build/host && cmake --build build/host && ./build/host/jelly_tests
./build/host/jelly-cli ./cli-data login 192.168.1.20:8096 user password
```

## Layout

```
core/      portable C++17: Jellyfin client, transcode profiles, download queue, progress store, sync engine
platform/  libcurl HTTP used by both the 3DS app and the host tools
app/       3DS app: citro2d UI (ui/), player (player/: FFmpeg demux, MVD / FFmpeg+Y2R video, NDSP audio)
tests/     doctest unit tests for core/
tools/cli  host CLI driving core/ against a real server
```
