/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/command_processor.h"

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/sampler_info.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos_zpd_report.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#if !defined(NDEBUG)

#define XE_ENABLE_GPU_REG_WRITE_LOGGING 1
#endif
DEFINE_bool(
    log_guest_driven_gpu_register_written_values, false,
    "Only does anything in debug builds, if set will log every write to a gpu "
    "register done by a guest. Does not log writes that are done by the CP on "
    "its own, just ones the guest makes or instructs it to make.",
    "Logging");

DEFINE_bool(disassemble_pm4, false,
            "Only does anything in debug builds, if set will disassemble and "
            "log all PM4 packets sent to the CP.",
            "Logging");

DEFINE_bool(
    log_ringbuffer_kickoff_initiator_bts, false,
    "Only does anything in debug builds, if set will log the pseudo-stacktrace "
    "of the guest thread that wrote the new read position.",
    "Logging");

DEFINE_bool(clear_memory_page_state, false,
            "Refresh state of memory pages to enable gpu written data. (Use "
            "for 'Team Ninja' Games to fix missing character models)",
            "GPU");

DEFINE_string(
    occlusion_query, "fast",
    "Controls hardware occlusion query behavior for EVENT_WRITE_ZPD.\n"
    "Used for effects like lens flares, object culling, and auto-exposure.\n"
    " fake: Write a fake result without asking the GPU. Safe for most games,\n"
    "       though some effects may look slightly wrong.\n"
    " fast: Ask the GPU but don't wait for the answer. Writes a cached\n"
    "       result immediately and updates it when the GPU catches up.\n"
    "       Cached results bias toward visible when guessing. (default)\n"
    " fast-alt: Variant of fast mode that keeps cached zero results for\n"
    "           unresolved reports. May improve effects relying on precise\n"
    "           visibility, but may be less stable for occlusion culling.\n"
    " strict: Ask the GPU and wait for the real result before continuing.\n"
    "         Most accurate, but may be somewhat less performant.",
    "GPU");

DEFINE_bool(
    occlusion_query_full_counters, false,
    "Controls in-shader emulation of the ZFail, StencilFail and Total ZPD "
    "counters to supplement both native and counter-based ZPass testing.\n"
    "Most titles only use the ZPass counter, so this is off by default since "
    "it's typically slow and rife with readback sync.\n"
    "RTV/FBO approximates Total and ZFail, whereas ROV/FSI uses depth/stencil "
    "tests in-shader to count everything like Xenos does.",
    "GPU");

DEFINE_string(
    readback_resolve, "none",
    "Controls CPU readback of render-to-texture resolve results.\n"
    " fast: Read from previous frame (1 frame delay, no GPU stall, slight "
    "performance hit)\n"
    " full: Wait for GPU to finish (accurate but slow, GPU-CPU sync stall)\n"
    " none: Disable readback completely (some games render better without it)",
    "GPU");

UPDATE_from_string(readback_resolve, 2025, 12, 4, 21, "fast");

DEFINE_bool(
    readback_resolve_half_pixel_offset, false,
    "When resolution scaling and readback resolve are enabled, resolve "
    "downscaling keeps one supersample out of each scaled pixel block.\n"
    "This selects the center of the block instead of the top-left corner, "
    "which can be closer to where the GPU would have sampled the original "
    "pixel, and can help with thin features and edge coverage.\n"
    "Both choices give a real rendered value, but they can disagree where "
    "geometry edges fall inside a pixel block.\n"
    "This matters when the CPU reads the resolve as data instead of an image, "
    "such as for gamma correction readbacks, occlusion checks, and GPU buffers "
    "reused later in the frame.",
    "GPU");

DEFINE_bool(
    readback_memexport, false,
    "Read data written by memory export in shaders on the CPU. "
    "This may be needed in some games (but many only access exported data on "
    "the GPU, and this flag isn't needed to handle such behavior), but causes "
    "mid-frame synchronization, so it has a huge performance impact.",
    "GPU");

namespace xe {
namespace gpu {

// This should be written completely differently with support for different
// types.
void SaveGPUSetting(GPUSetting setting, uint64_t value) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      OVERRIDE_bool(clear_memory_page_state, static_cast<bool>(value));
      break;
    case GPUSetting::ReadbackMemexport:
      OVERRIDE_bool(readback_memexport, static_cast<bool>(value));
      break;
  }
}

bool GetGPUSetting(GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      return cvars::clear_memory_page_state;
    case GPUSetting::ReadbackMemexport:
      return cvars::readback_memexport;
  }
  return false;
}

ReadbackResolveMode GetReadbackResolveMode() {
  const std::string& mode = cvars::readback_resolve;
  if (mode == "full") {
    return ReadbackResolveMode::kFull;
  } else if (mode == "none") {
    return ReadbackResolveMode::kDisabled;
  } else {
    // Default to "fast" for any unrecognized value
    return ReadbackResolveMode::kFast;
  }
}

void SetReadbackResolveMode(const std::string& mode) {
  OVERRIDE_string(readback_resolve, mode);
}

ZPDMode GetZPDMode() {
  const std::string& mode = cvars::occlusion_query;
  if (mode == "fake") {
    return ZPDMode::kFake;
  } else if (mode == "strict") {
    return ZPDMode::kStrict;
  } else if (mode == "fast-alt") {
    return ZPDMode::kFastAlt;
  }
  return ZPDMode::kFast;
}

void SetZPDMode(const std::string& mode) {
  OVERRIDE_string(occlusion_query, mode);
}

using namespace xe::gpu::xenos;

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   kernel::KernelState* kernel_state)
    : reader_(nullptr, 0),
      memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      trace_writer_(graphics_system->memory()->physical_membase()),
      worker_running_(true),
      write_ptr_index_event_(xe::threading::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state_->GetIdleProcess()));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  EndTracing();

  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  if (completion_callback) {
    completion_callback();
  }
}

void CommandProcessor::RequestFrameTrace(
    const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    XELOGE("Streaming trace; cannot also trace frame.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    XELOGE("Frame trace already pending; ignoring.");
    return;
  }
  trace_state_ = TraceState::kSingleFrame;
  trace_frame_path_ = root_path;
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    XELOGE("Streaming already active; ignoring request.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    XELOGE("Frame trace pending; ignoring streaming request.");
    return;
  }
  // Streaming starts on the next primary buffer execute.
  trace_state_ = TraceState::kStreaming;
  trace_stream_path_ = root_path;
}

void CommandProcessor::EndTracing() {
  if (!trace_writer_.is_open()) {
    return;
  }
  assert_true(trace_state_ == TraceState::kStreaming);
  trace_state_ = TraceState::kDisabled;
  trace_writer_.Close();
}

void CommandProcessor::RestoreRegisters(uint32_t first_register,
                                        const uint32_t* register_values,
                                        uint32_t register_count,
                                        bool execute_callbacks) {
  if (first_register > RegisterFile::kRegisterCount ||
      RegisterFile::kRegisterCount - first_register < register_count) {
    XELOGW(
        "CommandProcessor::RestoreRegisters out of bounds (0x{:X} registers "
        "starting with 0x{:X}, while a total of 0x{:X} registers are stored)",
        register_count, first_register, RegisterFile::kRegisterCount);
    if (first_register > RegisterFile::kRegisterCount) {
      return;
    }
    register_count =
        std::min(uint32_t(RegisterFile::kRegisterCount) - first_register,
                 register_count);
  }
  if (execute_callbacks) {
    for (uint32_t i = 0; i < register_count; ++i) {
      WriteRegister(first_register + i, register_values[i]);
    }
  } else {
    std::memcpy(register_file_->values + first_register, register_values,
                sizeof(uint32_t) * register_count);
  }
}

void CommandProcessor::RestoreGammaRamp(
    const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
    const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
    uint32_t new_gamma_ramp_rw_component) {
  std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
              sizeof(reg::DC_LUT_30_COLOR) * 256);
  std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb,
              sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
  OnGammaRamp256EntryTableValueWritten();
  OnGammaRampPWLValueWritten();
}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() &&
      kernel::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::SetDesiredSwapPostEffect(
    SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() {
    swap_post_effect_actual_ = swap_post_effect;
  });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    xe::FatalError("Unable to setup command processor internal state");
    return;
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "xe::gpu::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          constexpr int wait_time_ms = 2;
          xe::threading::Wait(write_ptr_index_event_.get(), true,
                              std::chrono::milliseconds(wait_time_ms));
          // Strict ZPD may still owe the guest a report it's spinning on with
          // nothing left in the ring.
          if (zpd_mode_ == ZPDMode::kStrict && zpd_awaited_report_count_) {
            PrepareForWait();
          }
        } else {
          xe::threading::MaybeYield();
        }
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D ||
                read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    // Keep in mind that the gpu also updates the cpu-side copy if the write
    // pointer and read pointer would be equal
    if (read_ptr_writeback_ptr_) {
      xe::store_and_swap<uint32_t>(
          memory_->TranslatePhysical(read_ptr_writeback_ptr_), read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  threading::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    threading::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  ResetZPDState();
  return true;
}

void CommandProcessor::ShutdownContext() { ResetZPDState(); }

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);

  std::memset(kernel_state_->memory()->TranslatePhysical(primary_buffer_ptr_),
              0, primary_buffer_size_);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr,
                                                  uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

XE_NOINLINE XE_COLD void CommandProcessor::LogKickoffInitator(uint32_t value) {
  cpu::backend::GuestPseudoStackTrace st;

  if (logging::ShouldLog(LogLevel::Debug) &&
      kernel_state_->processor()->backend()->PopulatePseudoStacktrace(&st)) {
    logging::LoggerBatch<LogLevel::Debug> log_initiator{};

    log_initiator("Updating read ptr to {}, initiator stacktrace below\n",
                  value);

    for (uint32_t i = 0; i < st.count; ++i) {
      log_initiator("\t{:08X}\n", st.return_addrs[i]);
    }

    if (st.truncated_flag) {
      log_initiator("\t(Truncated stacktrace to {} entries)\n",
                    cpu::backend::MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES);
    }
    log_initiator.submit('d');
  }
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  XE_UNLIKELY_IF(cvars::log_ringbuffer_kickoff_initiator_bts) {
    LogKickoffInitator(value);
  }
  write_ptr_index_ = value;
  write_ptr_index_event_->SetBoostPriority();
}

void CommandProcessor::LogRegisterSet(uint32_t register_index, uint32_t value) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    const RegisterInfo* reginfo = RegisterFile::GetRegisterInfo(register_index);

    if (!reginfo) {
      XELOGD("Unknown_Reg{:04X} <- {:08X}\n", register_index, value);
    } else {
      XELOGD("{} <- {:08X}\n", reginfo->name, value);
    }
  }
#endif
}

void CommandProcessor::LogRegisterSets(uint32_t base_register_index,
                                       const uint32_t* values,
                                       uint32_t n_values) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    auto target = logging::internal::GetThreadBuffer();

    auto target_ptr = target.first;

    size_t total_size = 0;

    size_t rem_size = target.second;

    for (uint32_t i = 0; i < n_values; ++i) {
      uint32_t register_index = base_register_index + i;

      uint32_t value = xe::load_and_swap<uint32_t>(&values[i]);

      const RegisterInfo* reginfo =
          RegisterFile::GetRegisterInfo(register_index);

      if (!reginfo) {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size,
                                       "Unknown_Reg{:04X} <- {:08X}\n",
                                       register_index, value);
        target_ptr = tmpres.out;
        rem_size -= tmpres.size;
        total_size += tmpres.size;

      } else {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size, "{} <- {:08X}\n",
                                       reginfo->name, value);
        rem_size -= tmpres.size;
        target_ptr = tmpres.out;
        total_size += tmpres.size;
      }
    }
    logging::internal::AppendLogLine(LogLevel::Debug, 'd', total_size);
  }
#endif
}

void CommandProcessor::HandleSpecialRegisterWrite(uint32_t index,
                                                  uint32_t value) {
  RegisterFile& regs = *register_file_;
  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_MEM_REGs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        regs.values[index] |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color =
              regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl]
                                 [gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index =
              (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
              ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask =
            regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(
            XE_GPU_REG_DC_LUT_RW_INDEX,
            xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}
void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // chrispy: rearrange check order, place set after checks

  if (XE_LIKELY(index < RegisterFile::kRegisterCount)) {
    register_file_->values[index] = value;

    // quick pre-test
    // todo: figure out just how unlikely this is. if very (it ought to be,
    // theres a ton of registers other than these) make this predicate
    // branchless and mark with unlikely, then make HandleSpecialRegisterWrite
    // noinline yep, its very unlikely. these ORS here are meant to be bitwise
    // ors, so that we do not do branching evaluation of the conditions (we will
    // almost always take all of the branches)

    unsigned expr = (index - XE_GPU_REG_SCRATCH_REG0 < 8) |
                    (index == XE_GPU_REG_COHER_STATUS_HOST) |
                    ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
                     (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));
    // chrispy: reordered for msvc branch probability (assumes if is taken and
    // else is not)
    if (XE_LIKELY(expr == 0)) {
      XE_MSVC_REORDER_BARRIER();

    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  } else {
    XELOGW("CommandProcessor::WriteRegister index out of bounds: {}", index);
    return;
  }
}
void CommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = xe::load_and_swap<uint32_t>(base + i);
    this->WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                  uint32_t base,
                                                  uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = ring->ReadAndSwap<uint32_t>();
    WriteRegister(base + i, data);
  }
}

void CommandProcessor::WriteALURangeFromRing(xe::RingBuffer* ring,
                                             uint32_t base,
                                             uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_times);
}

void CommandProcessor::WriteFetchRangeFromRing(xe::RingBuffer* ring,
                                               uint32_t base,
                                               uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_times);
}

void CommandProcessor::WriteBoolRangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_times);
}

void CommandProcessor::WriteLoopRangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_times);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_times);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index,
                                            uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}
XE_NOINLINE
void CommandProcessor::WriteOneRegisterFromRing(uint32_t base,
                                                uint32_t num_times) {
  for (uint32_t m = 0; m < num_times; m++) {
    uint32_t reg_data = reader_.ReadAndSwap<uint32_t>();
    uint32_t target_index = base;
    WriteRegister(target_index, reg_data);
  }
}
void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = xe::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  XELOGGPU("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
           base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {
  trace_writer_.Flush();
  if (zpd_mode_ == ZPDMode::kStrict && zpd_awaited_report_count_) {
    PrepareZPDForWait();
  }
}

void CommandProcessor::ReturnFromWait() {}

void CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(
      0, reinterpret_cast<const uint32_t*>(register_file_->values),
      RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(),
                               gamma_ramp_pwl_rgb(), gamma_ramp_rw_component_);
}

// Only called by EVENT_WRITE_ZPD. This closes the query interval since the last
// event and queues its counter snapshot.
void CommandProcessor::QueueZPDReport(uint32_t report_address) {
  CloseQuerySegment();

  ZPDReport& report = zpd_current_report_;
  report.address = report_address;
  if (zpd_mode_ == ZPDMode::kStrict) {
    // See EVENT_WRITE_ZPD for additional information on the pending sentinel.
    const uint32_t kPendingSentinel = xe::byte_swap(0xFFFFFEEDu);
    const auto* guest =
        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
            report_address);
    report.awaited = guest->ZPass_A == kPendingSentinel ||
                     guest->ZFail_A == kPendingSentinel;
  } else {
    // Fast modes write a guess now and correct it when the real delta lands.
    // Unknown still means visible. Replaying the last real delta for the same
    // report is usually a better guess than one fake sample. fast-alt is the
    // same as fast, but can replay zeroes, which often improves correctness
    // (545107FC, 454108D4, 4D5307D2), but stale zeroes tend to break occlusion
    // culling tests, resulting in popping primitives (4D5308AB, 4D530805).
    auto cache_it = fast_zpd_report_cached_deltas_.find(report_address);
    if (cache_it != fast_zpd_report_cached_deltas_.end() &&
        (cache_it->second.z_pass || zpd_mode_ == ZPDMode::kFastAlt)) {
      report.speculative_delta = cache_it->second;
    } else {
      report.speculative_delta = XenosZPDReport::FromNativeQuery(1);
    }
    zpd_speculative_sample_counter_ += report.speculative_delta;
    report.speculative_value = zpd_speculative_sample_counter_;
    report.speculative = true;
    WriteZPDReport(report_address, report.speculative_value);
  }
  zpd_awaited_report_count_ += report.awaited;
  zpd_reports_.push_back(report);

  // The next report's segment opens at its first draw.
  // Report runs without draws between them never use any pool slots.
  zpd_current_report_ = {};
  zpd_current_report_.handle = zpd_next_report_handle_++;
  zpd_active_segment_ = {};
  zpd_active_segment_.segment_pending_begin = true;
}

void CommandProcessor::OpenQuerySegment(bool can_close_submission) {
  if (zpd_current_report_.handle == kInvalidReportHandle ||
      !zpd_active_segment_.segment_pending_begin || !CanOpenZPDQuery()) {
    return;
  }

  EnsureZPDQueryResources();
  if (!IsZPDQueryPoolReady()) {
    // Fall back to fake results for the rest of the session.
    zpd_force_fake_fallback_ = true;
    zpd_current_report_ = {};
    zpd_active_segment_ = {};
    return;
  }

  // Frees any slots from completed submissions before asking for new ones.
  PumpQueryResolves();

  QueryOpenResult result = OpenZPDQuery(can_close_submission);
  if (result == QueryOpenResult::kPoolExhausted &&
      zpd_mode_ != ZPDMode::kStrict) {
    // Fast modes favor forward progress over accuracy. Report at least one
    // passing sample instead of waiting for a slot to become available.
    zpd_current_report_.delta.z_pass =
        std::max<uint64_t>(zpd_current_report_.delta.z_pass, 1);
    zpd_active_segment_.segment_pending_begin = false;
    return;
  }
  if (result != QueryOpenResult::kOpened) {
    return;
  }
  zpd_active_segment_.segment_active = true;
  zpd_active_segment_.segment_pending_begin = false;
}

// Closes the active host segment without ending the report.
// BeginQuery/EndQuery can't cross D3D12 command list or Vulkan render pass
// boundaries. The result accumulates across all pieces.
void CommandProcessor::CloseQuerySegment() {
  if (!zpd_active_segment_.segment_active) {
    return;
  }
  uint64_t submission = 0;
  if (CloseZPDQuery(zpd_current_report_.handle, submission)) {
    zpd_current_report_.pending_segments++;
    zpd_current_report_.last_segment_end_submission = submission;
  }
  zpd_active_segment_ = {};
  zpd_active_segment_.segment_pending_begin = true;
}

void CommandProcessor::UpdateZPDSegment(uint32_t scale_area, bool count_total) {
  if (zpd_current_report_.handle == kInvalidReportHandle) {
    return;
  }
  if (zpd_active_segment_.segment_active &&
      ((zpd_active_segment_.scale_area &&
        zpd_active_segment_.scale_area != scale_area) ||
       zpd_active_segment_.count_total != count_total)) {
    // Draw scale or hybrid Total counting changed in the middle of a report,
    // so close the segment and start a fresh one for this draw.
    CloseQuerySegment();
  }

  zpd_active_segment_.scale_area = scale_area;
  zpd_active_segment_.count_total = count_total;

  if (zpd_active_segment_.segment_pending_begin) {
    OpenQuerySegment(false);
  }
}

void CommandProcessor::OnZPDQueryResolved(ReportHandle report_handle,
                                          const XenosZPDReport& raw_counts,
                                          uint32_t scale_area) {
  ZPDReport* report = FindZPDReport(report_handle);
  if (!report) {
    return;
  }
  assert_true(report->pending_segments);
  --report->pending_segments;
  report->delta += raw_counts.Normalized(scale_area);
}

CommandProcessor::ZPDReport* CommandProcessor::FindZPDReport(
    ReportHandle report_handle) {
  if (report_handle == kInvalidReportHandle) {
    return nullptr;
  }
  if (zpd_current_report_.handle == report_handle) {
    return &zpd_current_report_;
  }
  if (!zpd_reports_.empty() && report_handle >= zpd_reports_.front().handle) {
    size_t index = size_t(report_handle - zpd_reports_.front().handle);
    if (index < zpd_reports_.size()) {
      assert_true(zpd_reports_[index].handle == report_handle);
      return &zpd_reports_[index];
    }
  }
  return nullptr;
}

void CommandProcessor::PrepareZPDForWait() {
  ReportHandle awaited_handle = kInvalidReportHandle;
  for (const ZPDReport& report : zpd_reports_) {
    if (report.awaited) {
      awaited_handle = report.handle;
      break;
    }
  }
  if (awaited_handle == kInvalidReportHandle) {
    return;
  }

  PollCompletedSubmission();
  PumpPendingRetire();

  // Draw-less queries still can't be written until the reports ahead resolve.
  ZPDReport* wait_report = FindZPDReport(awaited_handle);
  if (wait_report && !wait_report->pending_segments &&
      zpd_reports_.front().pending_segments) {
    wait_report = &zpd_reports_.front();
  }
  if (!wait_report || !wait_report->pending_segments) {
    return;
  }
  if (AwaitQueryResolve(wait_report->handle,
                        wait_report->last_segment_end_submission)) {
    PumpPendingRetire();
    return;
  }

  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (!zpd_pending_retire_start_ms_) {
    zpd_pending_retire_start_ms_ = now_ms;
    return;
  }
  if (now_ms - zpd_pending_retire_start_ms_ < kStrictZPDRetireDeadlineMs) {
    return;
  }

  ZPDReport& front = zpd_reports_.front();
  // Keep what resolved, with a floor of one so culling doesn't flash occluded.
  front.delta.z_pass = std::max<uint64_t>(front.delta.z_pass, 1);
  front.pending_segments = 0;
  PumpPendingRetire();
}

void CommandProcessor::PumpPendingRetire() {
  bool rebase_needed = false;
  while (!zpd_reports_.empty()) {
    ZPDReport& front = zpd_reports_.front();
    if (front.pending_segments) {
      if (zpd_mode_ == ZPDMode::kStrict) {
        break;
      }
      // A stuck front report would block everything behind it.
      uint64_t now_ms = Clock::QueryHostUptimeMillis();
      if (!zpd_pending_retire_start_ms_) {
        zpd_pending_retire_start_ms_ = now_ms;
        break;
      }
      if (now_ms - zpd_pending_retire_start_ms_ < kFastZPDRetireDeadlineMs) {
        break;
      }
      front.delta.z_pass = std::max<uint64_t>(front.delta.z_pass, 1);
      front.pending_segments = 0;
    }
    zpd_pending_retire_start_ms_ = 0;

    zpd_sample_counter_ += front.delta;
    if (front.speculative) {
      if (fast_zpd_report_cached_deltas_.size() >= kFastZPDCacheMaxEntries &&
          !fast_zpd_report_cached_deltas_.count(front.address)) {
        fast_zpd_report_cached_deltas_.clear();
      }
      fast_zpd_report_cached_deltas_[front.address] = front.delta;
    }
    if (!front.speculative) {
      WriteZPDReport(front.address, zpd_sample_counter_);
    } else if (front.speculative_value != zpd_sample_counter_) {
      WriteZPDReport(front.address, zpd_sample_counter_);
      rebase_needed = true;
    }
    if (front.awaited) {
      assert_true(zpd_awaited_report_count_);
      --zpd_awaited_report_count_;
    }
    zpd_reports_.pop_front();
  }

  if (rebase_needed) {
    XenosZPDReport running = zpd_sample_counter_;
    for (ZPDReport& report : zpd_reports_) {
      assert_true(report.speculative);
      running += report.speculative_delta;
      if (report.speculative_value != running) {
        WriteZPDReport(report.address, running);
        report.speculative_value = running;
      }
    }
    zpd_speculative_sample_counter_ = running;
  } else if (zpd_reports_.empty()) {
    zpd_speculative_sample_counter_ = zpd_sample_counter_;
  }
}

#define COMMAND_PROCESSOR CommandProcessor
#include "pm4_command_processor_implement.h"
}  // namespace gpu
}  // namespace xe
