# recorder-host

Headless OBS (libobs 32.2.2, no UI) that records League of Legends for the DPM desktop app.
DPM spawns `recorder-host.exe` as a child process and drives it over stdin/stdout.

GPL-2.0-or-later, because it links libobs. DPM itself only talks to it through pipes and links nothing.

## Build

Needs Visual Studio 2026 (C++ workload, Windows SDK 10.0.26100), git and node.

```
scripts\build.cmd            # build\obs-install\bin\64bit\recorder-host.exe
scripts\build.cmd --sign     # + signs graphics-hook, inject-helper, get-graphics-offsets and the host (DPM_SIGN_KEYPAIR)
scripts\build.cmd --sign --copy   # + assembles build\dist (what DPM ships) and copies it to DPM_APP_RECORDER_HOST_DIR
```

Clone with `git clone --recursive`, or let the script fetch the submodule. Signing is optional and needs your own
certificate: `--sign` calls DigiCert's `smctl` with the keypair alias in `DPM_SIGN_KEYPAIR`. An unsigned build records
the desktop fine, but Vanguard refuses an unsigned graphics hook, so game capture needs a signed one.

The script initialises `obs-studio/` (submodule pinned to 32.2.2), applies `patches/`, builds OBS without
frontend/browser/websocket/scripting, builds the host, then copies the obs-deps DLLs the binaries really import.

`patches/0001-dpm.patch` numbers split files (`{seq}` in the file name format), refreshes the hook copy when its contents change, and renames graphics-hook's kernel objects (`DPMCaptureHook_*`) and its ProgramData folder
(`dpm-recorder-hook`), so our hook never collides with an OBS Studio installed on the same machine.

## Protocol

One JSON object per line. stdout carries only protocol lines; libobs logs go to stderr
(`RECORDER_HOST_DEBUG=1` adds debug logs). Closing stdin stops the outputs as `stop` does (5 s limit, then forced, no
`stopped` reply) so the vod is finalised, then shuts the host down. `shutdown` does the same.

Commands (`id` is echoed back on replies and errors):

| cmd | params | reply |
|---|---|---|
| `info` | – | `info {protocol, obsVersion, inputs, outputs, encoders, defaultEncoder, features}` (inputs/outputs = Windows endpoint ids + names) |
| `audio_apps` | – | `audio_apps {apps: [{executable, pid, active}]}`: apps with an audio session on any output |
| `monitor` | `audio` | `monitoring`: opens the tracks with meters, writes nothing (the settings mixer) |
| `start` | `output: {segments?: {directory, prefix, seconds}, vod?}` (one or both), `video`, `audio`, `preview?`, `thumbnail?` | `starting {encoder, path, vod, width, height, fps, preview, thumbnail, tracks}` (`path` is the first segment, empty without segments; `preview` and `thumbnail` tell whether each is on) |
| `set_volume` | `track`, `volume` (0–2) | `ok` |
| `loudness` | `sinceWallMs` | `loudness {loudnessLufs?, peakDb?}` of the mix since then |
| `thumbnail` | `path?`, `width?`, `height?` (default: those of `start.thumbnail`, else 480x270) | `thumbnail {path, wallMs}` once the file is written; `error` "not recording" outside a recording |
| `stop` | – | `stopped {vod, endWallMs?, tiles?, loudnessLufs?, peakDb?}` once every output is closed (`tiles` with `preview`) |
| `shutdown` | – | – |

`info.protocol` is `2`. `info.features` lists what this host supports: `"vod-only"`, `"progress"`, `"encoder-settings"`,
`"preview"`, `"thumbnail"`.

`output` needs `segments`, `vod` or both; with neither, `start` fails with `nothing to record`. The app runs one at a time
(the vod in a game, the ring for a replay buffer); both together still work.

`video: {source: "game" | "monitor", window?, width?, height?, fps?, bitrateKbps?, encoder?, keyframeSec?, encoderSettings?}`;
`window` uses OBS's `title:class:exe` form and defaults to League's game window. `keyframeSec` (default 2) is the keyframe
interval without segments; with segments it is `segments.seconds`. `encoderSettings` is an object of libobs encoder
settings applied as given over the host's own `rate_control: "CBR"`, `bitrate` and `keyint_sec` (e.g. `{preset: "p5"}` for
NVENC, `{rate_control: "CRF", crf: 20}` for x264). The host does not interpret them; an encoder that refuses them fails `start`.

`audio: [{id, kind: "process", executable | window} | {id, kind: "input", deviceId}, volume?, mono?]`, 5 at most.
A process track captures the app's process tree wherever it plays (WASAPI process loopback). Inputs are downmixed to mono
unless `mono: false` (a mic on input 1 of a stereo interface would otherwise sit in the left ear only).

Stream layout, in the segments and the vod alike: video, then audio track 0 `mix` (every source at its volume, what
players play), then one track per source in `audio` order (kept for re-mixing). `set_volume` changes the mix live.

Segments: OBS splits on keyframes (GOP = `seconds`) into `{prefix}_000000.ts`, `{prefix}_000001.ts`, … and the host
sends `segment {file, startWallMs, endWallMs}` as each one closes, like a line of ffmpeg's segment list. The first
segment can be longer than `seconds`.

`vod` is a path for a Hybrid MP4 written as the game plays: readable even after a crash, and complete the moment `stop`
returns: no remux. If it cannot start, `start` fails; with segments, a `warning` is sent instead and the ring still records.

Events:

| event | fields | sent |
|---|---|---|
| `progress` | `endWallMs` | On every video keyframe after the first (every `keyframeSec`), on the vod when there is one, else on the ring. All video before `endWallMs` has been handed to the muxer as a complete fragment: how far the file reaches if the host dies, and the host's heartbeat while recording. |
| `stopped` | `endWallMs?` | Wall clock of the end of the video. For the vod: file time 0 plus the vod's video duration (`mp4_output` never writes a track's last packet). For the ring: one frame after its last picture. Absent if no video was written. |
| `preview` | `file, sheet, tiles, firstWallMs, intervalMs` | Each time a Sprite sheet is written (see below). |
| `thumbnail` | `path, wallMs` | The automatic Thumbnail of `start.thumbnail` was written (no `id`). |

Other events: `ready`, `started` (the first output is writing: the ring when there is one, else the vod), `hooked {title,
class, executable}`, `unhooked`, `levels {levels: {trackId: peakDb}}` (4x/s while tracks are open), `output_stopped
{output, code, error?}` (a non-zero code outside `stop` means the output died), `anchor`, `segment`, `warning {message}`,
`error {cmd, id, message}`.

### Preview tiles, Sprite sheets, Thumbnail

Every image the app shows of a recording is written while it records, so nothing is decoded afterwards.

`preview: {directory, prefix, tileWidth, tileHeight, intervalMs, columns, rows, flushTiles?}` (`flushTiles` default 5)
grabs one tile of the composited picture every `intervalMs` and packs the tiles, row by row, into JPEG Sprite sheets
`{directory}/{prefix}_000000.jpg`, `{prefix}_000001.jpg`, … of `columns` x `rows` tiles; cells not grabbed yet are black.
A sheet is rewritten every `flushTiles` tiles, when it is full, and once more before `stopped` if tiles were added since,
and each write sends `preview {file, sheet, tiles, firstWallMs, intervalMs}`: `file` is the sheet's path, `tiles` the
tiles grabbed since `start`, `firstWallMs` the composition time of tile 0 (the first frame after `start`) on the clock
of `anchor.wallMs`. Tile `k` is the first frame composited at or after `firstWallMs + k * intervalMs`: the cadence follows
the composition clock, and after lagged frames one frame fills every cell that was due. `stopped.tiles` is the final
count. Tiles stop when `stop` arrives, just before the outputs, so the last tile is inside the video.

`thumbnail: {path, width?, height?}` (default 480x270) writes one JPEG at the first frame after `hooked` (the first frame
for `monitor`) and sends `thumbnail {path, wallMs}`. The `thumbnail` command writes another one (same path and size unless
given) and replies `thumbnail {id, path, wallMs}`; `wallMs` is the composition time of the frame. If the file cannot be
written, the command gets an `error {cmd: "thumbnail"}` and the automatic one a `warning`.

Every image goes to `<file>.tmp`, then is renamed over `<file>`, so a reader never sees half a file. A failed write sends a
`warning` (the sheet is kept and written again at the next flush) and never stops the recording. The image keeps the
canvas aspect inside the tile or Thumbnail, letterboxed when they differ.

How: from libobs's main-rendered callback, on the graphics thread, the canvas (before the NV12 conversion) is drawn into
a chain of small render targets, halving it each pass (a 2x2 box filter per bilinear tap, in linear light) then scaling
to the target, and copied to a staging surface that is mapped one frame later, when the GPU is done with it. JPEG
encoding (the Windows Imaging Component, part of Windows) and file writes run on a worker thread. Measured 2026-11-10
on the monitor (3440x1440 into 1920x1080, NVIDIA GPU, NVENC): 0.05 ms of graphics-thread time per grab (0.6 ms for the first,
which creates the targets), 0.04 ms per readback, 3.2 ms per 1600x900 sheet, 2.6 ms per 480x270 Thumbnail, 33 ms from the
`thumbnail` command to its reply. A 1600x900 sheet of desktop tiles is about 70 KB.

### Anchor

`anchor {kind: "first" | "check", ptsSec, wallMs, dtsWallMs}` maps file time to wall clock (Unix ms):
`wall(t) = wallMs + (t - ptsSec) * 1000`. It is sent on the first video keyframe, then every 30 keyframes (~60 s at the
default 2 s interval) as a drift check. It is measured on the vod's own packets when there is a vod, else on the ring's,
so `t` is the vod's file time: the first anchor has `ptsSec` 0 and gives where the vod starts on the wall clock.
`wallMs` is the frame's composition time (`os_gettime_ns`, QPC) converted with a clock pair sampled at conversion time.

Measured 2026-09-28: 6 screen flashes at logged times land +20 to +36 ms late (1–2 frames of display/capture latency), and
in game the HUD clock's second boundaries land within 7–24 ms of the Live Client API game time. No drift over the recording.

### Loudness

The mix is measured live (ITU-R BS.1770-4: K-weighting, 400 ms blocks, absolute and relative gates) so a recording knows its
normalisation gain when it stops, and a clip can ask for its own window, without decoding anything again.
