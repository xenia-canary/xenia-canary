# Contributing to Kinectix

## Scope rules — read first

**The single most important rule:** if your change touches files outside this allowlist, the PR will be closed and asked to be redirected to xenia-canary upstream:

- `src/xenia/hid/nui/**`
- `src/xenia/kernel/xam/xam_nui.cc`
- `src/xenia/kernel/xam/xam_nui.h`
- `.github/**`
- top-level `*.md` (docs)
- `tools/nui_*` (recorder, fixture utilities)
- premake / CMake additions for NUI build flags only

Anything else — GPU fixes, kernel bugs, audio, controller — belongs in [xenia-canary](https://github.com/xenia-canary/xenia-canary). We will happily help you draft that PR, but Kinectix does not carry private divergence outside its scope. Every byte of drift is rebase pain when we sync from upstream.

If you have a non-NUI bug fix that you've found while testing Kinectix, the workflow is:

1. Open the PR against xenia-canary first.
2. Once it's merged (or even queued for review), open a Kinectix issue linking it. We'll cherry-pick it locally so users don't have to wait, and it'll naturally come through on the next upstream sync.

## Commit message convention

All commits whose primary purpose is NUI-related must be prefixed:

```
[nui] Add INuiBackend interface and NuiManager skeleton

Introduces the abstract backend interface used by xam_nui.cc to fetch
skeleton/depth/color frames. Provides the NuiManager singleton and a
no-op default backend.

Refs: #12
```

Bug fixes destined for upstream use the prefix the upstream uses (no `[nui]`):

```
GPU: Fix incorrect tile size in resolve path
```

## Branch model

- `canary` — automated mirror of `xenia-canary/xenia-canary`'s `canary_experimental` branch. Do not push directly.
- `main` — protected. PRs only. Rebased on `canary` weekly by `.github/workflows/canary-sync.yml`.
- `feature/nui-<short-name>` — your working branch. PR'd to `main`.

Note: the `upstream` name in this repo refers to the **remote** pointing at `xenia-canary/xenia-canary` (used for `git fetch upstream`), not a branch. The mirror branch is `canary`.

If you fork from `main` and the weekly canary rebase happens before your PR merges, you'll need to rebase your branch. This is normal. Keep features small to minimize the pain.

## Code style

We inherit xenia's style guide verbatim — see [xenia-canary's style notes](https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/docs). Run clang-format with the project's `.clang-format` before opening a PR. PRs that don't format-clean will get a comment, not a close.

## Tests

Every PR that changes behavior must ship one of:

- A unit test in `src/xenia/hid/nui/tests/` exercising the new path.
- A `.xnuirec` fixture in `tests/fixtures/` plus a regression test that loads it and asserts on observed XAM NUI output.
- Both, ideally.

We treat the recorded backend as our reference oracle: a `.xnuirec` fixture captured from a real Kinect, replayed through the recorded backend, must produce the same XAM NUI guest-side state every run on every host. CI runs the full fixture suite on every push.

If your change touches a backend that requires hardware (libfreenect, libfreenect2, MediaPipe), you must additionally:

1. Document the manual test in the PR description ("ran with Kinect v1 against Kinect Adventures, walked the menu, captured `kinect_adventures_menu.xnuirec`").
2. Attach the `.xnuirec` capture as a CI fixture if it's small enough.

## Bringing in a new backend

If you want to add a new `INuiBackend` (say, OpenXR body tracking, or VR trackers):

1. Open a discussion issue first. Backend additions expand our maintenance surface; we want to agree on it before code.
2. The backend must be optional, gated by a build flag (`KINECTIX_NUI_<NAME>=ON|OFF`).
3. The default build with no flags must still link and produce a working binary that uses the recorded backend.
4. The backend cannot pull in dependencies that change xenia-canary's build assumptions (no new global LDFLAGS, no replacing CRTs, etc.).

## Branding and trademarks

- Do not use the Xenia name or logo in PRs, screenshots, or branches. Use "Kinectix" or "this fork".
- Do not refer to the project as "Xenia with Kinect" in commit messages, issue titles, or release notes.
- Do not link to upstream Xenia issues from Kinectix marketing material in a way that suggests endorsement.
- If a maintainer of upstream Xenia asks us to change something, default to yes. We are guests in their ecosystem.

## Code of conduct

Be kind. We are working on motion control emulation for fifteen-year-old games. Take it seriously, but not too seriously.

## Who reviews what

- Backend changes (`hid/nui/recorded`, `hid/nui/freenect*`, `hid/nui/mediapipe`): any maintainer.
- `xam_nui.cc` and the `INuiBackend` interface itself: requires two maintainer LGTMs because contract changes propagate.
- `.github/workflows/canary-sync.yml` and other CI: requires a maintainer LGTM and a known-good run on a fork before merge.

## Release process

(TODO once we cut v0.1.) Sketch:

1. Tag from `main` after a passing CI run with all NUI fixtures green.
2. CI builds Windows and Linux binaries.
3. GitHub Release with changelog (auto-generated from `[nui]` prefixed commits since last tag).
4. Binaries named `kinectix-<version>-<platform>.zip`, never `xenia-*`.
