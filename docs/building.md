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

# Run premake and opens IDE:
# Defaults to Visual Studio on Windows and CLion on Linux.
xb devenv

# Opens desired IDE:
xb devenv --ide=vs
xb devenv --ide=clion

# Run premake to update the sln/vcproj's:
xb premake

# Format code to the style guide:
xb format

# Build and run the unit tests:
xb test
```

#### Build Tools only (no Visual Studio IDE)

The standalone Build Tools provide the MSVC compiler and Windows SDK that
`xb` and CLion need, without the Visual Studio IDE:

1. Download **Build Tools for Visual Studio** from the
   [Visual Studio downloads page](https://visualstudio.microsoft.com/downloads/)
   (under "Tools for Visual Studio") and run the installer as admin.
2. Select the **Desktop development with C++** workload and install.
3. Check that `VC\Tools\MSVC` exists under the install path (default
   `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools` for VS 2026),
   then run `xb setup` in a fresh terminal.

Or from an admin PowerShell:

```powershell
winget install --id Microsoft.VisualStudio.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

If winget says the package is already installed, it skips the installer and
never adds the workload. Modify the existing install instead (adjust
`--installPath` to your version and location):

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --norestart
```

Switching between the VS IDE and Build Tools, or a VS update that changes the
MSVC version directory, can leave a stale compiler path in
`build/CMakeCache.txt`. If configure then fails with a missing compiler,
delete `build/CMakeCache.txt` and `build/CMakeFiles` and configure again.
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
Multi-Config in `build/`) and opens the project folder in CLion, which
imports the presets in `CMakePresets.json`. Then build/run the `xenia-app`
target. With only Build Tools installed, plain `xb devenv` picks CLion.
The launcher (`clion64` or `clion`) must be on PATH; otherwise pass it with
`--clion-path="C:\Program Files\JetBrains\CLion\bin\clion64.exe"` or open the
folder manually.

CLion must use an MSVC toolchain (MinGW is not supported); it detects a
Build Tools install as a "Visual Studio" toolchain. If CLion reports
"Cannot locate vcvarsall.bat" or no toolchain after a VS install was removed
or added, go to Settings > Build, Execution, Deployment > Toolchains, remove
the old Visual Studio entry and add (+) a new one, then reload.
`xb devenv --ide=clion` warns when the configured toolchain looks stale.

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
