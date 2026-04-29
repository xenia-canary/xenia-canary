# Architecture

This document describes how NUI emulation slots into Xenia's existing structure, the backend interface, and the on-disk recording format.

## Where NUI lives

Xenia's HLE (high-level emulation) handles peripherals through `src/xenia/hid/`. Kinectix adds a parallel subdirectory:

```
src/xenia/
├── hid/
│   ├── input_system.cc           # existing controller/HID dispatch
│   ├── ...
│   └── nui/                      # NEW
│       ├── nui_backend.h         # INuiBackend interface
│       ├── nui_manager.cc        # Singleton that owns the active backend
│       ├── nui_manager.h
│       ├── recorded/
│       │   ├── recorded_backend.cc
│       │   ├── recorded_backend.h
│       │   ├── xnuirec_reader.cc
│       │   ├── xnuirec_writer.cc
│       │   └── xnuirec.fbs       # flatbuffer schema (or .proto)
│       ├── freenect/
│       │   ├── freenect_backend.cc        # Kinect v1, build-flagged
│       │   ├── freenect_backend.h
│       │   ├── freenect2_backend.cc       # Kinect v2, build-flagged
│       │   └── freenect2_backend.h
│       ├── mediapipe/
│       │   ├── mediapipe_backend.cc       # webcam + ML, build-flagged
│       │   └── mediapipe_backend.h
│       └── tests/
│           ├── recorded_backend_test.cc
│           ├── nui_manager_test.cc
│           └── fixtures/
│               ├── kinect_adventures_idle.xnuirec
│               └── ...
└── kernel/xam/
    └── xam_nui.cc                # MODIFIED — routes XAM NUI syscalls to NuiManager
```

The choice of `hid/nui/` rather than `kernel/xam/nui/` is deliberate: the device backends are HID-shaped (they produce input state at a tick rate). The XAM NUI syscalls in `kernel/xam/xam_nui.cc` are the kernel-side surface that calls into `NuiManager` to fetch state, just as the existing controller code calls into `InputSystem`.

## INuiBackend interface

Sketch in `nui_backend.h`:

```cpp
namespace xe::hid::nui {

// 20-joint Kinect v1 skeleton. Index matches NUI_SKELETON_POSITION_*.
struct SkeletonJoint {
  float x;        // meters, sensor frame
  float y;
  float z;
  float confidence;  // 0.0..1.0; 0 means "not tracked"
};

struct SkeletonFrame {
  uint64_t timestamp_us;       // microseconds since session start
  uint32_t skeleton_index;     // 0..5; up to 6 skeletons tracked
  bool tracked;                // false = empty slot
  SkeletonJoint joints[20];
};

struct DepthFrame {
  uint64_t timestamp_us;
  uint16_t width;
  uint16_t height;
  std::vector<uint16_t> pixels;  // 11-bit depth in mm, packed in uint16_t
};

struct ColorFrame {
  uint64_t timestamp_us;
  uint16_t width;
  uint16_t height;
  std::vector<uint8_t> pixels;   // BGRA, row-major
};

class INuiBackend {
 public:
  virtual ~INuiBackend() = default;

  // Lifecycle
  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;
  virtual bool IsConnected() const = 0;

  // Capability flags — game queries these via XamNuiGetSkeletonCapabilities etc.
  virtual bool SupportsSkeleton() const = 0;
  virtual bool SupportsDepth() const = 0;
  virtual bool SupportsColor() const = 0;

  // Polling — returns latest frame or nullopt if no new data.
  // Called from xam_nui.cc on the game's clock, not host clock.
  virtual std::optional<SkeletonFrame> PollSkeleton(uint32_t index) = 0;
  virtual std::optional<DepthFrame> PollDepth() = 0;
  virtual std::optional<ColorFrame> PollColor() = 0;

  // Diagnostics
  virtual std::string Name() const = 0;
};

}  // namespace xe::hid::nui
```

`NuiManager` is a thin singleton owning one `INuiBackend`. Backend selection is via CLI flag `--nui_backend=recorded|freenect|freenect2|mediapipe|none`, default `none` (disabled).

## Recording format: `.xnuirec`

Goal: deterministic playback fixture for tests, and a way for users to share captured Kinect sessions.

**Format choice:** flatbuffers, not protobuf. Reasons:
- Xenia already vendors flatbuffers (used elsewhere in the codebase). No new dependency.
- Zero-copy access matters when streaming long recordings.
- Schema evolution semantics are sufficient for our needs.

Schema sketch (`xnuirec.fbs`):

```
namespace Kinectix.Xnuirec;

table Header {
  format_version: uint = 1;
  device_kind: string;        // "kinect_v1", "kinect_v2", "synthetic"
  sample_rate_hz: float;      // typically 30.0
  recording_duration_us: uint64;
  recorded_at_iso8601: string;
  game_title_id: uint = 0;    // optional, e.g. 0x4D5307D5 for Kinect Adventures
  comment: string;
}

table Joint {
  x: float;
  y: float;
  z: float;
  confidence: float;
}

table Skeleton {
  skeleton_index: uint;
  tracked: bool;
  joints: [Joint];            // length 20 if tracked, empty otherwise
}

table Frame {
  timestamp_us: uint64;
  skeletons: [Skeleton];
  depth_pixels: [ushort];     // optional; empty if not recorded
  depth_width: ushort = 0;
  depth_height: ushort = 0;
  color_pixels: [ubyte];      // optional; BGRA, empty if not recorded
  color_width: ushort = 0;
  color_height: ushort = 0;
}

table Recording {
  header: Header;
  frames: [Frame];
}

root_type Recording;
file_identifier "XNUI";
```

Color/depth pixels are heavyweight — we make them optional per-frame so test fixtures can be skeleton-only. A full RGB+depth recording at 30 fps for 60 seconds is roughly 4 GB; skeleton-only is sub-MB.

**Compression:** for distribution, `.xnuirec.zst` (zstd) is acceptable. The format itself is uncompressed; the loader transparently decompresses if it sees a `.zst` extension.

## XAM NUI integration

The existing `xam_nui.cc` in upstream Xenia is mostly empty stubs. Kinectix rewrites it to:

1. On Xbox guest call to e.g. `XamNuiGetSkeletonCapabilities`, return capabilities derived from `NuiManager::ActiveBackend()`.
2. On `XamNuiSkeletonGetNextFrame` (or whatever the canonical name is — confirmed during Stage 0), call `backend->PollSkeleton(index)` and translate the result into the guest-side struct layout.
3. Maintain a mapping table from XAM NUI function ordinals to `INuiBackend` methods.

The exact list of XAM NUI functions called by reference titles is unknown until Stage 0 telemetry runs. The interface above is sized to plausibly cover them, but we expect to grow it.

## Threading

Backends run their own producer thread (e.g. libfreenect callback thread). They write into a triple-buffered slot per frame type. `Poll*()` reads the latest committed slot lock-free. Game thread never blocks on Kinect I/O.

## Game clock vs host clock

XAM NUI APIs return timestamps on the guest clock. Backends produce frames on the host clock. `NuiManager` translates: it samples `kernel_state->frame_count()` and the guest tick rate, computes a guest-relative timestamp for each backend frame as it arrives. Replay backends pre-advance based on the guest's frame count, so the same `.xnuirec` file produces identical state on every run regardless of host frame rate. **This is essential for deterministic CI tests.**

## Open architectural questions

1. **Multi-skeleton enumeration order.** Kinect v1 tracks up to 6 skeletons; only 2 are "active" with full joint data, 4 are "passive" (position-only). We need to confirm whether games depend on a specific stable index assignment or whether the OS shuffles them. Resolved during Stage 0.

2. **Calibration.** Real Kinect output is in sensor-relative meters. Recorded fixtures must capture sensor pose (height, tilt) so playback in different physical setups is consistent. Stored in `Header` — schema TODO.

3. **Audio.** Eventually XAUDIO2 microphone array integration will need a parallel `IAudioInputBackend`. Out of scope for v1, but the directory structure (`hid/nui/`, `hid/audio/` future) anticipates it.

4. **Hot-plug.** Backend disconnect (Kinect unplugged) — return `IsConnected() == false` and let `xam_nui.cc` translate to the appropriate error code. The exact error semantics are TBD.
