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

## Build status of the NUI tree

As of the Stage 1 wiring commit, `xenia-hid-nui` is built and linked into `xenia-app` on every configuration. At default settings (`--nui_backend=none`) the null backend is installed, exposing no Kinect to the guest, matching upstream behavior.

`recorded_backend.cc` is built but uses a stub `XnuirecReader` — `Open()` always returns false. The real implementation requires flatbuffer codegen from `xnuirec.fbs`, which is a follow-up. Until then, `--nui_backend=recorded` reports the backend as disconnected at runtime.

`KINECTIX_NUI_FREENECT`, `KINECTIX_NUI_FREENECT2`, `KINECTIX_NUI_MEDIAPIPE` are declared as CMake options but enabling them currently only sets `XE_KINECTIX_NUI_*=1` defines and prints a STATUS message. Backend sources land in Stage 2/3.

## Common build issues

- **Lint failures on PR:** the `Lint` CI job runs `clang-format-20` on every C/C++ file. If your PR has formatting drift, run `./xb format --all` locally and commit the result. **Always use clang-format version 20 specifically** — minor versions of clang-format produce different output, and we don't want CI bouncing PRs that look fine to a contributor with a different version installed.
- **Premake errors mentioning `KINECTIX_NUI_*`:** the flag is gated on the option being declared; if you copy the build line from somewhere old, double-check the option name matches `src/xenia/hid/nui/premake5.lua`.
- **`xenia-hid-nui` link errors:** these will only appear once we wire the build (above). If you see them before that, you've gone off-script.

## How Kinectix differs from running stock xenia-canary

In practice, until Stage 1 wiring lands, Kinectix builds produce a binary that is **functionally identical** to xenia-canary's. The differences are in the source tree, not the runtime. Kinect titles will still report "no Kinect" and behave exactly as they do under upstream.

Once Stage 1 lands, the only runtime difference at default settings is one extra log line at startup:

```
i> NUI: kinectix: backend=null connected=no caps=0x0
```

…meaning the no-op backend is installed and Kinect-using titles still see no sensor — same as before. To get different behavior, pass `--nui_backend=recorded --nui_record_path=...` and a fixture file.
