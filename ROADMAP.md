# Roadmap

A staged plan from "fork exists, nothing works" to "shipped Kinect support". Each stage is sized so a single contributor can ship it in 1–3 weeks of focused work.

## Stage 0 — Telemetry (1 week)

**Goal:** know exactly which XAM NUI functions reference titles call, in what order, with what arguments.

**Tasks:**

- Patch `xam_nui.cc` to log every entry: function name, ordinal, argument bytes, calling thread, current frame.
- Run the five reference titles (Kinect Adventures, Fruit Ninja Kinect, Kinect Sports, Dance Central, Kinect Fun Lab) for ~5 minutes each. Drive past the splash screen, into menus, into one round of gameplay if possible (will likely not get past the "no Kinect detected" check — that's fine, we want to see the detection path).
- Aggregate logs into `docs/telemetry/<title>.csv`: function, call count, top argument signatures.

**Deliverable:** a single document (`docs/XAM_NUI_API_OBSERVED.md`) listing every function we've seen called, sorted by call frequency. This is the spec we implement against.

**Why first:** every current proposal for Kinect emulation has been written from theoretical knowledge of XAM. We don't have empirical data on what the games actually use. This stage produces it. Without it, every interface guess we make is a coin flip.

**Risk:** games may bail out of NUI initialization so early that we see only 3–4 calls before they give up. Mitigation: implement minimum-viable stub responses (return success, return capabilities-supported flags) iteratively, re-run, capture the next batch of calls. Three iterations should peel back to actual gameplay.

## Stage 1 — Recorded backend + INuiBackend (2 weeks)

**Goal:** any contributor can run a reference title against a `.xnuirec` fixture and see motion gameplay on screen.

**Tasks:**

- Land `INuiBackend` interface in `src/xenia/hid/nui/nui_backend.h`.
- Land `NuiManager` singleton.
- Land `RecordedBackend` reading `.xnuirec` files.
- Define and land `xnuirec.fbs` schema.
- Write a CLI tool `tools/nui_record` (no-op stub for now; populated in Stage 2) and `tools/nui_inspect` (dumps a `.xnuirec` to console).
- Rewrite `xam_nui.cc` based on Stage 0 telemetry. Hook into `NuiManager`.
- Hand-author or synthesize at least one `.xnuirec` fixture (a 30-second loop of "person standing, idle pose"). This will not be visually convincing, but it will be enough to get past the initial NUI detection in games.

**Deliverable:** Kinect Adventures launches past the "Wave to begin" screen using a hand-authored fixture. CI runs the fixture in a regression test.

**Risk:** "person standing idle" may not be enough to satisfy detection — some games require an actual gesture (a wave) to advance. Mitigation: progressively richer hand-authored fixtures. Build a tiny GUI tool (or use an external blender export) to author multi-pose fixtures.

## Stage 2 — libfreenect backend (2 weeks)

**Goal:** users with a real Kinect v1 can plug it in and play.

**Tasks:**

- Land `FreenectBackend` in `src/xenia/hid/nui/freenect/`. Build-flagged behind `KINECTIX_NUI_FREENECT=ON`, default off.
- Vendor or `find_package` libfreenect. Document install instructions for Windows, Linux, macOS.
- Implement skeleton tracking. **Note:** libfreenect itself does NOT do skeleton tracking — it only delivers depth + RGB + IR. We need a skeleton tracker. Three options, in order of preference:
  - (a) Ship a minimal skeleton tracker as part of the backend. Random-forest classifier trained on the [Microsoft Kinect Body Pose Dataset](https://www.microsoft.com/en-us/research/publication/real-time-human-pose-recognition-in-parts-from-single-depth-images/) approach. **High effort, high quality.**
  - (b) Use OpenNI 2 + NiTE 2 if we can find a license-compatible source. NiTE was Microsoft proprietary; PrimeSense's library has been forked. **Medium effort, license-questionable.**
  - (c) Run MediaPipe Pose against the RGB stream, project to skeleton. **Low effort, lower quality, depends on MediaPipe build.**

  **Recommendation:** start with (c) for the v1 backend; treat (a) as a long-term project if anyone is interested. (c) gives us pixel-noisy but functional skeleton tracking with a path that already exists in Stage 3.

- Populate `tools/nui_record` to capture from libfreenect into `.xnuirec`. This is the path to building our test fixture corpus from real hardware.

**Deliverable:** `KINECTIX_NUI_FREENECT=ON` build runs Kinect Adventures end-to-end with a real Kinect plugged in. A captured `.xnuirec` from the same session replays correctly through the recorded backend.

**Risk:** Windows libfreenect support is fragile, especially on Windows 11. The Microsoft Kinect for Windows Runtime is deprecated and unreliable. Mitigation: prioritize Linux first; Windows users may need a `KINECTIX_NUI_KINECTSDK=ON` alternative path that uses the legacy MS SDK. Ship Linux support in this stage and treat Windows libfreenect as a follow-up.

## Stage 2b — libfreenect2 backend (2 weeks, parallel-able)

**Goal:** users with a Kinect v2 + USB 3.0 adapter can plug it in.

**Tasks:**

- Land `Freenect2Backend` in `src/xenia/hid/nui/freenect/`. Build-flagged behind `KINECTIX_NUI_FREENECT2=ON`.
- Vendor or `find_package` libfreenect2. Skeleton tracking same approach as Stage 2.
- Map v2's 25-joint skeleton to v1's 20-joint output by dropping `Neck`, `HandTipLeft`, `HandTipRight`, `ThumbLeft`, `ThumbRight`. (`Spine` and `SpineMid` collapse to `Spine` v1 sense.)

**Risk:** USB 3.0 adapter availability is shrinking. Document this clearly; v2 support is for users who already own the adapter.

## Stage 3 — Webcam + MediaPipe backend (optional, post Stage 2)

**Goal:** users with no Kinect at all can play with reduced fidelity.

**Tasks:**

- Land `MediaPipeBackend` in `src/xenia/hid/nui/mediapipe/`. Build-flagged behind `KINECTIX_NUI_MEDIAPIPE=ON`.
- Capture from `cv::VideoCapture` (or platform-native).
- Run MediaPipe Pose, get 33-point body landmarks.
- Project to Kinect v1 20-joint output. Many joints are direct mappings; some (`SpineShoulder`, `Spine`) are interpolations between MediaPipe landmarks.
- Synthesize fake depth from monocular pose estimation. Quality will be poor; depth-using games will probably not work well. Skeleton-using games should work acceptably.

**Risk:** quality. We document this as "best-effort, expect glitches in dance/precision titles, fine for casual stuff".

## Cross-stage tracks

These run alongside the staged work:

- **Game compatibility tracking.** A `compat.md` table mirroring xenia's game-compatibility repo, tracking which titles work with which backends.
- **Documentation.** User-facing docs at `docs/getting-started.md`, separate from contributor docs in this repo root.
- **Release cadence.** First tagged release after Stage 1 (recorded backend works, no hardware required). Second release after Stage 2 (real Kinect support).

## What we are NOT doing in v1.0

- XAUDIO2 microphone integration. Kinect mic array is a separate, large effort. Tracked separately as v2.0 scope.
- Azure Kinect SDK. EOL'd by Microsoft, libdepthengine is closed-source. Skip.
- VR tracker / SlimeVR / Vive proxy. Suggested in upstream issue #2302 but couples our project to VR ecosystems we don't want to maintain.
- Multi-Kinect setups (used in some research demos). Out of scope.
- Recording/replaying gameplay video. Out of scope; that's xenia's job, not ours.

## Done criteria for v1.0

- All five reference titles boot to playable state with the recorded backend and a per-game fixture.
- libfreenect Linux backend works end-to-end with at least Kinect Adventures and Fruit Ninja Kinect.
- All NUI code paths covered by CI tests using fixtures.
- Pre-built Linux + Windows binaries on GitHub Releases.
- Documented setup path for a new user.
- An average of less than one hour of upstream-rebase pain per week.
