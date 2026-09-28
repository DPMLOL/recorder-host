# recorder-host

Headless OBS (libobs 32.2.2, no UI) that records League of Legends for the DPM desktop app.
DPM spawns `recorder-host.exe` as a child process and drives it over stdin/stdout.

GPL-2.0-or-later, because it links libobs. DPM itself only talks to it through pipes and links nothing.

## Build

Needs Visual Studio 2026 (C++ workload, Windows SDK 10.0.26100), git and node.

```
scripts\build.cmd            # build\obs-install\bin\64bit\recorder-host.exe
scripts\build.cmd --sign     # + signs graphics-hook, inject-helper, get-graphics-offsets and the host (DPM_SIGN_KEYPAIR)
```

The script initialises `obs-studio/` (submodule pinned to 32.2.2), applies `patches/`, builds OBS without
frontend/browser/websocket/scripting, builds the host, then copies the obs-deps DLLs the binaries really import.

`patches/0001-dpm.patch` numbers split files (`{seq}` in the file name format), refreshes the hook copy when its contents change, and renames graphics-hook's kernel objects (`DPMCaptureHook_*`) and its ProgramData folder
(`dpm-recorder-hook`), so our hook never collides with an OBS Studio installed on the same machine.

## Protocol

One JSON object per line. stdout carries only protocol lines; libobs logs go to stderr
(`RECORDER_HOST_DEBUG=1` adds debug logs). Closing stdin shuts the host down.

Commands (`id` is echoed back on replies and errors):

| cmd | params | reply |
|---|---|---|
| `info` | – | `info {obsVersion, inputs, outputs, encoders, defaultEncoder}` (inputs/outputs = Windows endpoint ids + names) |
| `audio_apps` | – | `audio_apps {apps: [{executable, pid, active}]}`: apps with an audio session on any output |
| `monitor` | `audio` | `monitoring`: opens the tracks with meters, writes nothing (the settings mixer) |
| `start` | `output: {segments: {directory, prefix, seconds}, vod?}`, `video`, `audio` | `starting {encoder, path, vod, width, height, fps, tracks}` |
| `set_volume` | `track`, `volume` (0–2) | `ok` |
| `loudness` | `sinceWallMs` | `loudness {loudnessLufs?, peakDb?}` of the mix since then |
| `stop` | – | `stopped {vod, loudnessLufs?, peakDb?}` once every output is closed |
| `shutdown` | – | – |

`video: {source: "game" | "monitor", window?, width?, height?, fps?, bitrateKbps?, encoder?}`; `window` uses OBS's
`title:class:exe` form and defaults to League's game window.

`audio: [{id, kind: "process", executable | window} | {id, kind: "input", deviceId}, volume?, mono?]`, 5 at most.
A process track captures the app's process tree wherever it plays (WASAPI process loopback). Inputs are downmixed to mono
unless `mono: false` (a mic on input 1 of a stereo interface would otherwise sit in the left ear only).

Stream layout, in the segments and the vod alike: video, then audio track 0 `mix` (every source at its volume, what
players play), then one track per source in `audio` order (kept for re-mixing). `set_volume` changes the mix live.

Segments: OBS splits on keyframes (GOP = `seconds`) into `{prefix}_000000.ts`, `{prefix}_000001.ts`, … and the host
sends `segment {file, startWallMs, endWallMs}` as each one closes, like a line of ffmpeg's segment list. The first
segment can be longer than `seconds`.

`vod` is a path for a Hybrid MP4 written as the game plays: readable even after a crash, and complete the moment `stop`
returns — no remux. If it cannot start, a `warning` is sent and the ring still records.

Other events: `ready`, `started`, `hooked {title, class, executable}`, `unhooked`, `levels {levels: {trackId: peakDb}}`
(4x/s while tracks are open), `output_stopped {output, code, error?}` (a non-zero code outside `stop` means the output
died), `anchor`, `warning {message}`, `error {cmd, id, message}`.

### Anchor

`anchor {kind: "first" | "check", ptsSec, wallMs, dtsWallMs}` maps file time to wall clock (Unix ms):
`wall(t) = wallMs + (t - ptsSec) * 1000`. It is sent on the first video keyframe, then every 30 keyframes (~60 s) as a drift check.
`wallMs` is the frame's composition time (`os_gettime_ns`, QPC) converted with a clock pair sampled at conversion time.

Measured 2026-09-28: 6 screen flashes at logged times land +20 to +36 ms late (1–2 frames of display/capture latency), and
in game the HUD clock's second boundaries land within 7–24 ms of the Live Client API game time. No drift over the recording.

### Loudness

The mix is measured live (ITU-R BS.1770-4: K-weighting, 400 ms blocks, absolute and relative gates) so a recording knows its
normalisation gain when it stops, and a clip can ask for its own window, without decoding anything again.
