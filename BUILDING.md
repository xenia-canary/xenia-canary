# Building Kinectix

The build process is **identical to upstream xenia-canary** — same `xb` script, same toolchain, same dependencies. See [`docs/building.md`](docs/building.md) (inherited from upstream) for the full setup.

This file documents only the **Kinectix-specific** additions on top of that.

## Quickstart (assumes you already have an xenia-canary build environment)

```bash
git clone https://github.com/RedMadKnight/Kinectix.git
cd Kinectix
./xb premake
./xb build
```

Output binaries land in the same place upstream puts them: `build/bin/<config>/`.

## Toolchain summary (Linux)

```bash
sudo apt install -y build-essential clang-format-20 git curl python3
```

## Toolchain summary (Windows)

- **Visual Studio 2022** with the "Desktop development with C++" workload
- **Windows 10 SDK 10.0.22621.0** or newer
- **Python 3.10+**
- **Git for Windows** (Git Bash recommended for our docs' shell snippets)

## NUI build flags

Kinectix adds optional backend flags. **None are required** — a default `xb build` produces a working binary that uses the no-op null backend. NUI code does not affect non-NUI titles.

| Flag | Default | Effect | External requirement |
|------|---------|--------|----------------------|
| `KINECTIX_NUI_FREENECT` | OFF | Builds the libfreenect (Kinect v1) backend | `libfreenect-dev` (Linux) / libfreenect from source (Windows) |
| `KINECTIX_NUI_FREENECT2` | OFF | Builds the libfreenect2 (Kinect v2) backend | `libfreenect2-dev` |
| `KINECTIX_NUI_MEDIAPIPE` | OFF | Builds the webcam + MediaPipe pose backend | `mediapipe`, `opencv` |

To enable a flag at CMake configure time:

```bash
cmake -S . -B build -DKINECTIX_NUI_FREENECT=ON
cmake --build build
```

(Or, via xenia-canary's `xb` wrapper which forwards `-D` to CMake under the hood.)

The recorded backend (replay from `.xnuirec` files) is **always built** and requires no external dependencies.

## Runtime flags

```
--nui_backend=none|recorded|freenect|freenect2|mediapipe   (default: none)
--nui_record_path=<path-to-.xnuirec>                       (only for recorded backend)
```

A backend selected via `--nui_backend` that wasn't built into this binary falls back to `none` with a warning in the log; it is not a fatal error.

## Telemetry — capturing real Kinect title call sequences

Stage 2 adds an opt-in tracer that logs every XAM NUI export entry made by a running title. We need this data to know which of the ~28 NUI functions real games actually call, in what order, and with what arguments — that's the input to writing real implementations rather than stubs.

```
--nui_telemetry   (default: off)
```

When on, every entry into a function in `src/xenia/kernel/xam/xam_nui.cc` emits a one-line `XELOGI` trace like:

```
i> [nui] XamNuiGetDeviceStatus(status_ptr=82A40C00)
i> [nui] XamNuiIsDeviceReady()
i> [nui] XamNuiHudSetEngagedTrackingID(id=00000001)
i> [nui] XamNuiSkeletonGetBestSkeletonIndex(unk=0)
```

Cost when off is one global-bool load + branch (predicted not-taken via `XE_UNLIKELY`) — leaving it compiled in for non-Kinect titles is harmless.

### Capturing a trace

1. Build a binary as usual.
2. Run a Kinect title with `--nui_telemetry --log_file=trace.log` (or whatever your usual log routing is).
3. Play through the part of the game that exercises NUI (sensor init, calibration, gameplay, identity prompt — pick one per session, narrower is more useful).
4. Filter the log: `grep "\[nui\]" trace.log > trace.nui.txt`.
5. Attach `trace.nui.txt` to the corresponding telemetry issue (one issue per title, see `Issues` on GitHub).

Note that `XamIsNuiAutomationEnabled` and `XamIsNatalPlaybackEnabled` are tagged `kHighFrequency` upstream — expect those to dominate the log volume. Don't strip them; their cadence is itself signal.

## Build status of the NUI tree

As of the Stage 1 wiring commit, `xenia-hid-nui` is built and linked into `xenia-app` on every configuration. At default settings (`--nui_backend=none`) the null backend is installed, exposing no Kinect to the guest, matching upstream behavior.

`recorded_backend.cc` is built but uses a stub `XnuirecReader` — `Open()` always returns false. The real implementation requires flatbuffer codegen from `xnuirec.fbs`, which is a follow-up. Until then, `--nui_backend=recorded` reports the backend as disconnected at runtime.

`KINECTIX_NUI_FREENECT`, `KINECTIX_NUI_FREENECT2`, `KINECTIX_NUI_MEDIAPIPE` are declared as CMake options but enabling them currently only sets `XE_KINECTIX_NUI_*=1` defines and prints a STATUS message. Backend sources land in Stage 2/3.

## Common build issues

- **Lint failures on PR:** the `Lint` CI job runs `clang-format-20` on every C/C++ file. If your PR has formatting drift, run `./xb format --all` locally and commit the result. **Always use clang-format version 20 specifically** — minor versions of clang-format produce different output, and we don't want CI bouncing PRs that look fine to a contributor with a different version installed.
- **CMake errors mentioning `KINECTIX_NUI_*`:** the flag is gated on the option being declared; if you copy the build line from somewhere old, double-check the option name matches `src/xenia/hid/nui/CMakeLists.txt`.
- **`xenia-hid-nui` link errors:** Stage 1 wired this library into `xenia-app` via `src/xenia/CMakeLists.txt` and `src/xenia/app/CMakeLists.txt`. If you hit unresolved symbols here on a fresh checkout, regenerate your build directory.

## How Kinectix differs from running stock xenia-canary

As of Stage 1, Kinectix builds produce a binary that is **functionally identical** to xenia-canary's at default settings — Kinect titles still report "no Kinect" and behave exactly as they do under upstream. The differences live behind cvars (`--nui_backend`, `--nui_telemetry`).

The only visible runtime difference at default settings is one extra log line at startup:

```
i> NUI: kinectix: backend=null connected=no caps=0x0
```

…meaning the no-op backend is installed and Kinect-using titles still see no sensor — same as before. To get different behavior, pass `--nui_backend=recorded --nui_record_path=...` and a fixture file.
