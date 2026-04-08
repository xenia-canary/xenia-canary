/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_GRAPHICS_SYSTEM_H_
#define XENIA_GPU_GRAPHICS_SYSTEM_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "xenia/cpu/processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
class Emulator;
}  // namespace xe

namespace xe {
namespace gpu {

constexpr std::array<std::pair<uint16_t, uint16_t>, 17>
    internal_display_resolution_entries = {{{640, 480},
                                            {640, 576},
                                            {720, 480},
                                            {720, 576},
                                            {800, 600},
                                            {848, 480},
                                            {1024, 768},
                                            {1152, 864},
                                            {1280, 720},
                                            {1280, 768},
                                            {1280, 960},
                                            {1280, 1024},
                                            {1360, 768},
                                            {1440, 900},
                                            {1680, 1050},
                                            {1920, 540},
                                            {1920, 1080}}};

constexpr std::array<std::pair<uint16_t, uint16_t>, 3>
    driver_display_resolution = {{{1440, 900}, {1280, 720}, {1680, 1050}}};

class CommandProcessor;

class GraphicsSystem {
#ifdef __APPLE__
  // Diagnostic: Warn if guest never writes CP_RB_BASE/CP_RB_CNTL/CP_RB_WPTR
  // after timeout
  void WarnIfRingBufferNeverInitialized(uint64_t since_first_vblank);
#endif

 public:
  virtual ~GraphicsSystem();

  virtual std::string name() const = 0;

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  kernel::KernelState* kernel_state() const { return kernel_state_; }
  ui::GraphicsProvider* provider() const { return provider_.get(); }
  ui::Presenter* presenter() const { return presenter_.get(); }

  virtual X_STATUS Setup(cpu::Processor* processor,
                         kernel::KernelState* kernel_state,
                         ui::WindowedAppContext* app_context,
                         bool with_presentation);
  virtual void Shutdown();

  // May be called from any thread any number of times, even during recovery
  // from a device loss.
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return register_file_; }
  CommandProcessor* command_processor() const {
    return command_processor_.get();
  }

#ifdef __APPLE__
  uint64_t last_write_pointer_update_ms() const {
    return last_wptr_update_ms_.load(std::memory_order_relaxed);
  }
  uint32_t last_write_pointer_value() const {
    return last_wptr_value_.load(std::memory_order_relaxed);
  }
  void RecordWritePointerUpdate(uint32_t value, const char* source);
  struct PipelinePrereqSnapshot {
    uint64_t mmio_any_writes = 0;
    uint64_t mmio_cp_rb_base_writes = 0;
    uint64_t mmio_cp_rb_cntl_writes = 0;
    uint64_t mmio_cp_rb_wptr_writes = 0;
    uint64_t mmio_cp_rb_base_guest_writes = 0;
    uint64_t mmio_cp_rb_cntl_guest_writes = 0;
    uint64_t mmio_cp_rb_wptr_guest_writes = 0;
    uint64_t mmio_cp_guest_writes = 0;
    uint64_t mmio_cp_host_writes = 0;

    uint64_t vd_swap_calls = 0;
    uint64_t vd_initialize_ring_buffer_calls = 0;
    uint64_t vd_set_interrupt_callback_calls = 0;
    uint64_t vd_get_system_command_buffer_calls = 0;
    uint64_t vd_enable_rptr_writeback_calls = 0;
    uint64_t vd_initialize_edram_calls = 0;
    uint64_t vd_retrain_edram_calls = 0;
    uint64_t vd_hsio_training_succeeded_calls = 0;

    bool interrupt_callback_set = false;
    uint32_t interrupt_callback = 0;
    uint64_t last_interrupt_callback_set_ms = 0;
    uint64_t last_interrupt_dispatch_ms = 0;
    bool interrupt_dispatch_seen = false;

    bool no_force_profile = false;
    bool suspend_force_fallback_until_module_ready = false;

    bool user_module_load_event_present = false;
    bool module_load_inflight = false;
    uint64_t module_load_calls = 0;
    uint64_t module_load_success = 0;
    uint64_t module_load_fail = 0;
    uint64_t module_load_progress_count = 0;
    uint64_t loaded_user_modules = 0;
    uint64_t loaded_executable_user_modules = 0;
    uint64_t module_last_begin_ms = 0;
    uint64_t module_last_end_ms = 0;
    uint64_t module_last_progress_ms = 0;
    bool executable_module_present = false;
    uint32_t executable_module_entry = 0;
    bool guest_main_thread_ready = false;
    bool guest_main_thread_running = false;
    std::string guest_main_dispatch_stall_state;
    uint32_t guest_main_dispatch_stall_repeat_samples = 0;
    uint64_t guest_main_dispatch_stall_age_ms = 0;

    uint64_t prelaunch_bootstrap_nudge_count = 0;
    bool prelaunch_bootstrap_nudge_done = false;
    uint64_t guest_main_resume_nudge_count = 0;
    uint64_t dead_pipeline_recovery_count = 0;
    uint64_t auto_callback_recovery_count = 0;
  };
  PipelinePrereqSnapshot GetPipelinePrereqSnapshot() const;
  void NotifyVdSwapCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdInitializeEnginesCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdGetGraphicsAsicIDCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdInitializeRingBufferCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdEnableRingBufferRPtrWriteBackCall(uint32_t pc,
                                                 uint32_t thread_id);
  void NotifyVdSetGraphicsInterruptCallbackCall(uint32_t pc, uint32_t thread_id,
                                                uint32_t lr = 0);
  void NotifyVdGetSystemCommandBufferCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdInitializeEDRAMCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdRetrainEDRAMCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdRetrainEDRAMWorkerCall(uint32_t pc, uint32_t thread_id);
  void NotifyVdIsHSIOTrainingSucceededCall(uint32_t pc, uint32_t thread_id);
#endif
  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  virtual void EnableReadPointerWriteBack(uint32_t ptr,
                                          uint32_t block_size_log2);

  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data);
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

  virtual void ClearCaches();

#ifdef __APPLE__
  bool IsReadyForSwap() const;
  uint32_t GetInterruptCallback() const;
  bool EnsureSystemCommandBufferReady(uint32_t min_size,
                                      const char* reason = nullptr);
  void SetSystemCommandBuffer(uint32_t ptr, uint32_t size,
                              const char* reason = nullptr);
  uint32_t GetSystemCommandBuffer() const;
  uint32_t GetSystemCommandBufferSize() const;
  void RegisterWriteCallback(uint32_t addr, uint32_t value);
  void AttemptFallbackInitialization();
#endif
  void InitializeShaderStorage(const std::filesystem::path& cache_root,
                               uint32_t title_id, bool blocking);

  void RequestFrameTrace();
  void BeginTracing();
  void EndTracing();

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  static std::pair<uint16_t, uint16_t> GetInternalDisplayResolution();

  std::pair<uint32_t, uint32_t> GetScaledAspectRatio() const {
    return {scaled_aspect_x_, scaled_aspect_y_};
  };
  void SetScaledAspectRatio(uint32_t x, uint32_t y) {
    scaled_aspect_x_ = x;
    scaled_aspect_y_ = y;
  };

 protected:
  GraphicsSystem();

  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  static uint32_t ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                    uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs,
                                 uint32_t addr, uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  void MarkVblank();

  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<ui::GraphicsProvider> provider_;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::atomic<bool> frame_limiter_worker_running_;
  kernel::object_ref<kernel::XHostThread> frame_limiter_worker_thread_;

  RegisterFile* register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;

  bool paused_ = false;

  uint32_t scaled_aspect_x_ = 0;
  uint32_t scaled_aspect_y_ = 0;

#ifdef __APPLE__
  std::atomic<uint64_t> last_wptr_update_ms_{0};
  std::atomic<uint32_t> last_wptr_value_{0};
  std::atomic<uint32_t> last_wptr_source_id_{0};
  std::atomic<uint64_t> wptr_update_count_{0};
  std::atomic<uint64_t> wptr_source_mmio_count_{0};
  std::atomic<uint64_t> wptr_source_cp_packet_count_{0};
  std::atomic<uint64_t> wptr_source_debug_swap_count_{0};
  std::atomic<uint64_t> wptr_source_other_count_{0};
  std::atomic<uint64_t> last_interrupt_callback_set_ms_{0};
  std::atomic<uint64_t> last_interrupt_dispatch_ms_{0};
  std::atomic<uint32_t> system_command_buffer_ptr_{0};
  std::atomic<uint32_t> system_command_buffer_size_{0};
#endif

 private:
#ifdef __APPLE__
  struct MmioWriteState {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> guest_count{0};
    std::atomic<uint64_t> host_count{0};
    std::atomic<uint32_t> value{0};
    std::atomic<uint32_t> pc{0};
    std::atomic<uint32_t> thread_id{0};
    std::atomic<uint64_t> time_ms{0};
  };

  struct VdCallState {
    std::atomic<uint64_t> count{0};
    std::atomic<uint32_t> pc{0};
    std::atomic<uint32_t> lr{0};
    std::atomic<uint32_t> thread_id{0};
    std::atomic<uint64_t> time_ms{0};
  };

  void UpdateMmioWriteState(MmioWriteState& state, uint32_t value, uint32_t pc,
                            uint32_t thread_id, bool guest_origin,
                            const char* source);
  void UpdateVdCallState(VdCallState& state, uint32_t pc, uint32_t thread_id,
                         const char* source, uint32_t lr = 0);
  void LogBootstrapMmioFirstWriteSnapshot(const char* reg_name, uint32_t reg,
                                          uint32_t addr, uint32_t value,
                                          uint32_t pc, uint32_t thread_id,
                                          bool guest_origin);
  void ProbeCpBootstrapRegistersWhenMmioSilent(uint64_t now_ms,
                                               uint64_t since_first_vblank);
  bool TryAutoRecoverMissingInterruptCallback(uint64_t now_ms,
                                              uint64_t since_first_vblank,
                                              const char* reason);
  void InjectGuestBootstrapDebugActivity(const char* reason,
                                         uint64_t since_first_vblank);
  void DumpNoSwapDebugState(uint64_t since_first_vblank, uint32_t read_ptr,
                            uint32_t write_ptr);
  void DumpGuestThreadStates(const char* reason);
#endif
  std::unique_ptr<ui::Presenter> presenter_;

  std::atomic_flag host_gpu_loss_reported_;

#ifdef __APPLE__
  MmioWriteState mmio_cp_rb_base_;
  MmioWriteState mmio_cp_rb_cntl_;
  MmioWriteState mmio_cp_rb_rptr_addr_;
  MmioWriteState mmio_cp_rb_rptr_;
  MmioWriteState mmio_cp_rb_wptr_;
  MmioWriteState mmio_cp_rb_wptr_delay_;
  MmioWriteState mmio_cp_rb_rptr_wr_;
  MmioWriteState mmio_fetch0_;
  std::atomic<uint64_t> mmio_any_write_count_{0};
  std::atomic<uint64_t> mmio_non_cp_write_count_{0};
  std::atomic<uint64_t> mmio_cp_guest_write_count_{0};
  std::atomic<uint64_t> mmio_cp_host_write_count_{0};
  std::atomic<uint32_t> mmio_any_first_reg_{0xFFFFFFFFu};
  std::atomic<uint32_t> mmio_any_last_reg_{0};
  std::atomic<uint32_t> mmio_any_last_value_{0};
  std::atomic<uint32_t> mmio_any_last_pc_{0};
  std::atomic<uint32_t> mmio_any_last_thread_id_{0};
  std::atomic<uint64_t> mmio_cp_endian_autofix_count_{0};
  std::atomic<uint32_t> mmio_cp_endian_last_reg_{0};
  std::atomic<uint32_t> mmio_cp_endian_last_raw_value_{0};
  std::atomic<uint32_t> mmio_cp_endian_last_normalized_value_{0};

  VdCallState vd_swap_state_;
  VdCallState vd_init_engines_state_;
  VdCallState vd_get_graphics_asic_id_state_;
  VdCallState vd_init_ring_buffer_state_;
  VdCallState vd_enable_rptr_writeback_state_;
  VdCallState vd_set_interrupt_callback_state_;
  VdCallState vd_get_system_cmd_buffer_state_;
  VdCallState vd_init_edram_state_;
  VdCallState vd_retrain_edram_state_;
  VdCallState vd_retrain_edram_worker_state_;
  VdCallState vd_hsio_training_succeeded_state_;
  std::atomic<uint64_t> last_no_swap_thread_dump_ms_{0};
  std::atomic<uint64_t> last_silent_mmio_probe_ms_{0};
  bool silent_mmio_probe_initialized_ = false;
  uint32_t silent_mmio_probe_last_cp_rb_base_ = 0;
  uint32_t silent_mmio_probe_last_cp_rb_cntl_ = 0;
  uint32_t silent_mmio_probe_last_cp_rb_rptr_addr_ = 0;
  uint32_t silent_mmio_probe_last_cp_rb_rptr_ = 0;
  uint32_t silent_mmio_probe_last_cp_rb_wptr_ = 0;
  uint32_t silent_mmio_probe_last_cp_rb_wptr_delay_ = 0;
  uint32_t silent_mmio_probe_last_cp_rb_rptr_wr_ = 0;
  std::array<uint32_t, 6> silent_mmio_probe_last_fetch0_{};
  std::atomic<uint64_t> startup_watch_first_vblank_ms_{0};
  std::atomic<uint64_t> startup_watch_last_log_ms_{0};
  std::atomic<bool> startup_watch_ready_logged_{false};
  std::atomic<bool> startup_watch_timeout_logged_{false};
  std::atomic<uint64_t> prelaunch_bootstrap_nudge_count_{0};
  std::atomic<uint64_t> last_prelaunch_bootstrap_nudge_ms_{0};
  std::atomic<bool> prelaunch_bootstrap_nudge_done_{false};
  std::atomic<uint64_t> prelaunch_module_nudge_count_{0};
  std::atomic<uint64_t> last_prelaunch_module_nudge_ms_{0};
  std::atomic<bool> prelaunch_module_nudge_done_{false};
  std::atomic<uint64_t> prelaunch_force_module_load_nudge_count_{0};
  std::atomic<uint64_t> last_prelaunch_force_module_load_nudge_ms_{0};
  std::atomic<bool> prelaunch_force_module_load_nudge_inflight_{false};
  std::atomic<bool> prelaunch_executable_module_missing_seen_{false};
  std::atomic<bool> prelaunch_executable_module_transition_logged_{false};
  std::atomic<uint64_t> prelaunch_guest_main_launch_nudge_count_{0};
  std::atomic<uint64_t> last_prelaunch_guest_main_launch_nudge_ms_{0};
  std::atomic<bool> prelaunch_guest_main_launch_nudge_done_{false};
  std::atomic<uint64_t> guest_main_resume_nudge_count_{0};
  std::atomic<uint64_t> last_guest_main_resume_nudge_ms_{0};
  std::atomic<uint64_t> dead_pipeline_recovery_count_{0};
  std::atomic<uint64_t> last_dead_pipeline_recovery_ms_{0};
  std::atomic<bool> dead_pipeline_recovery_done_{false};
  std::atomic<uint64_t> auto_callback_recovery_count_{0};
  std::atomic<uint64_t> last_auto_callback_recovery_ms_{0};
  std::atomic<bool> auto_callback_recovery_done_{false};
  uint32_t forced_interrupt_callback_ = 0;
  bool forced_interrupt_callback_failed_ = false;
  std::atomic<uint64_t> last_forced_interrupt_ms_{0};
  std::atomic<bool> forced_gpu_init_done_{false};
  uint32_t forced_ring_buffer_ptr_ = 0;
  uint32_t forced_ring_buffer_size_log2_ = 0;
#endif
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GRAPHICS_SYSTEM_H_
