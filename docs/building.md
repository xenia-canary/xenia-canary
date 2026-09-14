# Building

You must have a 64-bit machine for building and running the project. Always
run your system updater before building and make sure you have the latest
drivers.

## Setup

### Windows

* Windows 10 or later
* [Visual Studio 2022 or later](https://www.visualstudio.com/downloads/)
  or [CLion](https://www.jetbrains.com/clion/) with the standalone [Build Tools](#build-tools-only-no-visual-studio-ide)
  (compiler + SDK, no IDE needed)
* CMake 3.10+ (or C++ CMake tools for Windows)
* Windows 11 SDK version 10.0.22000.0 (for Visual Studio 2022, this or any newer version)
* [Python 3.6+ 64-bit](https://www.python.org/downloads/)
  * Ensure Python is in PATH.
* [Vulkan SDK](https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-sdk.exe)
  * The build script will automatically detect it if installed at `C:\VulkanSDK`

```
git clone https://github.com/xenia-canary/xenia-canary.git
cd xenia-canary
xb setup

# Build on command line (add --config=release for release):
xb build


# Pull latest changes, rebase, update submodules, and run premake:
xb pull

# Run premake and open the detected IDE (Visual Studio if installed,
# else CLion; run the 'xenia-app' project):
xb devenv

# Force an IDE (auto is the default; vs fails if only Build Tools exist,
# clion needs 'clion'/'clion64' on PATH or you open the folder manually):
xb devenv --ide=clion
xb devenv --ide=vs

# Use a CLion launcher that is not on PATH:
xb devenv --ide=clion --clion-path="C:\Program Files\JetBrains\CLion\bin\clion64.exe"

# Run premake to update the sln/vcproj's:
xb premake

# Format code to the style guide:
xb format

# Build and run the unit tests:
xb test
```

#### Build Tools only (no Visual Studio IDE)

You don't need the Visual Studio IDE — the standalone Build Tools provide
the MSVC compiler and Windows SDK that `xb` and CLion use:

1. Download **Build Tools for Visual Studio** from the
   [Visual Studio downloads page](https://visualstudio.microsoft.com/downloads/)
   (under "Tools for Visual Studio") and run the installer as admin.
2. Select the **Desktop development with C++** workload.
3. Install, then verify the C++ workload actually landed: the
   `VC\Tools\MSVC` folder should exist under the install path (default:
   `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` for 2026 —
   adjust `18` to your version or use your actual path).
   If it's missing, the `--add` flags were dropped — re-run the install
   command. Then open a fresh terminal and run `xb setup` — `xb` locates the
   Build Tools automatically, so the install location doesn't matter.

Command-line alternatives (admin terminal):

```
:: VS Build Tools
winget install --id Microsoft.VisualStudio.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

If winget reports the package already installed with no upgrade available,
it skips the installer and the workload is never added. Modify the existing
install instead (admin terminal):

```
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart
```
(adjust `--installPath` to your version/location — `18` is VS 2026).

CLion picks up a Build Tools install as a "Visual Studio" toolchain on its
own — nothing extra to configure beyond selecting it.

Switching between the VS IDE and Build Tools (or a VS update changing the
MSVC version directory) can orphan the compiler path in
`build/CMakeCache.txt` — if configure fails with a missing compiler error,
delete `build/CMakeCache.txt` and `build/CMakeFiles` and re-run configure.

If CLion fails to reload with "Cannot locate vcvarsall.bat" after removing
a VS installation, its cached toolchain is stale: Settings > Build,
Execution, Deployment > Toolchains > remove the old Visual Studio entry so
it redetects the current install (`xb devenv --ide=clion` warns about this
automatically).
CLion does not always redetect a new installation by itself — if reload then
reports a toolchain is not found, re-add it with the `+`
button (type Visual Studio), then reload.
<!--
# Remove intermediate files and build outputs (doesn't work on Linux):
xb clean

# Check for lint errors with clang-format:
xb lint

# Run the style checker on all code:
xb style

# Remove all build/ output and do a hard git reset:
xb nuke

# Runs the clang-tidy checker on all code:
xb tidy


## Testing:

# Generate tests:
xb gentests

# Run tests:
xb test

# Run GPU tests:
xb gputest


## Other:

# Generate SPIR-V binaries and header files:
xb genspirv
-->

#### CLion (Windows)

`xb devenv --ide=clion` runs the same configure as `xb build` (Ninja
Multi-Config in `build/`) and opens the project folder in CLion. If the
launcher is not on PATH, pass `--clion-path=<launcher>` or open the folder
manually. With only Build Tools installed (no `devenv.exe`), plain
`xb devenv` selects CLion automatically.

In CLion, the presets in `CMakePresets.json` are imported automatically.
Then build/run the `xenia-app` target.

Ensure CLion uses an MSVC (Visual Studio) toolchain: Settings > Build,
Execution, Deployment > Toolchains. The MinGW toolchain is not supported
on Windows — this project requires MSVC.

#### Debugging

VS behaves oddly with the debug paths. Open the 'xenia-app' project properties
and set the 'Command' to `$(SolutionDir)$(TargetPath)` and the
'Working Directory' to `$(SolutionDir)..\..`. You can specify flags and
the file to run in the 'Command Arguments' field (or use `--flagfile=flags.txt`).

By default logs are written to a file with the name of the executable. You can
override this with `--log_file=log.txt`.

If running under Visual Studio and you want to look at the JIT'ed code
(available around 0xA0000000) you should pass `--emit_source_annotations` to
get helpful spacers/movs in the disassembly.

### Linux

Linux support is extremely experimental and presently incomplete.

The build script uses Clang 19 or newer, and CI builds with Clang 20. GCC while
it should work in theory, is not easily interchangeable right now.

Clang builds against the system's GCC C++ standard library (libstdc++), which
must support C++20 `<format>`. That needs libstdc++ from GCC 13 or newer, so
distributions with an older default GCC, such as Debian 12 or Ubuntu 22.04,
can't build Xenia with their default toolchain. CI builds on Ubuntu 24.04.

* Normal building via `xb build` uses CMake+Ninja.
* Environment variables:
  Name  | Default Value
  ----- | -------------
  `CC`  | `clang`
  `CXX` | `clang++`

<!--* [CodeLite](https://codelite.org) is supported. `xb devenv` will generate a workspace and attempt to open it. Your distribution's version may be out of date so check their website.
* Experimental CMake generation is available to facilitate use of other IDEs such as [CLion](https://www.jetbrains.com/clion/). If `clion` is available inside `$PATH`, `xb devenv` will start it. Otherwise `build/CMakeLists.txt` needs to be generated by invoking `xb premake --devenv=cmake` manually.-->

Clang-19 or newer should be available from system repositories on all up to date distributions.
You will also need some development libraries. To get them on an Ubuntu system:

```sh
sudo apt-get install build-essential mesa-vulkan-drivers valgrind libc++-dev libc++abi-dev libgtk-3-dev liblz4-dev libudev-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libvulkan-dev libx11-xcb-dev clang-19 llvm-19 ninja-build
```

SDL3 is taken from the system when it is installed (`libsdl3-dev`, Ubuntu 25.04
or newer), and built from `third_party/SDL3` otherwise. That build needs the
ALSA, PulseAudio and PipeWire development packages above for sound, and
`libudev-dev` for controller hotplug.

In addition, you will need up to date Vulkan libraries and drivers for your hardware, which most distributions have in their standard repositories nowadays.

**Vulkan SDK (for shader compilation)**

The build uses `spirv-opt` and `glslangValidator` for shader compilation. Check
if your system version is recent enough:

```sh
spirv-opt --version
```

If the version is older than 2026.1, the system `spirv-tools` package will not
support all required options (e.g. `--canonicalize-ids`). In that case, install
the [Vulkan SDK from LunarG](https://vulkan.lunarg.com/sdk/home) and set
`VULKAN_SDK` so the build picks up the correct tools:

```sh
wget -qO vulkan-sdk.tar.xz https://sdk.lunarg.com/sdk/download/latest/linux/vulkan-sdk.tar.xz
mkdir -p ~/vulkan-sdk
tar -xf vulkan-sdk.tar.xz -C ~/vulkan-sdk
rm vulkan-sdk.tar.xz
export VULKAN_SDK=$HOME/vulkan-sdk/$(ls ~/vulkan-sdk)/x86_64
export PATH="$VULKAN_SDK/bin:$PATH"
```

Add the `export` lines to your shell profile to persist them across sessions.

## Running

To make life easier you can set the program startup arguments in your IDE to something like `--log_file=stdout /path/to/Default.xex` to log to console rather than a file and start up the emulator right away.
