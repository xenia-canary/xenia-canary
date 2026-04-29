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

To enable a flag at premake time:

```bash
./xb premake --KINECTIX_NUI_FREENECT
./xb build
```

The recorded backend (replay from `.xnuirec` files) is **always built** and requires no external dependencies.

## Runtime flags

```
--nui_backend=none|recorded|freenect|freenect2|mediapipe   (default: none)
--nui_record_path=<path-to-.xnuirec>                       (only for recorded backend)
```

A backend selected via `--nui_backend` that wasn't built into this binary falls back to `none` with a warning in the log; it is not a fatal error.

## Build status of the NUI tree

As of `v0.0.1-scaffolding`, the NUI code in `src/xenia/hid/nui/` is **not wired into the build**. The `premake5.lua` in that directory is correct, but it is not yet `include()`'d from `src/xenia/hid/premake5.lua`, so the kinectix NUI library does not yet link into the main emulator binary.

This is intentional — the scaffolding commit is meant to be a stable baseline before we start poking the build system. Wiring is tracked in the issue **"Stage 1: wire src/xenia/hid/nui/ into the build"** (or similar — see open issues).

When that issue lands, the build flags above start having effect. Until then, they are documentation for the future.

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
