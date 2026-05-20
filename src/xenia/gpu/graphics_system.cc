/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/graphics_system.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

DEFINE_uint32(internal_display_resolution, 8,
              "Allow games that support different resolutions to render "
              "in a specific resolution.\n"
              "This is not guaranteed to work with all games or improve "
              "performance.\n"
              "   0=640x480\n"
              "   1=640x576\n"
              "   2=720x480\n"
              "   3=720x576\n"
              "   4=800x600\n"
              "   5=848x480\n"
              "   6=1024x768\n"
              "   7=1152x864\n"
              "   8=1280x720 (Default)\n"
              "   9=1280x768\n"
              "   10=1280x960\n"
              "   11=1280x1024\n"
              "   12=1360x768\n"
              "   13=1440x900\n"
              "   14=1680x1050\n"
              "   15=1920x540\n"
              "   16=1920x1080\n"
              "   17=internal_display_resolution_x/y",
              "Video");
DEFINE_uint32(internal_display_resolution_x, 1280,
              "Custom width. See internal_display_resolution. Range 1-1920.",
              "Video");
DEFINE_uint32(internal_display_resolution_y, 720,
              "Custom height. See internal_display_resolution. Range 1-1080.\n",
              "Video");

DEFINE_bool(
    store_shaders, true,
    "Store shaders persistently and load them when loading games to avoid "
    "runtime spikes and freezes when playing the game not for the first time.",
    "GPU");

namespace xe {
namespace gpu {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if XE_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // XE_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : frame_limiter_worker_running_(false) {
  register_file_ = reinterpret_cast<RegisterFile*>(memory::AllocFixed(
      nullptr, sizeof(RegisterFile), memory::AllocationType::kReserveCommit,
      memory::PageAccess::kReadWrite));
}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(cpu::Processor* processor,
                               kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context,
                               bool with_presentation) {
  memory_ = processor->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;

  scaled_aspect_x_ = 16;
  scaled_aspect_y_ = 9;

  auto custom_res_x = cvars::internal_display_resolution_x;
  auto custom_res_y = cvars::internal_display_resolution_y;
  if (!custom_res_x || custom_res_x > 1920 || !custom_res_y ||
      custom_res_y > 1080) {
    OVERRIDE_uint32(internal_display_resolution_x,
                    internal_display_resolution_entries[8].first);
    OVERRIDE_uint32(internal_display_resolution_y,
                    internal_display_resolution_entries[8].second);
    config::SaveConfig();
    xe::FatalError(fmt::format(
        "Invalid custom resolution specified: {}x{}\n"
        "Width must be between 1-1920.\nHeight must be between 1-1080.",
        custom_res_x, custom_res_y));
  }

  if (with_presentation && provider_) {
    // Safe if either the UI thread call or the presenter creation fails.
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() {
        presenter_ = provider_->CreatePresenter(
            [this](bool is_responsible, bool statically_from_ui_thread) {
              OnHostGpuLossFromAnyThread(is_responsible);
            });
      });
    } else {
      // May be needed for offscreen use, such as capturing the guest output
      // image.
      presenter_ = provider_->CreatePresenter(
          [this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
    }
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    XELOGE("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Let the processor know we want register access callbacks.
  memory_->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<cpu::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<cpu::MMIOWriteCallback>(WriteRegisterThunk));

  // Frame limiter thread.
  frame_limiter_worker_running_ = true;
  frame_limiter_worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            uint64_t last_frame_time = Clock::QueryGuestTickCount();
    // Sleep for 90% of the vblank duration on Windows/macOS, spin for 10%
    // Linux uses full sleep duration due to scheduler quantum issues
#if XE_PLATFORM_WIN32 || XE_PLATFORM_MAC
            constexpr double duration_scalar = 0.90;
#elif XE_PLATFORM_LINUX
            constexpr double duration_scalar = 1.0;
#endif

            while (frame_limiter_worker_running_) {
              // Read guest_display_refresh_cap cvar each frame to allow
              // runtime changes
              // true: Fire vblanks at fixed rate (50Hz PAL, 60Hz NTSC)
              // false: Fire vblanks limited by framerate_limit or 1ms
              // Note: framerate_limit is handled separately at IssueSwap for
              // host presentation throttling
              bool refresh_cap_enabled = cvars::guest_display_refresh_cap;

              if (refresh_cap_enabled) {
                const uint32_t vblank_hz = GetGuestVblankRateHz();
                const uint64_t sleep_ns = static_cast<uint64_t>(
                    (1000000000.0 / static_cast<double>(vblank_hz)) *
                    duration_scalar);

#if XE_PLATFORM_WIN32 || XE_PLATFORM_MAC
                // Windows/macOS: time-gating + 90% sleep + 10% spin
                const uint64_t tick_freq = Clock::guest_tick_frequency();
                const uint64_t target_duration_ticks = tick_freq / vblank_hz;
                const uint64_t current_time = Clock::QueryGuestTickCount();
                const uint64_t time_delta = current_time - last_frame_time;

                if (time_delta >= target_duration_ticks) {
                  // If we've fallen behind by more than 2 frames, reset
                  if (time_delta > target_duration_ticks * 2) {
                    last_frame_time = current_time;
                  } else {
                    last_frame_time += target_duration_ticks;
                  }
                  MarkVblank();
#if XE_PLATFORM_MAC
                  threading::NanoSleepPrecise(sleep_ns);
#else
                  threading::NanoSleep(sleep_ns);
#endif
                }
#elif XE_PLATFORM_LINUX
                // Linux: simplified timing to avoid oversleeping
                MarkVblank();
                threading::NanoSleep(sleep_ns);
#endif
              } else {
                // Unlimited mode (vsync=false) - fire vblanks as fast as
                // possible Host presentation is separately throttled by
                // framerate_limit
                MarkVblank();
                threading::Sleep(std::chrono::milliseconds(1));
              }
            }
            return 0;
          },
          kernel_state->GetIdleProcess()));
  // As we run vblank interrupts the debugger must be able to suspend us.
  frame_limiter_worker_thread_->set_can_debugger_suspend(true);
  frame_limiter_worker_thread_->set_name("GPU Frame limiter");
  frame_limiter_worker_thread_->Create();
  frame_limiter_worker_thread_->thread()->set_priority(
      threading::ThreadPriority::kLowest);
  if (cvars::trace_gpu_stream) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (frame_limiter_worker_thread_) {
    frame_limiter_worker_running_ = false;
    frame_limiter_worker_thread_->Wait(0, 0, 0, nullptr);
    frame_limiter_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(
    [[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }

  config::SaveConfig();

  xe::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context,
                                           GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                        uint32_t addr, uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case XE_GPU_REG_RB_EDRAM_TIMING:
      return 0x08100748;
    case XE_GPU_REG_RB_BC_CONTROL:
      return 0x0000200E;
    case XE_GPU_REG_D1MODE_V_COUNTER: {
      // Free-running scanline counter, like Xenos drives off the pixel clock.
      // Cycles 0..(total_lines-1) every frame, including the vertical-blank
      // region above the 720 active lines reported by D1MODE_VIEWPORT_SIZE
      // below. Total ~= active + 4% blanking (720p60 -> 750, 576p50 -> 625).
      // Period is measured from the MarkVblank cadence, so V_COUNTER stays
      // coupled to whatever vblank rate is actually in effect (50Hz, 60Hz,
      // or uncapped ~1ms) — matching how silicon raises vblank IRQs from
      // V_COUNTER crossings.
      const uint32_t vblank_hz = GetGuestVblankRateHz();
      const uint32_t total_lines = (vblank_hz == 50) ? 625u : 750u;
      uint64_t period = vblank_period_ticks_.load(std::memory_order_acquire);
      if (!period) {
        // Pre-first-vblank bootstrap: assume the configured rate.
        period = Clock::guest_tick_frequency() / vblank_hz;
        if (!period) {
          return 0;
        }
      }
      const uint64_t last =
          last_vblank_guest_tick_.load(std::memory_order_acquire);
      if (!last) {
        return 0;
      }
      const uint64_t now = Clock::QueryGuestTickCount();
      uint64_t delta = (now > last) ? (now - last) : 0;
      if (delta >= period) {
        // Reader outran the next vblank (stall, paused, etc.); park on the
        // last line until MarkVblank advances the anchor.
        delta = period - 1;
      }
      return static_cast<uint32_t>((delta * total_lines) / period);
    }
    case XE_GPU_REG_D1MODE_VBLANK_VLINE_STATUS:
      return 1;  // vblank
    case XE_GPU_REG_D1MODE_VIEWPORT_SIZE:
      // 1280x720, [width(0x0FFF), height(0x0FFF)].
      return 0x050002D0;
    default:
      if (!register_file()->IsValidRegister(r)) {
        XELOGE("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file()->values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      XELOGW("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  this->register_file()->values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr,
                                                uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback,
                                          uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  XELOGGPU("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  kernel_state()->EmulateCPInterruptDPC(interrupt_callback_,
                                        interrupt_callback_data_, source, cpu);
}

void GraphicsSystem::MarkVblank() {
  SCOPE_profile_cpu_f("gpu");

  // Capture vblank cadence so D1MODE_V_COUNTER tracks the actual rate
  // (50Hz, 60Hz, or uncapped ~1ms), not just the configured one.
  const uint64_t now = Clock::QueryGuestTickCount();
  const uint64_t prev =
      last_vblank_guest_tick_.exchange(now, std::memory_order_acq_rel);
  if (prev && now > prev) {
    vblank_period_ticks_.store(now - prev, std::memory_order_release);
  }

  // Increment vblank counter (so the game sees us making progress).
  command_processor_->increment_counter();

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread(
      [&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread(
      [&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  if (!cvars::store_shaders) {
    if (completion_callback) {
      completion_callback();
    }
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(
          cache_root, title_id, true, std::move(completion_callback));
    } else {
      xe::threading::Fence fence;
      command_processor_->CallInThread(
          [this, cache_root, title_id, &fence,
           completion_callback = std::move(completion_callback)]() mutable {
            command_processor_->InitializeShaderStorage(
                cache_root, title_id, true, std::move(completion_callback));
            fence.Signal();
          });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread(
        [this, cache_root, title_id,
         completion_callback = std::move(completion_callback)]() mutable {
          command_processor_->InitializeShaderStorage(
              cache_root, title_id, false, std::move(completion_callback));
        });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(cvars::trace_gpu_prefix);
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(cvars::trace_gpu_prefix);
}

void GraphicsSystem::EndTracing() { command_processor_->EndTracing(); }

void GraphicsSystem::Pause() {
  paused_ = true;

  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;

  command_processor_->Resume();
}

bool GraphicsSystem::Save(ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);

  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();

  return command_processor_->Restore(stream);
}

std::pair<uint16_t, uint16_t> GraphicsSystem::GetInternalDisplayResolution() {
  if (cvars::internal_display_resolution >=
      internal_display_resolution_entries.size()) {
    return {cvars::internal_display_resolution_x,
            cvars::internal_display_resolution_y};
  }
  return internal_display_resolution_entries
      [cvars::internal_display_resolution];
}

}  // namespace gpu
}  // namespace xe
