/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifdef __APPLE__
#ifndef _LIBCPP_HAS_QUICK_EXIT
#define _LIBCPP_HAS_QUICK_EXIT
#endif
#endif

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include "xenia/base/clock.h"
#include <thread>

#include "xenia/app/discord/discord_presence.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/assert.h"
#include "xenia/base/cpu_features_mac.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading_mac.h"
#include "xenia/config.h"
#include "xenia/debug/ui/debug_window.h"
#include "xenia/emulator_mac.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/window_mac.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app_mac.h"
#include "xenia/ui/windowed_app_context_mac.h"
#include "xenia/vfs/devices/host_path_device.h"

// Available audio systems:
#include "xenia/apu/nop/nop_audio_system.h"
#if !XE_PLATFORM_ANDROID
#include "xenia/apu/sdl/sdl_audio_system.h"
#endif  // !XE_PLATFORM_ANDROID
#if XE_PLATFORM_WIN32
#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#endif  // XE_PLATFORM_WIN32

// Available graphics systems:
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#if XE_PLATFORM_WIN32
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#endif  // XE_PLATFORM_WIN32

// Available input drivers:
#include "xenia/hid/nop/nop_hid.h"
#if !XE_PLATFORM_ANDROID
#include "xenia/hid/sdl/sdl_hid.h"
#endif  // !XE_PLATFORM_ANDROID
#if XE_PLATFORM_WIN32
#include "xenia/hid/winkey/winkey_hid.h"
#include "xenia/hid/xinput/xinput_hid.h"
#endif  // XE_PLATFORM_WIN32

DEFINE_string(apu, "any", "Audio system. Use: [any, nop, sdl, xaudio2]", "APU");
DEFINE_string(gpu, "any", "Graphics system. Use: [any, d3d12, vulkan, null]",
              "GPU");
DEFINE_string(hid, "any", "Input system. Use: [any, nop, sdl, winkey, xinput]",
              "HID");

DEFINE_path(
    storage_root, "",
    "Root path for persistent internal data storage (config, etc.), or empty "
    "to use the path preferred for the OS, such as the documents folder, or "
    "the emulator executable directory if portable.txt is present in it.",
    "Storage");
DEFINE_path(
    content_root, "",
    "Root path for guest content storage (saves, etc.), or empty to use the "
    "content folder under the storage root.",
    "Storage");
DEFINE_path(
    cache_root, "",
    "Root path for files used to speed up certain parts of the emulator or the "
    "game. These files may be persistent, but they can be deleted without "
    "major side effects such as progress loss. If empty, the cache folder "
    "under the storage root, or, if available, the cache directory preferred "
    "for the OS, will be used.",
    "Storage");

DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");

DEFINE_bool(mount_cache, true, "Enable cache mount", "Storage");
UPDATE_from_bool(mount_cache, 2024, 8, 31, 20, false);

DEFINE_bool(force_mount_devkit, false, "Force devkit mount", "Storage");

DEFINE_bool(log_cpu_verification, false, "Run CPU feature verification tests at startup", "Debug");
DEFINE_bool(log_cpu_features, true, "Log CPU feature detection at startup", "Debug");

DEFINE_transient_path(target, "",
                      "Specifies the target .xex or .iso to execute.",
                      "General");
#ifndef XE_PLATFORM_WIN32
DEFINE_transient_bool(portable, false,
                      "Specifies if Xenia should run in portable mode.",
                      "General");
#else
DEFINE_transient_bool(portable, true,
                      "Specifies if Xenia should run in portable mode.",
                      "General");
#endif

DECLARE_bool(debug);

DEFINE_bool(discord, true, "Enable Discord rich presence", "General");

DECLARE_bool(widescreen);

namespace xe {
namespace app {

class EmulatorApp final : public xe::ui::WindowedApp {
 public:
  static std::unique_ptr<xe::ui::WindowedApp> Create(
      xe::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<xe::ui::WindowedApp>(new EmulatorApp(app_context));
  }

  ~EmulatorApp();

  bool OnInitialize() override;

 protected:
  void OnDestroy() override;

 private:
  template <typename T, typename... Args>
  class Factory {
   private:
    struct Creator {
      std::string name;
      std::function<bool()> is_available;
      std::function<std::unique_ptr<T>(Args...)> instantiate;
    };

    std::vector<Creator> creators_;

   public:
    void Add(const std::string_view name, std::function<bool()> is_available,
             std::function<std::unique_ptr<T>(Args...)> instantiate) {
      creators_.push_back({std::string(name), is_available, instantiate});
    }

    void Add(const std::string_view name,
             std::function<std::unique_ptr<T>(Args...)> instantiate) {
      auto always_available = []() { return true; };
      Add(name, always_available, instantiate);
    }

    template <typename DT>
    void Add(const std::string_view name) {
      Add(name, DT::IsAvailable, [](Args... args) {
        return std::make_unique<DT>(std::forward<Args>(args)...);
      });
    }

    std::unique_ptr<T> Create(const std::string_view name, Args... args) {
      if (!name.empty() && name != "any") {
        auto it = std::find_if(
            creators_.cbegin(), creators_.cend(),
            [&name](const auto& f) { return name.compare(f.name) == 0; });
        if (it != creators_.cend() && (*it).is_available()) {
          return (*it).instantiate(std::forward<Args>(args)...);
        }
        return nullptr;
      } else {
        for (const auto& creator : creators_) {
          if (!creator.is_available()) continue;
          auto instance = creator.instantiate(std::forward<Args>(args)...);
          if (!instance) continue;
          return instance;
        }
        return nullptr;
      }
    }

    std::vector<std::unique_ptr<T>> CreateAll(const std::string_view name,
                                              Args... args) {
      std::vector<std::unique_ptr<T>> instances;

      // "Any" path
      if (name.empty() || name == "any") {
        for (const auto& creator : creators_) {
          if (!creator.is_available()) {
            continue;
          }

          // Skip xinput for "any" and use SDL
          if (creator.name.compare("xinput") == 0) {
            continue;
          }

          auto instance = creator.instantiate(std::forward<Args>(args)...);
          if (instance) {
            instances.emplace_back(std::move(instance));
          }
        }
        return instances;
      }

      // "Specified" path. Winkey is always added on windows.
      if (name != "winkey") {
        auto it = std::find_if(
            creators_.cbegin(), creators_.cend(),
            [&name](const auto& f) { return name.compare(f.name) == 0; });

        if (it != creators_.cend() && (*it).is_available()) {
          auto instance = (*it).instantiate(std::forward<Args>(args)...);
          if (instance) {
            instances.emplace_back(std::move(instance));
          }
        }
      }

      // Always add winkey for passthrough.
      auto it = std::find_if(
          creators_.cbegin(), creators_.cend(),
          [&name](const auto& f) { return f.name.compare("winkey") == 0; });
      if (it != creators_.cend() && (*it).is_available()) {
        auto instance = (*it).instantiate(std::forward<Args>(args)...);
        if (instance) {
          instances.emplace_back(std::move(instance));
        }
      }
      return instances;
    }
  };

  class DebugWindowClosedListener final : public xe::ui::WindowListener {
   public:
    explicit DebugWindowClosedListener(EmulatorApp& emulator_app)
        : emulator_app_(emulator_app) {}

    void OnClosing(xe::ui::UIEvent& e) override;

   private:
    EmulatorApp& emulator_app_;
  };

  explicit EmulatorApp(xe::ui::WindowedAppContext& app_context);

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(
      cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(
      ui::Window* window);

  void EmulatorThread();
  void ShutdownEmulatorThreadFromUIThread();

  DebugWindowClosedListener debug_window_closed_listener_;

  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<EmulatorWindow> emulator_window_;

  // Created on demand, used by the emulator.
  std::unique_ptr<xe::debug::ui::DebugWindow> debug_window_;

  // Refreshing the emulator - placed after its dependencies.
  std::atomic<bool> emulator_thread_quit_requested_;
  std::unique_ptr<xe::threading::Event> emulator_thread_event_;
  std::thread emulator_thread_;
  std::atomic<bool> emulator_launch_event_seen_{false};
};

void EmulatorApp::DebugWindowClosedListener::OnClosing(xe::ui::UIEvent& e) {
  EmulatorApp* emulator_app = &emulator_app_;
  emulator_app->emulator_->processor()->set_debug_listener(nullptr);
  emulator_app->debug_window_.reset();
}

EmulatorApp::EmulatorApp(xe::ui::WindowedAppContext& app_context)
    : xe::ui::WindowedApp(app_context, "xenia", "[Path to .iso/.xex]"),
      debug_window_closed_listener_(*this) {
  AddPositionalOption("target");
}

EmulatorApp::~EmulatorApp() {
  // Should be shut down from OnDestroy if OnInitialize has ever been done, but
  // for the most safety as a running thread may be destroyed only after
  // joining.
  ShutdownEmulatorThreadFromUIThread();
}

std::unique_ptr<apu::AudioSystem> EmulatorApp::CreateAudioSystem(
    cpu::Processor* processor) {
  Factory<apu::AudioSystem, cpu::Processor*> factory;
#if XE_PLATFORM_WIN32
  factory.Add<apu::xaudio2::XAudio2AudioSystem>("xaudio2");
#endif  // XE_PLATFORM_WIN32
#if !XE_PLATFORM_ANDROID
  factory.Add<apu::sdl::SDLAudioSystem>("sdl");
#endif  // !XE_PLATFORM_ANDROID
  factory.Add<apu::nop::NopAudioSystem>("nop");
  return factory.Create(cvars::apu, processor);
}

std::unique_ptr<gpu::GraphicsSystem> EmulatorApp::CreateGraphicsSystem() {
  // While Vulkan is supported by a large variety of operating systems (Windows,
  // GNU/Linux, Android, also via the MoltenVK translation layer on top of Metal
  // on macOS and iOS), please don't remove platform-specific GPU backends from
  // Xenia.
  //
  // Regardless of the operating system, having multiple options provides more
  // stability to users. In case of driver issues, users may try switching
  // between the available backends. For example, in June 2022, on Nvidia Ampere
  // (RTX 30xx), Xenia had synchronization issues that resulted in flickering,
  // most prominently in 4D5307E6, on Direct3D 12 - but the same issue was not
  // reproducible in the Vulkan backend, however, it used ImageSampleExplicitLod
  // with explicit gradients for cubemaps, which triggered a different driver
  // bug on Nvidia (every 1 out of 2x2 pixels receiving junk).
  //
  // Specifically on Microsoft platforms, there are a few reasons why supporting
  // Direct3D 12 is desirable rather than limiting Xenia to Vulkan only:
  // - Wider hardware support for Direct3D 12 on x86 Windows desktops.
  //   Direct3D 12 requires the minimum of Nvidia Fermi, or, with a pre-2021
  //   driver version, Intel HD Graphics 4200. Vulkan, however, is supported
  //   only starting with Nvidia Kepler and a much more recent Intel UHD
  //   Graphics generation.
  // - Wider hardware support on other kinds of Microsoft devices. The Xbox One
  //   and the Xbox Series X|S only support Direct3D as the GPU API in their UWP
  //   runtime, and only version 12 can be granted expanded resource access.
  //   Qualcomm, as of June 2022, also doesn't provide a Vulkan implementation
  //   for their Arm-based Windows devices, while Direct3D 12 is available.
  //   - Both older Intel GPUs and the Xbox One apparently, as well as earlier
  //     Windows 10 versions, also require Shader Model 5.1 DXBC shaders rather
  //     than Shader Model 6 DXIL ones, so a DXBC shader translator should be
  //     available in Xenia too, a DXIL one doesn't fully replace it.
  // - As of June 2022, AMD also refuses to implement the
  //   VK_EXT_fragment_shader_interlock Vulkan extension in their drivers, as
  //   well as its OpenGL counterpart, which is heavily utilized for accurate
  //   support of Xenos render target formats that don't have PC equivalents
  //   (8_8_8_8_GAMMA, 2_10_10_10_FLOAT, 16_16 and 16_16_16_16 with -32 to 32
  //   range, D24FS8) with correct blending. Direct3D 12, however, requires
  //   support for similar functionality (rasterizer-ordered views) on the
  //   feature level 12_1, and the AMD driver implements it on Direct3D, as well
  //   as raster order groups in their Metal driver.
  //
  // Additionally, different host GPU APIs receive feature support at different
  // paces. VK_EXT_fragment_shader_interlock first appeared in 2019, for
  // instance, while Xenia had been taking advantage of rasterizer-ordered views
  // on Direct3D 12 for over half a year at that point (they have existed in
  // Direct3D 12 since the first version).
  //
  // MoltenVK on top Metal also has its flaws and limitations. Metal, for
  // instance, as of June 2022, doesn't provide a switch for primitive restart,
  // while Vulkan does - so MoltenVK is not completely transparent to Xenia,
  // many of its issues that may be not very obvious (unlike when the Metal API
  // is used directly) should be taken into account in Xenia. Also, as of June
  // 2022, MoltenVK translates SPIR-V shaders into the C++-based Metal Shading
  // Language rather than AIR directly, which likely massively increases
  // pipeline object creation time - and Xenia translates shaders and creates
  // pipelines when they're first actually used for a draw command by the game,
  // thus it can't precompile anything that hasn't ever been encountered before
  // there's already no time to waste.
  //
  // Very old hardware (Direct3D 10 level) is also not supported by most Vulkan
  // drivers. However, in the future, Xenia may be ported to it using the
  // Direct3D 11 API with the feature level 10_1 or 10_0. OpenGL, however, had
  // been lagging behind Direct3D prior to versions 4.x, and didn't receive
  // compute shaders until a 4.2 extension (while 4.2 already corresponds
  // roughly to Direct3D 11 features) - and replacing Xenia compute shaders with
  // transform feedback / stream output is not always trivial (in particular,
  // will need to rely on GL_ARB_transform_feedback3 for skipping over memory
  // locations that shouldn't be overwritten).
  //
  // For maintainability, as much implementation code as possible should be
  // placed in `xe::gpu` and shared between the backends rather than duplicated
  // between them.
  const std::string gpu_implementation_name = cvars::gpu;
  if (gpu_implementation_name == "null") {
    return std::make_unique<gpu::null::NullGraphicsSystem>();
  }
  Factory<gpu::GraphicsSystem> factory;
#if XE_PLATFORM_WIN32
  factory.Add<gpu::d3d12::D3D12GraphicsSystem>("d3d12");
#endif  // XE_PLATFORM_WIN32
  factory.Add<gpu::vulkan::VulkanGraphicsSystem>("vulkan");
  std::unique_ptr<gpu::GraphicsSystem> gpu_implementation =
      factory.Create(gpu_implementation_name);
  if (!gpu_implementation) {
    xe::FatalError(
        "Unable to initialize the graphics subsystem.\n"
        "\n"
#if XE_PLATFORM_ANDROID
        "The GPU must support at least Vulkan 1.0 with the 'independentBlend' "
        "feature.\n"
        "\n"
#else
#if XE_PLATFORM_WIN32
        "For Direct3D 12, at least Windows 10 is required, and the GPU must be "
        "compatible with Direct3D 12 feature level 11_0.\n"
        "\n"
#endif  // XE_PLATFORM_WIN32
        "For Vulkan, the Vulkan runtime must be installed, and the GPU must "
        "support at least Vulkan 1.0. The Vulkan runtime can be downloaded at "
        "https://vulkan.lunarg.com/sdk/home.\n"
        "\n"
        "Also, ensure that you have the latest driver installed for your GPU.\n"
        "\n"
#endif  // XE_PLATFORM_ANDROID
        "See https://xenia.jp/faq/ for more information and the system "
        "requirements.");
  }
  return gpu_implementation;
}

std::vector<std::unique_ptr<hid::InputDriver>> EmulatorApp::CreateInputDrivers(
    ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;
  if (cvars::hid.compare("nop") == 0) {
    drivers.emplace_back(
        xe::hid::nop::Create(window, EmulatorWindow::kZOrderHidInput));
  } else {
    Factory<hid::InputDriver, ui::Window*, size_t> factory;
#if XE_PLATFORM_WIN32
    factory.Add("xinput", xe::hid::xinput::Create);
#endif  // XE_PLATFORM_WIN32
#if !XE_PLATFORM_ANDROID
    factory.Add("sdl", xe::hid::sdl::Create);
#endif  // !XE_PLATFORM_ANDROID
#if XE_PLATFORM_WIN32
    // WinKey input driver should always be the last input driver added!
    factory.Add("winkey", xe::hid::winkey::Create);
#endif  // XE_PLATFORM_WIN32
    for (auto& driver : factory.CreateAll(cvars::hid, window,
                                          EmulatorWindow::kZOrderHidInput)) {
      if (XSUCCEEDED(driver->Setup())) {
        drivers.emplace_back(std::move(driver));
      }
    }
    if (drivers.empty()) {
      // Fallback to nop if none created.
      drivers.emplace_back(
          xe::hid::nop::Create(window, EmulatorWindow::kZOrderHidInput));
    }
  }
  return drivers;
}

bool EmulatorApp::OnInitialize() {
  // Guard against accidentally launching a 32-bit binary, which causes
  // pervasive pointer truncation / guest mapping failures.
#if XE_ARCH_AMD64 != 1
  XELOGE(
      "RING BUFFER: startup architecture guard failed: XE_ARCH_AMD64={} "
      "(expected 1)",
      XE_ARCH_AMD64);
  return false;
#endif
#if defined(__i386__) || defined(_M_IX86)
  XELOGE(
      "RING BUFFER: startup architecture guard failed: 32-bit x86 build "
      "detected");
  return false;
#endif
  if (sizeof(void*) != 8) {
    XELOGE(
        "RING BUFFER: startup architecture guard failed: pointer_bits={} "
        "(expected 64)",
        sizeof(void*) * 8);
    return false;
  }
  XELOGI(
      "RING BUFFER: startup architecture guard passed: compile_arch=x86_64 "
      "pointer_bits={}",
      sizeof(void*) * 8);

  Profiler::Initialize();
  Profiler::ThreadEnter("Main");

  // Figure out where internal files and content should go.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe::filesystem::GetExecutableFolder();
    if (!cvars::portable &&
        !std::filesystem::exists(storage_root / "portable.txt")) {
      storage_root = xe::filesystem::GetUserFolder();
#if defined(XE_PLATFORM_WIN32) || defined(XE_PLATFORM_LINUX)
      storage_root = storage_root / "Xenia";
#else
      // TODO(Triang3l): Point to the app's external storage "files" directory
      // on Android.
#warning Unhandled platform for the data root.
      storage_root = storage_root / "Xenia";
#endif
    }
  }
  storage_root = std::filesystem::absolute(storage_root);
  XELOGI("Storage root: {}", storage_root);

  config::SetupConfig(storage_root);

#if XE_ARCH_AMD64 == 1
  amd64::InitFeatureFlags();
  
  // Add CPU feature verification and logging
  auto& cpu = CpuFeatures::Get();
  if (cvars::log_cpu_features) {
    cpu.LogFeatures();
  }
  cpu.ValidateRequiredFeatures();
  
  // Run library verification tests
  if (cvars::log_cpu_verification) {
    XELOGI("");
    XELOGI("Running CPU feature verification tests...");
    cpu.RunAllVerificationTests();
  }
#endif

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else {
    // If content root isn't an absolute path, then it should be relative to the
    // storage root.
    if (!content_root.is_absolute()) {
      content_root = storage_root / content_root;
    }
  }
  content_root = std::filesystem::absolute(content_root);
  XELOGI("Content root: {}", content_root);

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
    cache_root = storage_root / "cache_host";
    // TODO(Triang3l): Point to the app's external storage "cache" directory on
    // Android.
  } else {
    // If content root isn't an absolute path, then it should be relative to the
    // storage root.
    if (!cache_root.is_absolute()) {
      cache_root = storage_root / cache_root;
    }
  }
  cache_root = std::filesystem::absolute(cache_root);
  XELOGI("Host cache root: {}", cache_root);

  if (cvars::discord) {
    discord::DiscordPresence::Initialize();
    discord::DiscordPresence::NotPlaying();
  }

  // Create the emulator but don't initialize so we can setup the window.
  emulator_ =
      std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  // Determine window size based on user setting.
  auto res = xe::gpu::GraphicsSystem::GetInternalDisplayResolution();

  // Main emulator display window.
  emulator_window_ = EmulatorWindow::Create(emulator_.get(), app_context(),
                                            res.first, res.second);
  if (!emulator_window_) {
    XELOGE("Failed to create the main emulator window");
    return false;
  }

  // Setup the emulator and run its loop in a separate thread.
  emulator_thread_quit_requested_.store(false, std::memory_order_relaxed);
  emulator_thread_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(emulator_thread_event_);
  emulator_thread_ = std::thread(&EmulatorApp::EmulatorThread, this);

  return true;
}

void EmulatorApp::OnDestroy() {
  ShutdownEmulatorThreadFromUIThread();

  if (cvars::discord) {
    discord::DiscordPresence::Shutdown();
  }

  Profiler::Dump();
  // The profiler needs to shut down before the graphics context.
  Profiler::Shutdown();

  // Write all cvar overrides to the config.
  config::SaveConfig();

  // TODO(DrChat): Remove this code and do a proper exit.
  XELOGI("Cheap-skate exit!");
#ifdef __APPLE__
  std::exit(EXIT_SUCCESS);
#else
  std::quick_exit(EXIT_SUCCESS);
#endif
}

void EmulatorApp::EmulatorThread() {
  assert_not_null(emulator_thread_event_);

  xe::threading::set_name("Emulator");
  Profiler::ThreadEnter("Emulator");

  // Log subsystem verification before setup
  XELOGI("+===================================================+");
  XELOGI("|     Pre-Setup Subsystem Verification              |");
  XELOGI("+===================================================+");

  auto& cpu = CpuFeatures::Get();
  XELOGI("CPU Summary: {} | Rosetta2: {} | VMX: {} | AVX2: {}",
         cpu.GetCpuString(),
         cpu.is_rosetta2() ? "Yes" : "No", 
         cpu.has_vmx2neon() ? "OK" : "NO",
         cpu.has_avx2() ? "OK" : "NO");

  XELOGI("");

  // Setup and initialize all subsystems. If we can't do something
  // (unsupported system, memory issues, etc) this will fail early.
  XELOGI("Calling emulator_->Setup()...");
  X_STATUS result = emulator_->Setup(
      emulator_window_->window(), emulator_window_->imgui_drawer(), true,
      CreateAudioSystem, CreateGraphicsSystem, CreateInputDrivers);
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: {:08X}", result);
    app_context().RequestDeferredQuit();
    return;
  }
  XELOGI("DEBUG: Emulator setup completed successfully in EmulatorThread");

  bool defer_presenter_setup_for_target_launch = !cvars::target.empty();

  XELOGI("Setting up graphics system presenter painting...");
  if (defer_presenter_setup_for_target_launch) {
    XELOGI(
        "Deferring presenter painting setup until after RunTitle for target "
        "launch to avoid queue starvation");
  } else {
    app_context().CallInUIThreadTagged(
        "SetupGraphicsSystemPresenterPainting",
        [this]() { emulator_window_->SetupGraphicsSystemPresenterPainting(); });
  }

  XELOGI("Getting filesystem from emulator...");
  const auto fs = emulator_->file_system();

  if (cvars::mount_scratch) {
    XELOGI("Setting up scratch filesystem...");
    auto scratch_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\SCRATCH", emulator_->storage_root() / "scratch", false);
    if (!scratch_device->Initialize()) {
      XELOGE("Unable to scan scratch path");
    } else {
      if (!fs->RegisterDevice(std::move(scratch_device))) {
        XELOGE("Unable to register scratch path");
      } else {
        XELOGI("Scratch filesystem mounted successfully");
        fs->RegisterSymbolicLink("scratch:", "\\SCRATCH");
      }
    }
  } else {
    XELOGI("Scratch filesystem mounting disabled");
  }

  if (cvars::mount_cache) {
    XELOGI("Setting up cache filesystems...");
    auto cache0_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\CACHE0", emulator_->storage_root() / "cache0", false);
    if (!cache0_device->Initialize()) {
      XELOGE("Unable to scan cache0 path");
    } else {
      if (!fs->RegisterDevice(std::move(cache0_device))) {
        XELOGE("Unable to register cache0 path");
      } else {
        XELOGI("Cache0 filesystem mounted successfully");
        fs->RegisterSymbolicLink("cache0:", "\\CACHE0");
      }
    }

    auto cache1_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\CACHE1", emulator_->storage_root() / "cache1", false);
    if (!cache1_device->Initialize()) {
      XELOGE("Unable to scan cache1 path");
    } else {
      if (!fs->RegisterDevice(std::move(cache1_device))) {
        XELOGE("Unable to register cache1 path");
      } else {
        XELOGI("Cache1 filesystem mounted successfully");
        fs->RegisterSymbolicLink("cache1:", "\\CACHE1");
      }
    }

    // Some (older?) games try accessing cache:\ too
    // NOTE: this must be registered _after_ the cache0/cache1 devices, due to
    // substring/start_with logic inside VirtualFileSystem::ResolvePath, else
    // accesses to those devices will go here instead
    auto cache_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\CACHE", emulator_->storage_root() / "cache", false);
    if (!cache_device->Initialize()) {
      XELOGE("Unable to scan cache path");
    } else {
      if (!fs->RegisterDevice(std::move(cache_device))) {
        XELOGE("Unable to register cache path");
      } else {
        XELOGI("Cache filesystem mounted successfully");
        fs->RegisterSymbolicLink("cache:", "\\CACHE");
      }
    }
    XELOGI("All cache filesystems setup completed");
  } else {
    XELOGI("Cache filesystem mounting disabled");
  }

  if (cvars::force_mount_devkit) {
    auto devkit_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\DEVKIT", "devkit", false);

    if (!devkit_device->Initialize()) {
      XELOGE("Unable to scan devkit path");
    }

    if (!fs->RegisterDevice(std::move(devkit_device))) {
      XELOGE("Unable to register devkit path");
    }

    fs->RegisterSymbolicLink("DEVKIT:", "\\DEVKIT");
    fs->RegisterSymbolicLink("e:", "\\DEVKIT");
  }

  // Set a debug handler.
  // This will respond to debugging requests so we can open the debug UI.
  if (cvars::debug) {
    emulator_->processor()->set_debug_listener_request_handler(
        [this](xe::cpu::Processor* processor) {
          if (debug_window_) {
            return debug_window_.get();
          }
          app_context().CallInUIThreadSynchronous([this]() {
            fprintf(stderr, "We are alling the debug window and this is the ui thread: %s\n", app_context().IsInUIThread() ? "YES" : "NO");
            debug_window_ = xe::debug::ui::DebugWindow::Create(emulator_.get(),
                                                               app_context());
            fprintf(stderr, "Now we are attempting to add the listener\n");
            debug_window_->window()->AddListener(
                &debug_window_closed_listener_);
          });
          // If failed to enqueue the UI thread call, this will just be null.
          return debug_window_.get();
        });
  }

  emulator_->on_launch.AddListener([&](auto title_id, const auto& game_title) {
    if (cvars::discord) {
      discord::DiscordPresence::PlayingTitle(
          game_title.empty() ? "Unknown Title" : std::string(game_title));
    }
    app_context().CallInUIThread([this]() { emulator_window_->UpdateTitle(); });
    XELOGI("DEBUG: on_launch: signaling emulator thread event (title_id={:08X})",
           title_id);
    emulator_launch_event_seen_.store(true, std::memory_order_relaxed);
    emulator_thread_event_->Set();
  });

  emulator_->on_shader_storage_initialization.AddListener(
      [this](bool initializing) {
        app_context().CallInUIThread([this, initializing]() {
          emulator_window_->SetInitializingShaderStorage(initializing);
        });
      });

  emulator_->on_patch_apply.AddListener([this]() {
    app_context().CallInUIThread([this]() { emulator_window_->UpdateTitle(); });
  });

  emulator_->on_terminate.AddListener([]() {
    if (cvars::discord) {
      discord::DiscordPresence::NotPlaying();
    }
  });

  // Enable emulator input now that the emulator is properly loaded.
  XELOGI("Enabling emulator input...");
    app_context().CallInUIThreadTagged(
      "OnEmulatorInitialized",
      [this]() { emulator_window_->OnEmulatorInitialized(); });

  // Grab path from the flag or unnamed argument.
  XELOGI("Checking for target ROM path...");
  std::filesystem::path path;
  if (!cvars::target.empty()) {
    path = cvars::target;
    XELOGI("Found target path: '{}'", cvars::target);
  } else {
    XELOGI("No target ROM path specified in cvars::target");
  }

  // Track whether we attempted to launch from command line
  bool attempted_command_line_launch = false;

  if (!path.empty()) {
    XELOGI("Target path is not empty, attempting to launch ROM...");
    attempted_command_line_launch = true;

    // Normalize the path and make absolute.
    auto abs_path = std::filesystem::absolute(path);
    XELOGI("Absolute target path: '{}'", abs_path.string());

    XELOGI("DEBUG: CALLING RunTitle (UI thread) - EXTREME DEBUG MODE");
    XELOGI("DEBUG: TARGET PATH: {}", abs_path.string());
    XELOGI("DEBUG: TARGET EXISTS: {}", std::filesystem::exists(abs_path) ? "YES" : "NO");
    XELOGI("DEBUG: TARGET SIZE: {} bytes", std::filesystem::file_size(abs_path));
    XELOGI("DEBUG: APP_CONTEXT: {}", static_cast<void*>(&app_context()));

    fprintf(stderr, "DEBUG: ABOUT TO CALL RunTitle! Path: %s\n", abs_path.c_str());

    XELOGI("Calling emulator_window_->RunTitle() on UI thread...");
    X_STATUS run_title_result = X_STATUS_UNSUCCESSFUL;
    app_context().CallInUIThreadSynchronousTagged(
        "RunTitle", [this, abs_path, &run_title_result]() {
          XELOGI("DEBUG: UI THREAD: RunTitle lambda executing");
          run_title_result = emulator_window_->RunTitle(abs_path);
          XELOGI("DEBUG: UI THREAD: RunTitle returned: {:08X}", run_title_result);
        });
    result = run_title_result;
    XELOGI("DEBUG: RunTitle UI-thread call completed (result={:08X})", result);
    fprintf(stderr, "DEBUG: RunTitle UI thread call completed. Result: %08X\n",
            result);

    if (defer_presenter_setup_for_target_launch) {
      app_context().CallInUIThreadTagged(
          "SetupGraphicsSystemPresenterPainting",
          [this]() { emulator_window_->SetupGraphicsSystemPresenterPainting(); });
    }

    bool has_guest_main_thread = emulator_->HasGuestMainThread();
    bool guest_main_thread_running = emulator_->IsGuestMainThreadRunning();
    XELOGI(
      "DEBUG: POST-RUNTITLE THREAD SNAPSHOT: has_guest_main_thread={} "
      "guest_main_thread_running={}",
      has_guest_main_thread ? "YES" : "NO",
      guest_main_thread_running ? "YES" : "NO");
    if (!has_guest_main_thread) {
      XELOGW(
        "DEBUG: POST-RUNTITLE: guest main thread not registered yet; GPU "
        "bootstrap diagnostics will remain in prelaunch mode until this "
        "changes.");
    }

    XELOGI("DEBUG: RunTitle returned result: {:08X}", result);
    fprintf(stderr, "DEBUG: FINAL RunTitle result: %08X\n", result);

    // Detailed error analysis
    switch (result) {
      case 0x00000000:
        XELOGI("DEBUG: SUCCESS: Title launch successful");
        break;
      case 0x00000001:
        XELOGE("DEBUG: ERROR 0x00000001: X_STATUS_UNSUCCESSFUL - General failure");
        fprintf(stderr, "DEBUG: ERROR 0x00000001: X_STATUS_UNSUCCESSFUL - General failure\n");
        printf("This is a test for the sake of my sanity. If you see this as an output, that means nothing is wrong with our log statements\n");
        printf("So please do not insist that suddently I'm not cleaning artifacts, I am, so the issue is something else if you add log statements and nothing changes\n");
        break;
      case 0xC0000001:
        XELOGE("DEBUG: ERROR 0xC0000001: X_STATUS_UNSUCCESSFUL - Windows-style failure");
        break;
      case 0xC0000022:
        XELOGE("DEBUG: ERROR 0xC0000022: X_STATUS_OBJECT_NAME_NOT_FOUND");
        break;
      case 0xC0000034:
        XELOGE("DEBUG: ERROR 0xC0000034: X_STATUS_OBJECT_NAME_INVALID");
        break;
      default:
        XELOGE("DEBUG: ERROR Unknown: {:08X}", result);
        break;
    }

    // Check for EXPLICIT success, not just "not critical failure"
    // 0x00000001 is X_STATUS_UNSUCCESSFUL and should be treated as failure
    if (result != X_STATUS_SUCCESS) {
      XELOGE("DEBUG: FAILED TO LAUNCH TARGET!");
      XELOGE("DEBUG: RunTitle returned: {:08X} (not X_STATUS_SUCCESS)", result);
      // Don't fatal error - just log it and continue
      // This allows the user to try loading another title
    } else {
      XELOGI("DEBUG: ROM launch initiated successfully");
    }
  } else {
    XELOGI("No ROM to launch - entering idle mode");
  }

  // Only check XAM loader data if we didn't already attempt a command-line launch
  // This prevents double-launching the same title
  if (!attempted_command_line_launch) {
    XELOGI("Checking XAM module for loader data...");
    auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
        "xam.xex");

    if (xam) {
      XELOGI("Loading XAM loader data...");
      xam->LoadLoaderData();

      if (xam->loader_data().launch_data_present) {
        XELOGI("XAM launch data present, loading from: '{}'", xam->loader_data().host_path);
        const std::filesystem::path host_path = xam->loader_data().host_path;
        app_context().CallInUIThread([this, host_path]() {
          return emulator_window_->RunTitle(host_path);
        });
      } else {
        XELOGI("No XAM launch data present");
      }
    } else {
      XELOGI("XAM module not found");
    }
  } else {
    XELOGI("Skipping XAM loader data check - already attempted command-line launch");
  }

  // Now, we're going to use this thread to drive events related to emulation.
  XELOGI("Entering main emulator event loop...");
  bool launch_event_consumed = false;
  uint64_t last_wait_log_ms = 0;
  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    XELOGI("Waiting for emulator thread event...");
    auto wait_result = xe::threading::Wait(
        emulator_thread_event_.get(), false, std::chrono::milliseconds(1000));
    if (wait_result == xe::threading::WaitResult::kSuccess) {
      XELOGI("DEBUG: Emulator thread event signaled - entering WaitUntilExit");
      launch_event_consumed = true;
      emulator_->WaitUntilExit();
      break;
    }

    // If the event didn't fire but the main thread is already live, proceed.
    if (!launch_event_consumed && emulator_->HasGuestMainThread()) {
      XELOGW(
          "DEBUG: Guest main thread detected without event signal - proceeding to WaitUntilExit");
      launch_event_consumed = true;
      emulator_->WaitUntilExit();
      break;
    }

    // Periodic heartbeat so the wait doesn't look like a dead hang.
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    if (now_ms - last_wait_log_ms > 5000) {
      XELOGI(
          "DEBUG: Emulator thread wait timeout - launch_event_seen={}, guest_main_present={}",
          emulator_launch_event_seen_.load(std::memory_order_relaxed) ? "YES"
                                                                     : "NO",
          emulator_->HasGuestMainThread() ? "YES" : "NO");
      last_wait_log_ms = now_ms;
    }
  }
}

void EmulatorApp::ShutdownEmulatorThreadFromUIThread() {
  // TODO(Triang3l): Proper shutdown of the emulator (relying on std::quick_exit
  // for now) - currently WaitUntilExit loops forever otherwise (plus possibly
  // lots of other things not shutting down correctly now). Some parts of the
  // code call the regular std::exit, which seems to be calling destructors (at
  // least on Linux), so the entire join is currently commented out.
#if 0
  // Same thread as the one created it, to make sure there's zero possibility of
  // a race with the creation of the emulator thread.
  assert_true(app_context().IsInUIThread());
  emulator_thread_quit_requested_.store(true, std::memory_order_relaxed);
  if (!emulator_thread_.joinable()) {
    return;
  }
  emulator_thread_event_->Set();
  emulator_thread_.join();
#endif
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::EmulatorApp::Create);
