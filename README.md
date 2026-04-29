# Kinectix

**A fork of [xenia-canary](https://github.com/xenia-canary/xenia-canary) focused on Kinect / NUI peripheral emulation for Xbox 360 titles.**

> **Not affiliated with the Xenia project.** Kinectix is an independent fork released under the same BSD 3-Clause license as the upstream. The Xenia name and logo are not used in branding; the upstream binary name (`xenia.exe`) is preserved inside the build for compatibility but releases are distributed under the Kinectix name.

## Why this fork exists

Kinect support has been requested in upstream Xenia since 2018 (issues #1241, #2302, #2339; xenia-canary #537). The upstream project has explicitly chosen not to prioritize peripheral emulation — the [FAQ](https://github.com/xenia-project/xenia/wiki/FAQ) lists Kinect alongside mice and microphones as out-of-scope. Multiple maintainers have stated that the blocker is volunteer bandwidth and ongoing maintenance burden, not a hard technical problem.

Kinectix takes ownership of that scope. We track xenia-canary upstream weekly, contribute non-NUI bug fixes back where possible, and develop the NUI emulation layer here as first-class scope.

## Scope

**In scope:**

- XAM NUI HAL (`xam_nui.cc`) implementation for Xbox 360 Kinect (Kinect v1)
- Skeleton tracking pass-through and recorded playback
- libfreenect / libfreenect2 integration as optional backends
- Webcam + ML pose estimation as a fallback backend
- Tooling: skeleton recorder, player, fixture format

**Out of scope (for now):**

- XAUDIO2 microphone array (Kinect microphone) — separate effort
- Kinect Sensor v2 / Azure Kinect SDK paths beyond v2 USB 3.0 adapter
- VR tracker (SlimeVR / Vive) skeleton synthesis
- Any non-NUI feature work — those go to xenia-canary, not here

## Status

| Stage | Description | Status |
|------|-------------|--------|
| 0 | Telemetry: log all `xam_nui.*` syscalls from reference titles | not started |
| 1 | `INuiBackend` + recorded playback (`.xnuirec` files) | not started |
| 2 | libfreenect backend (Kinect v1, build-flag) | blocked by Stage 1 |
| 2b | libfreenect2 backend (Kinect v2 via USB 3.0 adapter) | blocked by Stage 1 |
| 3 | Webcam + MediaPipe pose estimation backend | optional, post Stage 2 |

See [ROADMAP.md](ROADMAP.md) for stage detail.

## Reference titles

Validation targets, in priority order:

1. **Kinect Adventures** — flagship bundled title, simplest skeleton interactions
2. **Fruit Ninja Kinect** — minimal API surface, gesture-driven
3. **Kinect Sports** — multiplayer, broader skeleton coverage
4. **Dance Central** — high-fidelity tracking, stress test for skeleton smoothing
5. **Kinect Fun Lab** — known to exercise edge-case NUI calls

## Branch model

- `upstream` — mirror of `xenia-canary/xenia-canary` master, updated weekly by [`upstream-sync.yml`](.github/workflows/upstream-sync.yml). Never commit here directly.
- `main` — integration branch. Rebased on `upstream` weekly. All code review lands here.
- `feature/nui-*` — working branches for in-progress NUI work, PR'd to `main`.
- `release/*` — tagged release branches with pre-built binaries.

**Rule:** any commit touching files outside `src/xenia/hid/nui/`, `src/xenia/kernel/xam/xam_nui.cc`, `.github/`, and top-level docs must be a bug fix. Bug fixes get cherry-picked into a PR against `xenia-canary` upstream within a week. We don't carry private divergence; it is rebase poison.

## Building

Same toolchain as xenia-canary. See [BUILDING.md](BUILDING.md) (TODO — inherits from upstream).

```bash
git clone https://github.com/<your-org>/Kinectix
cd Kinectix
./xb premake
./xb build
```

Optional NUI backends are gated by CMake / premake flags:

```bash
./xb build --define KINECTIX_NUI_FREENECT=ON
./xb build --define KINECTIX_NUI_FREENECT2=ON
./xb build --define KINECTIX_NUI_MEDIAPIPE=ON
```

Default backend is `recorded` (no external dependencies).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). The short version:

- Stay inside `src/xenia/hid/nui/` and `xam_nui.cc` unless you're fixing a bug we'll send upstream.
- Prefix NUI commits with `[nui]`.
- Do not use Xenia branding in PRs, screenshots, or release artifacts.
- All PRs must include either a unit test or a `.xnuirec` fixture demonstrating behavior.

## License

BSD 3-Clause, inherited from xenia-canary. See [LICENSE](LICENSE).

## Trademark notice

"Xbox", "Kinect", and "Xbox 360" are trademarks of Microsoft Corporation. "Xenia" is the name of an independent emulator project; Kinectix is a fork and is not endorsed by, affiliated with, or sponsored by the Xenia project or Microsoft. Kinectix does not distribute Microsoft software or game data.
