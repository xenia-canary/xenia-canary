/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_COMMAND_PROCESSOR_MAC_H_
#define XENIA_GPU_COMMAND_PROCESSOR_MAC_H_

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "xenia/base/ring_buffer_mac.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/kernel/xthread_mac.h"
#include "xenia/memory_mac.h"
#include "xenia/ui/presenter_mac.h"

namespace xe {

class ByteStream;

namespace gpu {

enum class GPUSetting { ClearMemoryPageState, ReadbackMemexport };

enum class ReadbackResolveMode {
  kDisabled,  // No readback (none)
  kFast,      // Delayed sync, 1 frame behind (fast)
  kFull       // Immediate sync with GPU stall (full)
};

void SaveGPUSetting(GPUSetting setting, uint64_t value);
bool GetGPUSetting(GPUSetting setting);
ReadbackResolveMode GetReadbackResolveMode();
void SetReadbackResolveMode(const std::string& mode);

class GraphicsSystem;
class Shader;

struct SwapState {
  // Lock must be held when changing data in this structure.
  std::mutex mutex;
  // Dimensions of the framebuffer textures. Should match window size.
  uint32_t width = 0;
  uint32_t height = 0;
  // Current front buffer, being drawn to the screen.
  uintptr_t front_buffer_texture = 0;
  // Current back buffer, being updated by the CP.
  uintptr_t back_buffer_texture = 0;
  // Backend data
  void* backend_data = nullptr;
  // Whether the back buffer is dirty and a swap is pending.
  bool pending = false;
};

enum class SwapMode {
  kNormal,
  kIgnored,
};

enum class GammaRampType {
  kUnknown = 0,
  kTable,
  kPWL,
};

class CommandProcessor {
 protected:
  RingBuffer
      reader_;  // chrispy: instead of having ringbuffer on stack, have it near
                // the start of the class so we can access it via rel8. This
                // also reduces the number of params we need to pass
 public:
  enum class SwapPostEffect {
    kNone,
    kFxaa,
    kFxaaExtreme,
  };

  CommandProcessor(GraphicsSystem* graphics_system,
                   kernel::KernelState* kernel_state);
  virtual ~CommandProcessor();
  uint32_t counter() const { return counter_; }
  void increment_counter() { counter_++; }

  uint32_t primary_buffer_ptr() const { return primary_buffer_ptr_; }
  uint32_t primary_buffer_size() const { return primary_buffer_size_; }
  uint32_t read_ptr_index() const { return read_ptr_index_; }
  uint32_t write_ptr_index() const { return write_ptr_index_.load(); }
  uint64_t swap_packet_count() const { return swap_packet_count_.load(); }
  uint64_t last_swap_packet_ms() const { return last_swap_packet_ms_.load(); }
  uint32_t last_swap_frontbuffer_ptr() const {
    return last_swap_frontbuffer_ptr_.load();
  }
  uint32_t last_swap_width() const { return last_swap_width_.load(); }
  uint32_t last_swap_height() const { return last_swap_height_.load(); }
  uint64_t ring_buffer_watch_hits() const {
    return ring_buffer_watch_hits_.load();
  }
  uint64_t ring_buffer_last_write_ms() const {
    return ring_buffer_last_write_ms_.load();
  }
  uint32_t ring_buffer_last_write_phys() const {
    return ring_buffer_last_write_phys_.load();
  }
  struct RingBufferDebugState {
    bool worker_running = false;
    bool worker_setup_complete = false;
    bool worker_setup_failed = false;
    bool ring_buffer_initialized = false;
    bool initialization_acknowledged = false;
    bool has_write_ptr_event = false;
    bool has_init_complete_event = false;
    uint64_t init_request_count = 0;
    uint64_t init_ack_transition_count = 0;
    uint64_t last_init_request_ms = 0;
    uint64_t last_init_signal_ms = 0;
    uint64_t last_init_ack_ms = 0;
    uint32_t primary_buffer_ptr = 0;
    uint32_t primary_buffer_size = 0;
    uint32_t read_ptr_index = 0;
    uint32_t write_ptr_index = 0;
    uint32_t read_ptr_writeback_ptr = 0;
    uint32_t read_ptr_update_freq = 0;
    uint64_t swap_packet_count = 0;
    uint64_t last_swap_packet_ms = 0;
    uint32_t last_swap_frontbuffer_ptr = 0;
    uint32_t last_swap_width = 0;
    uint32_t last_swap_height = 0;
    uint64_t debug_host_swap_injection_count = 0;
    uint64_t debug_host_swap_ring_injection_count = 0;
    uint64_t debug_host_swap_direct_issue_count = 0;
    uint64_t debug_host_swap_last_ms = 0;
    uint32_t debug_host_swap_last_frontbuffer_ptr = 0;
    uint32_t debug_host_swap_last_width = 0;
    uint32_t debug_host_swap_last_height = 0;
    uint64_t ring_buffer_watch_hits = 0;
    uint64_t ring_buffer_watch_arm_count = 0;
    uint64_t ring_buffer_watch_last_arm_ms = 0;
    uint64_t ring_buffer_last_write_ms = 0;
    uint32_t ring_buffer_last_write_phys = 0;
    bool ring_buffer_watch_rearm_requested = false;
    uint64_t ring_watch_last_transition_ms = 0;
    uint32_t ring_watch_last_phase_id = 0;
    uint32_t ring_watch_last_range_start = 0;
    uint32_t ring_watch_last_range_size = 0;
    bool ring_watch_last_guest_write_enabled = false;
    bool ring_watch_restore_verify_pending = false;
    bool ring_watch_force_rw_mode = false;
  };
  RingBufferDebugState GetRingBufferDebugState() const;
  // Used by the GraphicsSystem to know if we've ever presented a frame.  The
  // flag itself is modified by specific command processor implementations
  // (e.g. Vulkan/Direct3D) when they actually issue the first swap.  Exposing
  // an accessor lets callers check without poking at private members.
  bool first_frame_rendered() const { return first_frame_rendered_; }
  Shader* active_vertex_shader() const { return active_vertex_shader_; }
  Shader* active_pixel_shader() const { return active_pixel_shader_; }

  virtual bool Initialize();
  virtual void Shutdown();

  void CallInThread(std::function<void()> fn, const char* tag = nullptr);

  virtual void ClearCaches();

  bool IsRingBufferInitialized() const;
  void ForceFallbackInitialization();
  void SetInterruptCallback(uint32_t callback, uint32_t user_data);

  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect GetDesiredSwapPostEffect() const {
    return swap_post_effect_desired_;
  }
  void SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect);
  // Implementations must not make assumptions that the front buffer will
  // necessarily be a resolve destination - it may be a texture generated by any
  // means like written to by the CPU or loaded from a file (the disclaimer
  // screen right in the beginning of 4D530AA4 is not a resolved render target,
  // for instance).
  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                         uint32_t frontbuffer_height) {}
  // Debug hook for forcing a swap from the host side (no-op by default).
  virtual void DebugIssueSwapFromHost(uint32_t width, uint32_t height) {}

  // May be called not only from the command processor thread when the command
  // processor is paused, and the termination of this function may be explicitly
  // awaited.
  virtual void InitializeShaderStorage(const std::filesystem::path& cache_root,
                                       uint32_t title_id, bool blocking);

  virtual void RequestFrameTrace(const std::filesystem::path& root_path);
  virtual void BeginTracing(const std::filesystem::path& root_path);
  virtual void EndTracing();

  virtual void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) = 0;

  void RestoreRegisters(uint32_t first_register,
                        const uint32_t* register_values,
                        uint32_t register_count, bool execute_callbacks);
  void RestoreGammaRamp(
      const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
      const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
      uint32_t new_gamma_ramp_rw_component);
  virtual void RestoreEdramSnapshot(const void* snapshot) = 0;

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);

  void UpdateWritePointer(uint32_t value, const char* source = nullptr);

  void LogRegisterSet(uint32_t register_index, uint32_t value);
  void LogRegisterSets(uint32_t base_register_index, const uint32_t* values,
                       uint32_t n_values);
  void NotifySwapPacket(uint32_t frontbuffer_ptr, uint32_t width,
                        uint32_t height);
  void RecordDebugHostSwapInjection(uint32_t frontbuffer_ptr, uint32_t width,
                                    uint32_t height, bool ring_injected);

  // Debug: ring buffer write watch via physical memory callbacks.
  void EnableRingBufferWriteWatch(const char* reason);
  void OnRingBufferWriteWatch(uint32_t physical_address_start, uint32_t length,
                              bool exact_range) {
    HandleRingBufferWriteWatch(physical_address_start, length, exact_range);
  }

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

  // Public access to worker thread status for debugging
  bool is_worker_thread_running() const { return worker_running_; }

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

 protected:
  struct IndexBufferInfo {
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
    uint32_t count = 0;
    uint32_t guest_base = 0;
    size_t length = 0;
  };

  static constexpr uint32_t kReadbackBufferSizeIncrement = 16 * 1024 * 1024;

  // Eviction policy constants for readback buffer cache
  static constexpr size_t kMaxReadbackBuffers = 64;
  static constexpr uint64_t kReadbackBufferEvictionAgeFrames = 60;

  // Progressive alignment for readback buffers to avoid wasting memory
  static inline uint32_t AlignReadbackBufferSize(uint32_t size) {
    if (size < 1 * 1024 * 1024) {
      return xe::align(size, 256u * 1024u);  // 256KB for < 1MB
    } else if (size < 4 * 1024 * 1024) {
      return xe::align(size, 1u * 1024u * 1024u);  // 1MB for < 4MB
    } else {
      return xe::align(size, kReadbackBufferSizeIncrement);  // 16MB for >= 4MB
    }
  }

  // Generate a cache key for a specific resolve operation
  static inline uint64_t MakeReadbackResolveKey(uint32_t address,
                                                uint32_t length) {
    return (uint64_t(address) << 32) | uint64_t(length);
  }

  void WorkerThreadMain();
  void ExecutePendingFunctions();
  virtual bool SetupContext() = 0;
  virtual void ShutdownContext() = 0;
  // rarely needed, most register writes have no special logic here
  XE_NOINLINE
  void HandleSpecialRegisterWrite(uint32_t index, uint32_t value);
  void HandleRingBufferWriteWatch(uint32_t physical_address_start,
                                  uint32_t length, bool exact_range);
  void LogRingBufferProtectionState(const char* phase, uint32_t hit_phys,
                                    uint32_t hit_len);
  void LogWorkerPipelineChecklist(uint64_t loop_id, size_t pending_count);

  virtual void WriteRegister(uint32_t index, uint32_t value);

  // mem has big-endian register values
  XE_FORCEINLINE
  virtual void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                     uint32_t num_registers);

  XE_FORCEINLINE
  virtual void WriteRegisterRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                          uint32_t num_registers);

  XE_NOINLINE
  void WriteOneRegisterFromRing(
      uint32_t base,
      uint32_t
          num_times);  // repeatedly write a value to one register, presumably a
                       // register with special handling for writes

  void WriteALURangeFromRing(xe::RingBuffer* ring, uint32_t base,
                             uint32_t num_times);

  void WriteFetchRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                               uint32_t num_times);

  void WriteBoolRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                              uint32_t num_times);

  void WriteLoopRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                              uint32_t num_times);

  void WriteREGISTERSRangeFromRing(xe::RingBuffer* ring, uint32_t base,
                                   uint32_t num_times);

  void WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                            uint32_t num_registers);

  void WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                              uint32_t num_registers);

  void WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers);

  void WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers);

  void WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                  uint32_t num_registers);

  const reg::DC_LUT_30_COLOR* gamma_ramp_256_entry_table() const {
    return gamma_ramp_256_entry_table_;
  }
  const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl_rgb() const {
    return gamma_ramp_pwl_rgb_[0];
  }
  virtual void OnGammaRamp256EntryTableValueWritten() {}
  virtual void OnGammaRampPWLValueWritten() {}

  virtual void MakeCoherent();
  virtual void PrepareForWait();
  virtual void ReturnFromWait();

  virtual void OnPrimaryBufferEnd() {}

#include "pm4_command_processor_declare.h"

  virtual Shader* LoadShader(xenos::ShaderType shader_type,
                             uint32_t guest_address,
                             const uint32_t* host_address,
                             uint32_t dword_count) {
    return nullptr;
  }

  virtual bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                         IndexBufferInfo* index_buffer_info,
                         bool major_mode_explicit) {
    return false;
  }
  virtual bool IssueCopy() { return false; }

  // "Actual" is for the command processor thread, to be read by the
  // implementations.
  SwapPostEffect GetActualSwapPostEffect() const {
    return swap_post_effect_actual_;
  }

  virtual void InitializeTrace();

  Memory* memory_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  GraphicsSystem* graphics_system_ = nullptr;
  RegisterFile* XE_RESTRICT register_file_ = nullptr;

  TraceWriter trace_writer_;
  enum class TraceState {
    kDisabled,
    kStreaming,
    kSingleFrame,
  };
  TraceState trace_state_ = TraceState::kDisabled;
  std::filesystem::path trace_stream_path_;
  std::filesystem::path trace_frame_path_;

  std::atomic<bool> worker_running_;
  kernel::object_ref<kernel::XHostThread> worker_thread_;

  struct PendingFnEntry {
    std::function<void()> fn;
    std::string tag;
    uint64_t enqueue_ms = 0;
  };
  std::mutex pending_fns_mutex_;
  std::queue<PendingFnEntry> pending_fns_;

  // MicroEngine binary from PM4_ME_INIT
  std::vector<uint32_t> me_bin_;

  uint32_t counter_ = 0;
  std::atomic<uint64_t> swap_packet_count_{0};
  std::atomic<uint64_t> last_swap_packet_ms_{0};
  std::atomic<uint32_t> last_swap_frontbuffer_ptr_{0};
  std::atomic<uint32_t> last_swap_width_{0};
  std::atomic<uint32_t> last_swap_height_{0};
  std::atomic<uint64_t> debug_host_swap_injection_count_{0};
  std::atomic<uint64_t> debug_host_swap_ring_injection_count_{0};
  std::atomic<uint64_t> debug_host_swap_direct_issue_count_{0};
  std::atomic<uint64_t> debug_host_swap_last_ms_{0};
  std::atomic<uint32_t> debug_host_swap_last_frontbuffer_ptr_{0};
  std::atomic<uint32_t> debug_host_swap_last_width_{0};
  std::atomic<uint32_t> debug_host_swap_last_height_{0};
  bool first_frame_rendered_ =
      false;  // Track if first frame has been presented

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_ = 0;

  uint32_t read_ptr_index_ = 0;
  uint32_t read_ptr_update_freq_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;

  // Ring buffer write watch state (debug).
  void* ring_buffer_watch_handle_ = nullptr;
  std::atomic<uint64_t> ring_buffer_watch_hits_{0};
  std::atomic<uint64_t> ring_buffer_watch_arm_count_{0};
  std::atomic<uint64_t> ring_buffer_watch_last_arm_ms_{0};
  std::atomic<uint64_t> ring_buffer_last_write_ms_{0};
  std::atomic<uint32_t> ring_buffer_last_write_phys_{0};
  std::atomic<bool> ring_buffer_watch_rearm_requested_{false};
  std::atomic<uint64_t> ring_watch_last_transition_ms_{0};
  std::atomic<uint32_t> ring_watch_last_phase_id_{0};
  std::atomic<uint32_t> ring_watch_last_range_start_{0};
  std::atomic<uint32_t> ring_watch_last_range_size_{0};
  std::atomic<bool> ring_watch_last_guest_write_enabled_{false};
  std::atomic<bool> ring_watch_restore_verify_pending_{false};
  std::atomic<uint32_t> ring_watch_restore_verify_phys_{0};
  std::atomic<uint32_t> ring_watch_restore_verify_len_{0};
  std::atomic<bool> ring_watch_force_rw_mode_{false};

  std::unique_ptr<xe::threading::Event> write_ptr_index_event_;
  std::unique_ptr<xe::threading::Event> worker_setup_complete_event_;
  std::atomic<uint32_t> write_ptr_index_;
  std::atomic<bool> worker_setup_complete_{false};
  std::atomic<bool> worker_setup_failed_{false};
  std::atomic<bool> ring_buffer_initialized_{false};
  std::atomic<uint64_t> pipeline_check_loop_count_{0};
  std::atomic<uint64_t> pipeline_check_idle_loop_streak_{0};

  // Synchronization for ring buffer initialization
  std::mutex ring_buffer_ready_mutex_;
  std::condition_variable ring_buffer_ready_cv_;

  // Enhanced synchronization for macOS robustness
  std::atomic<bool> initialization_acknowledged_{false};
  std::unique_ptr<xe::threading::Event> initialization_complete_event_;
  std::atomic<uint64_t> init_request_count_{0};
  std::atomic<uint64_t> init_ack_transition_count_{0};
  std::atomic<uint64_t> last_init_request_ms_{0};
  std::atomic<uint64_t> last_init_signal_ms_{0};
  std::atomic<uint64_t> last_init_ack_ms_{0};

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_user_data_ = 0;

  // Pre-initialization state management
  void InitializeRingBufferState();

  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;

  bool paused_ = false;

  // By default (such as for tools), post-processing is disabled.
  // "Desired" is for the external thread managing the post-processing effect.
  SwapPostEffect swap_post_effect_desired_ = SwapPostEffect::kNone;
  SwapPostEffect swap_post_effect_actual_ = SwapPostEffect::kNone;

 private:
  reg::DC_LUT_30_COLOR gamma_ramp_256_entry_table_[256] = {};
  reg::DC_LUT_PWL_DATA gamma_ramp_pwl_rgb_[128][3] = {};
  uint32_t gamma_ramp_rw_component_ = 0;

  XE_NOINLINE XE_COLD void LogKickoffInitator(uint32_t value);
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_COMMAND_PROCESSOR_MAC_H_
