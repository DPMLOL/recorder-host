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

`patches/0001` renames graphics-hook's kernel objects (`DPMCaptureHook_*`) and its ProgramData folder
(`dpm-recorder-hook`), so our hook never collides with an OBS Studio installed on the same machine.

## Protocol

One JSON object per line. stdout carries only protocol lines; libobs logs go to stderr
(`RECORDER_HOST_DEBUG=1` adds debug logs). Closing stdin shuts the host down.

Commands (`id` is echoed back on replies and errors):

| cmd | params |
|---|---|
| `info` | – |
| `start` | `path`, `video: {source: "game" \| "monitor", window?, width?, height?, fps?, bitrateKbps?, encoder?}`, `audio: {game?: {window?}, mic?: {deviceId?}}` |
| `stop` | – |
| `shutdown` | – |

`window` uses OBS's `title:class:exe` form and defaults to League's game window. `deviceId` comes from `info.inputs`.
Audio track 1 is the game (process loopback, any output device), track 2 the microphone.

Events: `ready`, `info`, `starting`, `started`, `stopped {code, error?}`, `hooked {title, class, executable}`,
`unhooked`, `anchor`, `error {cmd, id, message}`.

### Anchor

`anchor {kind: "first" | "check", ptsSec, wallMs, dtsWallMs}` maps file time to wall clock (Unix ms):
`wall(t) = wallMs + (t - ptsSec) * 1000`. It is sent on the first video keyframe, then every 30 keyframes (~60 s) as a drift check.
`wallMs` is the frame's composition time (`os_gettime_ns`, QPC) converted with a clock pair sampled at conversion time.

Measured 2026-09-28 (monitor capture, 6 flashes at logged times): mapped times land +20 to +36 ms late, i.e. 1–2 frames of
display/capture latency, with no drift over the recording.
