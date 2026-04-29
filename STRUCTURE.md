# Source structure (proposed)

This is the file-level layout we'll create inside the forked xenia-canary tree. Files marked **NEW** are added by Kinectix; files marked **MODIFIED** are inherited from xenia-canary and changed.

```
src/xenia/
├── hid/
│   └── nui/                                              [NEW directory]
│       │
│       ├── nui_backend.h                                 [NEW]
│       │   # The INuiBackend abstract interface.
│       │   # SkeletonJoint, SkeletonFrame, DepthFrame, ColorFrame structs.
│       │   # All backends inherit from INuiBackend.
│       │
│       ├── nui_manager.h                                 [NEW]
│       ├── nui_manager.cc                                [NEW]
│       │   # Singleton owning the active backend.
│       │   # Reads --nui_backend CLI flag, instantiates concrete backend.
│       │   # Thin facade: ActiveBackend(), Reconnect(), Shutdown().
│       │   # Translates host timestamps to guest timestamps.
│       │
│       ├── nui_constants.h                               [NEW]
│       │   # Joint indices (NUI_SKELETON_POSITION_HEAD = 3, etc.) matching
│       │   # the Kinect SDK header constants. Skeleton count limits.
│       │   # Capability flag bitfields.
│       │
│       ├── recorded/                                     [NEW directory]
│       │   ├── recorded_backend.h                        [NEW]
│       │   ├── recorded_backend.cc                       [NEW]
│       │   │   # Implements INuiBackend by streaming frames from a .xnuirec
│       │   │   # file. Synchronizes playback to guest frame count, not host
│       │   │   # wall clock — required for deterministic CI.
│       │   │
│       │   ├── xnuirec.fbs                               [NEW]
│       │   │   # Flatbuffer schema for the recording format.
│       │   │   # Generates xnuirec_generated.h at build time.
│       │   │
│       │   ├── xnuirec_reader.h                          [NEW]
│       │   ├── xnuirec_reader.cc                         [NEW]
│       │   │   # mmap-based reader. Optionally decompresses .xnuirec.zst.
│       │   │   # Validates header magic ("XNUI") and format_version.
│       │   │
│       │   ├── xnuirec_writer.h                          [NEW]
│       │   └── xnuirec_writer.cc                         [NEW]
│       │       # Used by tools/nui_record to author files from a live
│       │       # backend. Streaming writer; flushes frames on cadence.
│       │
│       ├── freenect/                                     [NEW directory, build-flagged]
│       │   ├── freenect_backend.h                        [NEW]
│       │   ├── freenect_backend.cc                       [NEW]
│       │   │   # Kinect v1 via libfreenect.
│       │   │   # Producer thread: libfreenect callbacks → triple-buffered
│       │   │   # frame slots. Skeleton tracking via MediaPipe-on-RGB
│       │   │   # initially (see ROADMAP Stage 2).
│       │   │
│       │   ├── freenect2_backend.h                       [NEW]
│       │   ├── freenect2_backend.cc                      [NEW]
│       │   │   # Kinect v2 via libfreenect2.
│       │   │   # Same shape as v1 backend; 25→20 joint mapping inline.
│       │   │
│       │   └── skeleton_tracker.h                        [NEW]
│       │       # Shared skeleton tracking logic (whichever approach we
│       │       # land — pluggable stub initially).
│       │
│       ├── mediapipe/                                    [NEW directory, build-flagged]
│       │   ├── mediapipe_backend.h                       [NEW]
│       │   ├── mediapipe_backend.cc                      [NEW]
│       │   │   # Webcam capture (cv::VideoCapture or platform-native) →
│       │   │   # MediaPipe Pose → 33 landmarks → 20-joint Kinect skeleton.
│       │   │   # Synthesizes fake depth (monocular estimation).
│       │   │
│       │   └── pose_to_skeleton.cc                       [NEW]
│       │       # MediaPipe-to-Kinect joint mapping. Math-heavy but isolated.
│       │
│       ├── tests/                                        [NEW directory]
│       │   ├── recorded_backend_test.cc                  [NEW]
│       │   ├── nui_manager_test.cc                       [NEW]
│       │   ├── xnuirec_roundtrip_test.cc                 [NEW]
│       │   ├── joint_mapping_test.cc                     [NEW]
│       │   │   # 25-joint v2 → 20-joint v1 mapping correctness.
│       │   │   # 33-landmark MediaPipe → 20-joint correctness.
│       │   │
│       │   └── fixtures/                                 [NEW directory]
│       │       ├── kinect_adventures_idle.xnuirec        [NEW, hand-authored]
│       │       ├── kinect_adventures_wave.xnuirec        [NEW, hand-authored]
│       │       ├── fruit_ninja_kinect_swipe.xnuirec      [NEW, hand-authored]
│       │       └── empty_session.xnuirec                 [NEW, no skeletons]
│       │
│       └── premake5.lua                                  [NEW]
│           # Build configuration. Conditionally includes freenect/,
│           # mediapipe/ subdirs based on KINECTIX_NUI_* flags.
│
├── kernel/xam/
│   ├── xam_nui.cc                                        [MODIFIED]
│   │   # Currently in upstream: stubs returning E_NUI_NOTINITIALIZED.
│   │   # Rewritten to: route XAM NUI syscalls to NuiManager.
│   │   # Map function ordinals to INuiBackend method calls.
│   │   # Translate guest-side struct layouts ↔ INuiBackend types.
│   │
│   └── xam_nui.h                                         [MODIFIED?]
│       # If exists upstream, may need extended declarations.
│
└── kernel/xam/
    └── xam_module.cc                                     [MODIFIED, minimal]
        # Register the new exported NUI functions if Stage 0 telemetry
        # reveals any not currently registered.

tools/                                                    [NEW directory at repo root]
├── nui_record/
│   ├── main.cc                                           [NEW]
│   │   # CLI: kinectix-record --backend=freenect --out=session.xnuirec
│   │   # Captures from a live backend, writes .xnuirec.
│   └── premake5.lua                                      [NEW]
│
├── nui_inspect/
│   ├── main.cc                                           [NEW]
│   │   # CLI: kinectix-inspect session.xnuirec
│   │   # Dumps header, frame count, sample frames as ASCII.
│   └── premake5.lua                                      [NEW]
│
└── nui_synthesize/                                       [optional]
    ├── main.cc                                           [NEW, optional]
    │   # CLI: kinectix-synthesize --pose=idle --duration=30 --out=...
    │   # Hand-author fixtures from named pose presets.
    └── premake5.lua                                      [NEW, optional]

docs/                                                     [NEW or extended]
├── XAM_NUI_API_OBSERVED.md                               [NEW, populated Stage 0]
│   # Which XAM NUI functions reference titles call.
│
├── telemetry/                                            [NEW]
│   ├── kinect_adventures.csv                             [NEW, Stage 0]
│   ├── fruit_ninja_kinect.csv                            [NEW, Stage 0]
│   └── ...
│
├── getting-started.md                                    [NEW]
│   # User-facing setup guide.
│
├── compat.md                                             [NEW]
│   # Game compatibility matrix per backend.
│
└── architecture/
    ├── skeleton_tracking.md                              [NEW]
    │   # Why we chose MediaPipe-on-RGB for v1 skeleton synthesis.
    └── timestamps.md                                     [NEW]
        # Guest-vs-host clock translation, why playback is deterministic.

.github/
├── workflows/
│   ├── upstream-sync.yml                                 [NEW]
│   │   # Cron-triggered weekly. Fetches xenia-canary master, attempts
│   │   # rebase of `main` onto fresh `upstream`. Opens a PR on success;
│   │   # opens an alert issue on conflict.
│   │
│   ├── ci.yml                                            [NEW]
│   │   # Build matrix (Linux/Windows × backend flags).
│   │   # Runs unit tests and fixture replays.
│   │
│   └── release.yml                                       [NEW]
│       # On tag push, builds release binaries and creates GitHub Release.
│
├── ISSUE_TEMPLATE/
│   ├── bug-report.yml                                    [NEW]
│   ├── feature-request.yml                               [NEW]
│   └── new-backend-proposal.yml                          [NEW]
│
└── PULL_REQUEST_TEMPLATE.md                              [NEW]

README.md                                                 [NEW, replaces upstream]
ARCHITECTURE.md                                           [NEW]
ROADMAP.md                                                [NEW]
CONTRIBUTING.md                                           [NEW]
STRUCTURE.md                                              [NEW, this file]
LICENSE                                                   [INHERITED, BSD 3-Clause]
```

## Summary of file count

- **NEW**: ~45 files of code, schema, docs, CI; plus 4+ test fixtures.
- **MODIFIED from upstream**: 1–3 files (`xam_nui.cc`, possibly `xam_nui.h`, `xam_module.cc`).
- **DELETED**: 0 — we don't remove upstream code.

The discipline of touching almost no upstream files is what keeps the weekly rebase tractable. Every additional `[MODIFIED]` line in this document is a future-merge-conflict liability. Resist the urge to "fix this little thing while I'm here" inside Kinectix — fix it upstream instead.

## What's not in this layout

- No vendored `third_party/libfreenect/` etc. We use system or `find_package` for backend libraries. If we ever need to vendor (because a distro ships an old version), we add `third_party/nui/` rather than touching the existing `third_party/`.
- No `xam_nui_constants.cc` — those constants live in `nui_constants.h` as `constexpr`.
- No "experimental" backend directory. New backends go through the discussion-issue process per CONTRIBUTING.md and either land in `hid/nui/` proper or stay out.
