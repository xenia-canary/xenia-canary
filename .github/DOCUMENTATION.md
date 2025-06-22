## Table of Contents
1. [Project Overview](#project-overview)
2. [Architecture Overview](#architecture-overview)
3. [Core Components](#core-components)
4. [Source Code Structure](#source-code-structure)
5. [Build System](#build-system)
6. [Third-Party Dependencies](#third-party-dependencies)
7. [Platform Support](#platform-support)
8. [Development Workflow](#development-workflow)

### Key Features
- **CPU Emulation**: PowerPC 970 (Xenon) processor emulation with JIT compilation
- **GPU Emulation**: Xenos GPU emulation with multiple backend support (D3D12, Vulkan)
- **Audio Processing**: XMA audio decoding and multiple audio backend support
- **File System**: Virtual file system supporting ISO, XEX, and STFS containers
- **Input Support**: Multiple input driver support (XInput, SDL, keyboard)
- **Memory Management**: Complete Xbox 360 memory layout emulation

## Architecture Overview

Xenia follows a modular architecture with clear separation between different subsystems:

```
┌─────────────────────────────────────────────────────────────┐
│                    Emulator Core                            │
├─────────────────────────────────────────────────────────────┤
│  CPU     │  GPU     │  APU     │  Kernel  │  Memory        │
│ (Xenon)  │ (Xenos)  │ (Audio)  │ (XboxOS) │ Management     │
├─────────────────────────────────────────────────────────────┤
│  VFS     │  HID     │  UI      │  Debug   │  Patcher       │
│(FileSystem)│(Input) │(Interface)│(Tools)  │(Game Fixes)    │
├─────────────────────────────────────────────────────────────┤
│              Platform Abstraction Layer                     │
│         (Windows, Linux, Android Support)                   │
└─────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. Emulator Core (`src/xenia/emulator.h/.cc`)

The main class that manages all subsystems:

**Key Features:**
- Initialize and coordinate all subsystems
- Manage emulator lifecycle (setup, launch, terminate)
- Handle game loading from various formats (XEX, ISO, STFS)
- Provide unified interface for all emulator operations
- Manage configuration and callbacks

**Important Methods:**
- `Setup()`: Initialize all subsystems with factory functions
- `LaunchPath()`: Launch games from file paths with format detection
- `LaunchXexFile()`: Launch XEX executables
- `LaunchDiscImage()`: Launch ISO disc images
- `LaunchStfsContainer()`: Launch STFS container files

### 2. Memory System (`src/xenia/memory.h/.cc`)

Emulates the Xbox 360's complex memory architecture:

**Memory Layout:**
- **Virtual Memory**: Guest virtual address space (0x00000000-0xFFFFFFFF)
- **Physical Memory**: 512MB system RAM mapping
- **Multiple Heaps**: Different memory regions with varying properties

**Key Features:**
- **Address Translation**: Guest ↔ Host address mapping
- **Memory Protection**: Page-level access control
- **MMIO Support**: Memory-mapped I/O for hardware registers
- **Physical Memory Callbacks**: Invalidation notifications for caches

**Heap Types:**
- `VirtualHeap`: Standard virtual memory allocation
- `PhysicalHeap`: Physical memory with 1:1 virtual mapping
- System heaps for kernel allocations

### 3. CPU Subsystem (`src/xenia/cpu/`)

Emulates the Xbox 360's triple-core PowerPC 970 (Xenon) processor:

#### CPU Architecture
- **Processor** (`processor.h/.cc`): Main CPU coordinator
- **Thread State** (`thread_state.h/.cc`): Per-thread execution context
- **Export Resolver** (`export_resolver.h/.cc`): Kernel function resolution

#### Code Execution
- **JIT Backend** (`backend/x64/`): Just-in-time compilation to x64
- **HIR** (`hir/`): High-level intermediate representation
- **PPC Decoder** (`ppc/`): PowerPC instruction decoding

#### Module Loading
- **XEX Module** (`xex_module.h/.cc`): Xbox executable format loader
- **ELF Module** (`elf_module.h/.cc`): ELF format support
- **Raw Module** (`raw_module.h/.cc`): Raw binary loading

### 4. GPU Subsystem (`src/xenia/gpu/`)

Emulates the Xbox 360's custom Xenos GPU:

#### Graphics Backends
- **D3D12** (`d3d12/`): Direct3D 12 backend (Windows)
- **Vulkan** (`vulkan/`): Vulkan backend (cross-platform)
- **Null** (`null/`): Headless backend for testing

#### Core Components
- **Command Processor** (`command_processor.h/.cc`): GPU command execution
- **Graphics System** (`graphics_system.h/.cc`): Main GPU coordinator
- **Shader Translator** (`shader_translator.h/.cc`): Shader conversion
- **Texture Cache** (`texture_cache.h/.cc`): Texture management
- **Render Target Cache** (`render_target_cache.h/.cc`): Framebuffer management

#### Shader Pipeline
- **DXBC Translator** (`dxbc_shader_translator.h/.cc`): DirectX bytecode output
- **SPIRV Translator** (`spirv_shader_translator.h/.cc`): SPIR-V output
- **Shader Compiler** (`shader_compiler_main.cc`): Offline shader compilation

### 5. Audio Subsystem (`src/xenia/apu/`)

Handles Xbox 360 audio processing:

#### Audio Backends
- **XAudio2** (`xaudio2/`): Windows native audio
- **SDL** (`sdl/`): Cross-platform audio via SDL2
- **NOP** (`nop/`): Silent audio backend

#### Audio Processing
- **XMA Decoder** (`xma_decoder.h/.cc`): Xbox Media Audio decoding
- **Audio System** (`audio_system.h/.cc`): Main audio coordinator
- **Audio Media Player** (`audio_media_player.h/.cc`): WMA/MP3 playback

### 6. Kernel Subsystem (`src/xenia/kernel/`)

Emulates the Xbox 360 operating system:

#### Kernel Modules
- **XAM** (`xam/`): Xbox Application Manager
- **XBDM** (`xbdm/`): Xbox Debug Manager
- **XboxKrnl** (`xboxkrnl/`): Core kernel functions

#### Kernel Objects
- **XThread** (`xthread.h/.cc`): Thread management
- **XEvent** (`xevent.h/.cc`): Event synchronization
- **XFile** (`xfile.h/.cc`): File I/O operations
- **XSocket** (`xsocket.h/.cc`): Network socket emulation

#### System Services
- **Kernel State** (`kernel_state.h/.cc`): Global kernel state
- **User Module** (`user_module.h/.cc`): User-mode module loading

### 7. Virtual File System (`src/xenia/vfs/`)

Provides unified file access across different storage formats:

#### Device Types (`devices/`)
- **Host Path Device**: Access to host filesystem
- **Disc Image Device**: ISO/XGD disc image support
- **STFS Device**: Secure Transient File System containers
- **Null Device**: Empty device for testing

#### Core Components
- **Virtual File System** (`virtual_file_system.h/.cc`): Main VFS coordinator
- **Device** (`device.h/.cc`): Base device interface
- **Entry** (`entry.h/.cc`): File/directory entries

### 8. Human Interface Devices (`src/xenia/hid/`)

Handles input from various sources:

#### Input Drivers
- **XInput** (`xinput/`): Xbox controller support (Windows)
- **SDL** (`sdl/`): Cross-platform controller support
- **WinKey** (`winkey/`): Keyboard input (Windows)
- **Skylander** (`skylander/`): Skylanders portal support

#### Core Components
- **Input System** (`input_system.h/.cc`): Input coordinator
- **Input Driver** (`input_driver.h`): Base driver interface

### 9. User Interface (`src/xenia/ui/`)

Provides windowing and graphics context management:

#### Graphics Backends
- **D3D12** (`d3d12/`): Direct3D 12 UI rendering
- **Vulkan** (`vulkan/`): Vulkan UI rendering

#### UI Components
- **Window** (`window.h/.cc`): Main application window
- **ImGui Integration** (`imgui_*.h/.cc`): Debug UI framework
- **Graphics Provider** (`graphics_provider.h`): Graphics context abstraction

### 10. Base Library (`src/xenia/base/`)

Provides cross-platform utilities and abstractions:

#### Platform Abstraction
- **Threading** (`threading.h/.cc`): Thread management
- **Memory** (`memory.h/.cc`): Memory allocation utilities
- **Filesystem** (`filesystem.h/.cc`): File system operations
- **Clock** (`clock.h/.cc`): High-resolution timing

#### Utilities
- **Logging** (`logging.h/.cc`): Structured logging system
- **String Utilities** (`string.h/.cc`): String manipulation
- **Math** (`math.h`): Mathematical utilities
- **Profiling** (`profiling.h/.cc`): Performance profiling

## Source Code Structure

```
xenia-canary/
├── src/xenia/                    # Main source code
│   ├── app/                      # Application entry points
│   │   ├── discord/              # Discord Rich Presence
│   │   ├── emulator_window.*     # Main emulator window
│   │   └── xenia_main.cc         # Application entry point
│   ├── apu/                      # Audio Processing Unit
│   ├── base/                     # Cross-platform utilities
│   ├── cpu/                      # CPU emulation
│   │   ├── backend/x64/          # x64 JIT backend
│   │   ├── compiler/             # Code compilation
│   │   ├── hir/                  # High-level IR
│   │   └── ppc/                  # PowerPC decoding
│   ├── debug/ui/                 # Debug interface
│   ├── gpu/                      # Graphics Processing Unit
│   │   ├── d3d12/                # Direct3D 12 backend
│   │   ├── vulkan/               # Vulkan backend
│   │   └── shaders/              # Shader resources
│   ├── hid/                      # Human Interface Devices
│   ├── kernel/                   # Xbox 360 OS emulation
│   │   ├── xam/                  # Xbox Application Manager
│   │   ├── xbdm/                 # Xbox Debug Manager
│   │   └── xboxkrnl/             # Core kernel
│   ├── patcher/                  # Game patching system
│   ├── tools/                    # Development tools
│   ├── ui/                       # User interface
│   ├── vfs/                      # Virtual File System
│   ├── config.*                  # Configuration management
│   ├── emulator.*                # Main emulator class
│   └── memory.*                  # Memory management
├── third_party/                  # External dependencies
├── docs/                         # Documentation
├── tools/                        # Build and development tools
├── assets/                       # Icons and resources
└── android/                      # Android port (experimental)
```

## Build System

Xenia uses **Premake5** as its build system generator:

### Build Configuration (`premake5.lua`)
- **Multi-platform**: Windows, Linux, Android support
- **Multiple Configurations**: Debug, Release, Checked
- **Compiler Support**: MSVC, Clang, GCC
- **Architecture**: x64 primary, ARM64 for Android

### Build Scripts
- **`xb` script**: Main build automation (Windows batch/PowerShell)
- **Cross-platform**: Handles setup, building, and development tasks

### Key Build Targets
- **xenia-app**: Main emulator executable
- **xenia-base**: Core utilities library
- **xenia-cpu**: CPU emulation library
- **xenia-gpu**: GPU emulation library
- **Various backends**: Platform-specific implementations

## Third-Party Dependencies

### Core Libraries
- **Capstone**: Disassembly framework
- **fmt**: String formatting library
- **ImGui**: Immediate mode GUI
- **SDL2**: Cross-platform multimedia
- **Vulkan**: Graphics API headers
- **xxHash**: Fast hashing algorithm

### Platform-Specific
- **Windows**: DirectX 12, XAudio2, WinAPI
- **Linux**: GTK3, X11, ALSA/PulseAudio
- **Android**: Android NDK, OpenSL ES

### Compression & Formats
- **zlib**: General compression
- **zstd**: Fast compression
- **snappy**: Fast compression
- **mspack**: Microsoft compression formats
- **pugixml**: XML parsing

### Development Tools
- **clang-format**: Code formatting
- **Google Style Guide**: Coding standards
- **RenderDoc**: Graphics debugging

## Platform Support

### Windows (Primary)
- **OS**: Windows 10/11 (64-bit)
- **Compiler**: Visual Studio 2019/2022
- **Graphics**: D3D12, Vulkan
- **Audio**: XAudio2, SDL2
- **Input**: XInput, SDL2, WinKey

### Linux (Experimental)
- **OS**: Modern Linux distributions
- **Compiler**: Clang 9+
- **Graphics**: Vulkan
- **Audio**: SDL2
- **Input**: SDL2
- **Dependencies**: GTK3, X11, Vulkan drivers

### Android (Experimental)
- **Target**: Android 7.0+ (API 24)
- **Architecture**: ARM64, x86_64
- **Graphics**: Vulkan
- **Audio**: OpenSL ES
- **Build**: Android NDK

## Development Workflow

### Getting Started
1. **Clone Repository**: `git clone https://github.com/xenia-canary/xenia-canary.git`
2. **Setup Environment**: `xb setup`
3. **Build Project**: `xb build`
4. **Run Emulator**: Execute built binary with game path

### Development Commands
- **`xb build`**: Build the project
- **`xb devenv`**: Open development environment
- **`xb format`**: Format code to style guide
- **`xb premake`**: Regenerate project files
- **`xb pull`**: Update and rebuild

### Code Style
- **C++20**: Modern C++ standard
- **Google Style**: Based on Google C++ Style Guide
- **clang-format**: Automated formatting
- **Naming**: snake_case for variables, PascalCase for classes

### Testing
- **Unit Tests**: Component-specific tests
- **Integration Tests**: Full system tests
- **GPU Tests**: Graphics-specific validation
- **Compatibility Testing**: Game-specific testing

### Debugging
- **Debug Builds**: Full debugging information
- **Profiling**: Built-in performance profiling
- **Logging**: Comprehensive logging system
- **Debug UI**: ImGui-based debug interface
- **RenderDoc**: Graphics debugging support

## Key Design Principles

1. **Accuracy**: Prioritize correctness over performance
2. **Modularity**: Clear separation of concerns
3. **Cross-platform**: Support multiple operating systems
4. **Maintainability**: Clean, well-documented code
5. **Performance**: Efficient emulation where possible
6. **Extensibility**: Plugin and patch system support

## Contributing Guidelines

1. **Follow Style Guide**: Use provided clang-format configuration
2. **Write Tests**: Include tests for new functionality
3. **Document Changes**: Update documentation for significant changes
4. **Performance**: Consider performance implications
5. **Compatibility**: Test with multiple games
6. **Code Review**: All changes require review