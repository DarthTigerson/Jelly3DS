# Jelly3DS — Design

Date: 2026-10-11. Status: approved in conversation (user granted full autonomy for the remaining sections).

## Goal

A native Jellyfin client for Nintendo 3DS (Old and New) whose primary job is **offline viewing**:
download episodes at home, watch them at the office with headphones, and have watched state /
resume positions flow back to Jellyfin the next time the console sees the server.
Streaming over Wi-Fi is also supported. Inspired by switchfin (`~/Documents/switchfin`), whose
borealis/mpv stack cannot run on 3DS; we reuse its ideas and Jellyfin API knowledge, not its code.

## Decisions

| Topic | Decision |
|---|---|
| Hardware | One `.cia` + `.3dsx` for all models; `APT_CheckNew3DS()` picks the runtime profile |
| Conversion | Always server-side by Jellyfin before bytes reach the 3DS |
| Format | MPEG-TS, H.264 **baseline**, AAC stereo 32 kHz. TS chosen over MP4 because Jellyfin's progressive MP4 is fragmented and not reliably seekable once saved |
| New 3DS profile | ≤400×240, ≤30 fps, ~600 kbps video, decoded by **MVD** hardware |
| Old 3DS profile | ≤320×192, ≤20 fps, ~350 kbps video, FFmpeg software decode (loop filter skipped) |
| Override | Settings can force either profile |
| Scope v1 | Login, browse libraries/series/seasons/episodes/movies, manual download queue, offline playback with resume, streaming, offline progress sync |
| Download selection | Manual only (episode / season / series) |
| Subtitles | Burned in server-side (`SubtitleMethod=Encode`); audio + subtitle chosen per download, remembered as defaults (by language) |
| Partial downloads | Transcoded streams have no byte ranges → a failed/interrupted download restarts from zero |

## Architecture

```
core/        portable C++17, no 3DS headers, unit-tested on macOS
  jellyfin/    models + API client (auth, browse, playback reporting)
  transcode/   DeviceProfile → stream URL
  downloads/   queue + index.json + per-item folders
  progress/    local resume positions + pending-sync log
  sync/        replay pending log to server, pull server positions
  platform.hpp IHttp, IFileSystem, IClock interfaces
platform/ctr   3DS impls (libcurl+mbedTLS, sdmc, osGetTime)
platform/host  macOS impls (libcurl, std::filesystem) for tests + CLI
player/      3DS: FFmpeg demux → VideoDecoder (MvdDecoder | SwDecoder) → GPU texture; AAC → NDSP; audio-clock A/V sync
ui/          3DS: citro2d screens, swkbd input
tools/cli    host CLI exercising core against a real server (login, browse, download)
tests/       doctest unit tests for core
```

Threads (3DS): main/UI (core 0); player demux+video decode thread; audio decode thread (core 1);
download worker thread. All core state behind mutexes; UI polls snapshots each frame.

## Data on SD (`sdmc:/3ds/Jelly3DS/`)

- `config.json` — server URL, userId, token, deviceId, profile override, audio/sub language defaults, TLS verify flag
- `downloads/index.json` — list of DownloadItem (id, names, series/season/episode, runtime, profile, audio/sub index, status, bytes)
- `downloads/<itemId>/video.ts`, `thumb.jpg`
- `progress.json` — map itemId → {positionTicks, played, updatedAt(unix s), dirty}

## Sync rules

- Playback updates `progress.json` every 10 s, on pause, on stop, on sleep.
- When online (login check succeeds) the sync pass, for every dirty entry: fetch server `UserData`;
  if server `LastPlayedDate` is newer than local `updatedAt`, adopt the server value; otherwise
  `POST /Sessions/Playing/Stopped {ItemId, PositionTicks}` (Jellyfin marks played past 90 %), or
  `POST /UserPlayedItems/{id}` (fallback `/Users/{uid}/PlayedItems/{id}`) when played. Clear dirty on success.
- Non-dirty downloaded items get their position refreshed from the server (watch on TV, continue on 3DS).

## Player

- Local: AVFormat file input, seek with `av_seek_frame(BACKWARD)`; resume from saved position.
- Stream: curl → ring buffer → custom AVIOContext (non-seekable); seek = reopen with `StartTimeTicks`.
- Video: MVD path feeds Annex-B NAL units (3-byte start code) into `mvdstdProcessVideoFrame`, renders BGR565,
  copied into a 512-stride linear buffer and `GX_DisplayTransfer`ed to a tiled texture.
  SW path: FFmpeg h264 → YUV420P → Y2R (RGB565) → same texture upload.
- Audio is master clock; video frames late by >1 frame are dropped (SW path also sets `skip_frame=NONREF` when behind).
- Controls: A play/pause, ◀▶ ±10 s, L/R ±60 s, B stop (saves), X toggles bottom backlight (headphone mode), touch seek bar.
- End of episode: next downloaded episode of the same series auto-plays after a 5 s countdown (B cancels).

## Error handling

- Offline at launch → open Downloads directly; browse tabs show "offline".
- Download failures keep the item in Failed with message; A retries.
- Sleep is disallowed while a download runs; X turns backlights off instead.
- Player errors return to the previous screen with a message; progress is still saved.

## Testing

- doctest unit tests for URL building, index/progress persistence, sync decisions, queue transitions (host).
- Integration: Jellyfin in Docker with a generated test series; host CLI logs in, downloads with each
  profile; ffprobe verifies codec/profile/resolution/fps/burned subs; the host build of the demux path
  plays through the files.
- 3DS build produces `Jelly3DS.3dsx` and `Jelly3DS.cia`; on-device test by the user.
