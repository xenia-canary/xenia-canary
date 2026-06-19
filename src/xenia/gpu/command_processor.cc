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
    "GPU");

DEFINE_bool(disassemble_pm4, false,
            "Only does anything in debug builds, if set will disassemble and "
            "log all PM4 packets sent to the CP.",
            "GPU");

DEFINE_bool(
    log_ringbuffer_kickoff_initiator_bts, false,
    "Only does anything in debug builds, if set will log the pseudo-stacktrace "
    "of the guest thread that wrote the new read position.",
    "GPU");

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
  // Only refresh completion if there is a strict ZPD retire pending so
  // PumpPendingRetire sees the latest progress without adding extra overhead.
  if (zpd_pending_retire_handle_ != kInvalidReportHandle) {
    PollCompletedSubmission();
  }
  // Give strict ZPD a chance to retire a pending report before the guest's
  // loop polls again.
  PumpPendingRetire();
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

CommandProcessor::PendingZPDSlot CommandProcessor::GetPendingZPDSlot(
    uint32_t slot_base, uint32_t end_record) const {
  PendingZPDSlot pending_slot;

  for (const auto& report_pair : logical_zpd_reports_) {
    const ZPDReport& report = report_pair.second;
    if (!report.ended || report.pending_segments == 0 ||
        report.slot_base != slot_base) {
      continue;
    }

    // Wait on the oldest unresolved report for this slot first.
    if (pending_slot.report_handle == kInvalidReportHandle ||
        report_pair.first < pending_slot.report_handle) {
      pending_slot.report_handle = report_pair.first;
    }

    // Slot reuse needs to be handled carefully in fast mode. Keep the biggest
    // cached delta, not the newest one. A stale zero is a lot more dangerous
    // than a stale nonzero.
    if (report.has_cached_delta) {
      if (!pending_slot.has_cached_delta ||
          report.cached_delta > pending_slot.cached_delta) {
        pending_slot.cached_delta = report.cached_delta;
      }
      pending_slot.has_cached_delta = true;
    }

    if (report.end_record) {
      auto report_cache_it =
          fast_zpd_report_cached_values_.find(report.end_record);
      if (report_cache_it != fast_zpd_report_cached_values_.end()) {
        if (!pending_slot.has_cached_delta ||
            report_cache_it->second > pending_slot.cached_delta) {
          pending_slot.cached_delta = report_cache_it->second;
        }
        pending_slot.has_cached_delta = true;
      }
    }
  }

  auto end_record_cache_it = fast_zpd_report_cached_values_.find(end_record);
  if (end_record_cache_it != fast_zpd_report_cached_values_.end()) {
    if (!pending_slot.has_cached_delta ||
        end_record_cache_it->second > pending_slot.cached_delta) {
      pending_slot.cached_delta = end_record_cache_it->second;
    }
    pending_slot.has_cached_delta = true;
  }

  return pending_slot;
}

bool CommandProcessor::BeginZPDReport(uint32_t report_address) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  // Track any delta to carry forward if the same slot is immediately reused.
  uint32_t carried_cached_delta = 0;
  bool has_carried_cached_delta = false;
  uint32_t carried_from_slot_base = 0;

  if (zpd_active_segment_.logical_active) {
    // New BEGIN while a prior report is open. Hardware has one register for
    // the query address, so a new BEGIN implicitly ends the prior one.
    if (zpd_active_segment_.end_record) {
      EndZPDReport(zpd_active_segment_.end_record, true);
    } else {
      carried_from_slot_base = zpd_active_segment_.slot_base;

      auto dying_report =
          logical_zpd_reports_.find(zpd_active_segment_.report_handle);
      if (dying_report != logical_zpd_reports_.end() &&
          dying_report->second.has_cached_delta) {
        carried_cached_delta = dying_report->second.cached_delta;
        has_carried_cached_delta = true;
      }

      if (zpd_active_segment_.segment_active) {
        // Deactivate the segment before DiscardZPDQuery so that
        // EndSubmission -> CloseQuerySegment does not re-enter and
        // issue a second EndQuery on the same slot.
        zpd_active_segment_.segment_active = false;
        DiscardZPDQuery();
      }
      logical_zpd_reports_.erase(zpd_active_segment_.report_handle);
      zpd_active_segment_ = {};
    }
  }

  uint32_t slot_base = XenosZPDReport::GetSlotBase(report_address);
  uint32_t begin_record = XenosZPDReport::GetBeginRecordBase(slot_base);
  uint32_t end_record = XenosZPDReport::GetEndRecordBase(slot_base);
  if (!slot_base) {
    return false;
  }

  // Resolve same slot hazards before invalidating pending writes from the prior
  // lifetime. For finished strict queries with unpolled completion, refresh now
  // and drain, avoiding unnecessary AwaitQueryResolve blocking.
  if (GetZPDMode() == ZPDMode::kStrict) {
    PollCompletedSubmission();
  } else {
    PumpQueryResolves();
  }

  PendingZPDSlot pending_slot = GetPendingZPDSlot(slot_base, end_record);

  if (pending_slot.report_handle != kInvalidReportHandle) {
    if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
      if (pending_slot.has_cached_delta) {
        carried_cached_delta = pending_slot.cached_delta;
        has_carried_cached_delta = true;
        carried_from_slot_base = slot_base;
      }
    } else {
      while (pending_slot.report_handle != kInvalidReportHandle) {
        auto report_it = logical_zpd_reports_.find(pending_slot.report_handle);
        if (report_it == logical_zpd_reports_.end()) {
          break;
        }

        uint64_t wait_for_submission =
            report_it->second.last_segment_end_submission;

        bool wait_succeeded =
            AwaitQueryResolve(pending_slot.report_handle, wait_for_submission);

        if (!wait_succeeded) {
          if (pending_slot.cached_delta != 0) {
            carried_cached_delta = pending_slot.cached_delta;
            carried_from_slot_base = slot_base;
          }
          break;
        }

        PumpQueryResolves();

        pending_slot = GetPendingZPDSlot(slot_base, end_record);
      }
    }
  }

  // Bump slot sequence - invalidates pending writes from prior lifetime.
  uint64_t slot_sequence_id = ++zpd_slot_sequences_[slot_base];

  // By default, BEGIN drops the cached value so an orphaned END doesn't replay
  // something from a prior lifetime. The alternate fast path keeps it around
  // long enough for an async zero to help the next unresolved write.
  if (GetZPDMode() != ZPDMode::kFastAlt) {
    auto cache_it = fast_zpd_report_cached_values_.find(end_record);
    if (cache_it != fast_zpd_report_cached_values_.end() &&
        cache_it->second == 0) {
      fast_zpd_report_cached_values_.erase(cache_it);
    }
  }

  ReportHandle report_handle = zpd_next_report_handle_++;
  if (report_handle == kInvalidReportHandle) {
    report_handle = zpd_next_report_handle_++;
  }

  ZPDReport& logical = logical_zpd_reports_[report_handle];
  logical.slot_base = slot_base;
  logical.slot_sequence_id = slot_sequence_id;
  logical.begin_record = begin_record;
  logical.end_record = end_record;
  logical.begin_value = zpd_slot_values_[slot_base];
  logical.accumulated_samples = 0;
  logical.first_segment_end_submission = 0;
  logical.last_segment_end_submission = 0;
  logical.pending_segments = 0;
  logical.cached_delta = 0;
  logical.has_cached_delta = false;
  logical.ended = false;

  if (slot_base == carried_from_slot_base && has_carried_cached_delta) {
    logical.cached_delta = carried_cached_delta;
    logical.has_cached_delta = true;
  }

  zpd_active_segment_.report_handle = report_handle;
  zpd_active_segment_.slot_base = slot_base;
  zpd_active_segment_.begin_record = begin_record;
  zpd_active_segment_.end_record = end_record;
  zpd_active_segment_.segment_active = false;
  // Opens lazily. OpenQuerySegment will open it at the next valid opportunity.
  zpd_active_segment_.segment_pending_begin = true;
  zpd_active_segment_.logical_active = true;

  OpenQuerySegment(true);
  return true;
}

// Guest END closes the logical lifetime, but the final value may still depend
// on in flight query segments.
bool CommandProcessor::EndZPDReport(uint32_t report_address,
                                    bool guest_forced_end) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  CommandProcessor::ReportHandle report_handle =
      zpd_active_segment_.report_handle;
  uint32_t stored_end_record = zpd_active_segment_.end_record;
  uint32_t report_record_base = XenosZPDReport::GetRecordBase(report_address);
  if (!report_record_base) {
    report_record_base = stored_end_record;
  }

  if (zpd_active_segment_.segment_active) {
    CloseQuerySegment();
  }

  zpd_active_segment_.segment_pending_begin = false;

  if (!report_record_base) {
    logical_zpd_reports_.erase(report_handle);
    zpd_active_segment_ = {};
    return false;
  }

  bool resolved_immediately = false;
  uint32_t begin_record = 0;
  uint32_t begin_value = 0;
  uint32_t final_value = 0;
  uint32_t cached_delta = 0;
  bool has_cached_delta = false;

  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    zpd_active_segment_ = {};
    return false;
  }

  ZPDReport& logical = it->second;
  logical.ended = true;
  logical.end_record = report_record_base;
  begin_record = logical.begin_record;
  begin_value = logical.begin_value;

  if (logical.pending_segments == 0) {
    resolved_immediately = true;
    final_value = NormalizeSampleCount(logical.accumulated_samples);

    cached_delta = final_value;
    has_cached_delta = true;
    logical.cached_delta = cached_delta;
    logical.has_cached_delta = true;
    if (fast_zpd_report_cached_values_.size() >= kFastZPDCacheMaxEntries &&
        !fast_zpd_report_cached_values_.count(report_record_base)) {
      fast_zpd_report_cached_values_.clear();
    }
    fast_zpd_report_cached_values_[report_record_base] = cached_delta;
    final_value = cached_delta;
  } else {
    if (logical.has_cached_delta) {
      cached_delta = logical.cached_delta;
      has_cached_delta = true;
    }
    auto cache_it = fast_zpd_report_cached_values_.find(report_record_base);
    if (cache_it != fast_zpd_report_cached_values_.end()) {
      cached_delta = cache_it->second;
      has_cached_delta = true;
    }
  }

  if (resolved_immediately) {
    CommitZPDReport(logical, final_value);
    logical_zpd_reports_.erase(it);
  }

  bool has_cross_slot_end =
      stored_end_record && stored_end_record != report_record_base;
  if (has_cross_slot_end) {
    // The guest ended a different ZPD record than the one opened by BEGIN.
    // Preserve the prior running slot value in the stored END record before
    // writing the actual END selected by the packet.
    WriteZPDReport(0, stored_end_record, 0, begin_value, false);
  }

  if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
    bool write_begin = begin_record && report_record_base &&
                       begin_record != report_record_base;
    // Unknown still means visible in fast mode. Reusing cached zeroes can help
    // flares stop shining through walls (545107FC, 454108D4, 4D5307D2), but it
    // also tends to break occlusion culling (4D5308AB, 4D530805), so only do it
    // in the alternate fast path.
    uint32_t speculative = cached_delta;
    if (!resolved_immediately) {
      speculative = 1;
      if (has_cached_delta &&
          (cached_delta != 0 || GetZPDMode() == ZPDMode::kFastAlt)) {
        speculative = cached_delta;
      }
    }
    WriteZPDReport(begin_record, report_record_base, begin_value, speculative,
                   write_begin);
  } else if (!resolved_immediately) {
    PumpQueryResolves();

    // Recheck after the drain. The report may have resolved synchronously if
    // all segments were already complete by the time we got here.
    if (!logical_zpd_reports_.count(report_handle)) {
      // OnZPDQueryResolved already committed and erased the report; nothing
      // left to defer.
    } else if (zpd_pending_retire_handle_ != report_handle) {
      zpd_pending_retire_handle_ = report_handle;
      zpd_pending_retire_stalls_ = 0;
      zpd_pending_retire_start_ms_ = Clock::QueryHostUptimeMillis();
    }
  }

  zpd_active_segment_ = {};
  return true;
}

void CommandProcessor::OpenQuerySegment(bool can_close_submission) {
  if (GetZPDMode() == ZPDMode::kFake || zpd_force_fake_fallback_ ||
      !zpd_active_segment_.logical_active ||
      !zpd_active_segment_.segment_pending_begin || !CanOpenZPDQuery()) {
    return;
  }

  EnsureZPDQueryResources();

  // Resource setup failed. Drop the logical report and fall back to fake mode.
  if (!IsZPDQueryPoolReady()) {
    zpd_force_fake_fallback_ = true;
    logical_zpd_reports_.erase(zpd_active_segment_.report_handle);
    zpd_active_segment_ = {};
    return;
  }

  // Frees any slots from completed submissions before asking for new ones.
  PumpQueryResolves();

  QueryOpenResult open_result =
      OpenZPDQuery(zpd_active_segment_.report_handle, can_close_submission);
  switch (open_result) {
    case QueryOpenResult::kOpened:
      break;
    case QueryOpenResult::kDeferred:
      return;
    case QueryOpenResult::kPoolExhausted: {
      if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
        // Fast mode favors forward progress over accuracy. Keep a minimal
        // accumulated value instead of waiting for a slot to become available.
        auto it = logical_zpd_reports_.find(zpd_active_segment_.report_handle);
        if (it != logical_zpd_reports_.end()) {
          it->second.accumulated_samples =
              std::max<uint64_t>(it->second.accumulated_samples, uint64_t{1});
        }
        zpd_active_segment_.segment_pending_begin = false;
        return;
      }
      return;
    }
    case QueryOpenResult::kFailed:
    default:
      return;
  }

  zpd_active_segment_.segment_active = true;
  zpd_active_segment_.segment_pending_begin = false;
}

// Closes the active host segment without ending the logical report.
// BeginQuery/EndQuery can't cross D3D12 command list or Vulkan render pass
// boundaries. The result accumulates across all pieces.
void CommandProcessor::CloseQuerySegment() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_active_segment_.segment_active) {
    return;
  }

  uint64_t submission = 0;
  if (!CloseZPDQuery(zpd_active_segment_.report_handle, submission)) {
    zpd_active_segment_.segment_active = false;
    zpd_active_segment_.segment_pending_begin =
        zpd_active_segment_.logical_active;
    return;
  }

  auto it = logical_zpd_reports_.find(zpd_active_segment_.report_handle);
  if (it != logical_zpd_reports_.end()) {
    // Lets PumpPendingRetire drain early segments without blocking on the
    // final segment's submission.
    if (it->second.pending_segments == 0) {
      it->second.first_segment_end_submission = submission;
    }
    it->second.pending_segments++;
    it->second.last_segment_end_submission = submission;
  }

  zpd_active_segment_.segment_active = false;

  zpd_active_segment_.segment_pending_begin =
      zpd_active_segment_.logical_active;
}

void CommandProcessor::OnZPDQueryResolved(ReportHandle report_handle,
                                          uint64_t raw_samples) {
  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    return;
  }

  ZPDReport& logical = it->second;

  if (logical.pending_segments) {
    logical.pending_segments--;
  }

  logical.accumulated_samples += raw_samples;

  if (logical.ended && logical.pending_segments == 0) {
    uint32_t final_value = NormalizeSampleCount(logical.accumulated_samples);

    logical.cached_delta = final_value;
    logical.has_cached_delta = true;
    if (logical.end_record) {
      if (fast_zpd_report_cached_values_.size() >= kFastZPDCacheMaxEntries &&
          !fast_zpd_report_cached_values_.count(logical.end_record)) {
        fast_zpd_report_cached_values_.clear();
      }
      fast_zpd_report_cached_values_[logical.end_record] = final_value;
    }
    if (IsZPDReportCurrent(logical)) {
      CommitZPDReport(logical, final_value);
    }
    logical_zpd_reports_.erase(it);
  }
}

void CommandProcessor::PumpPendingRetire() {
  ReportHandle handle_to_await = zpd_pending_retire_handle_;
  if (handle_to_await == kInvalidReportHandle) {
    return;
  }

  auto logical_report = logical_zpd_reports_.find(handle_to_await);
  if (logical_report == logical_zpd_reports_.end()) {
    // If the report is already gone it retired through another path.
    // Clear so we don't spin on a handle that no longer exists.
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    return;
  }

  uint64_t wait_for_submission =
      logical_report->second.last_segment_end_submission;
  uint64_t first_submission =
      logical_report->second.first_segment_end_submission;

  // Early segments can be retired here and, in the best case, the report
  // fully resolves without any wait.
  if (first_submission != 0 && first_submission < wait_for_submission &&
      first_submission <= GetCompletedSubmission()) {
    PumpQueryResolves();
    logical_report = logical_zpd_reports_.find(handle_to_await);
    if (logical_report == logical_zpd_reports_.end()) {
      zpd_pending_retire_handle_ = kInvalidReportHandle;
      zpd_pending_retire_stalls_ = 0;
      return;
    }
    wait_for_submission = logical_report->second.last_segment_end_submission;
  }

  if (AwaitQueryResolve(handle_to_await, wait_for_submission)) {
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    return;
  }

  if (wait_for_submission == 0 ||
      GetCompletedSubmission() >= wait_for_submission) {
    ++zpd_pending_retire_stalls_;
  }

  // Abandon if the deadline has elapsed or the stall limit has been reached.
  // Both are checked to account for varied guest polling behavior.
  bool deadline_exceeded =
      (Clock::QueryHostUptimeMillis() - zpd_pending_retire_start_ms_ >=
       kStrictZPDRetireDeadlineMs);
  if (deadline_exceeded ||
      zpd_pending_retire_stalls_ >= kStrictZPDRetireMaxStalls) {
    // Write the cached delta to guest memory to avoid a sudden occlusion flash.
    if (IsZPDReportCurrent(logical_report->second)) {
      uint32_t fallback_delta = logical_report->second.cached_delta
                                    ? logical_report->second.cached_delta
                                    : 1;
      CommitZPDReport(logical_report->second, fallback_delta);
    }
    logical_zpd_reports_.erase(logical_report);
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
  }
}

void CommandProcessor::WriteZPDReport(uint32_t begin_record,
                                      uint32_t end_record, uint32_t begin_value,
                                      uint32_t delta_value,
                                      bool write_begin_record) {
  xenos::xe_gpu_depth_sample_counts* begin =
      begin_record
          ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
                begin_record)
          : nullptr;
  if (!end_record) {
    return;
  }
  xenos::xe_gpu_depth_sample_counts* end =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
          end_record);

  XenosZPDReport::WriteReportDelta(begin, end, begin_value, delta_value,
                                   write_begin_record);
}

void CommandProcessor::CommitZPDReport(ZPDReport& report,
                                       uint32_t delta_value) {
  uint32_t end_record =
      report.end_record ? report.end_record
                        : XenosZPDReport::GetEndRecordBase(report.slot_base);
  WriteZPDReport(report.begin_record, end_record, report.begin_value,
                 delta_value, report.begin_record != 0);

  // Advance running total so the next BeginReport on this slot picks up
  // the correct begin_value.
  uint32_t saturated_delta = XenosZPDReport::SaturateSampleCount(delta_value);
  uint32_t end_value = report.begin_value + saturated_delta;
  zpd_slot_values_[report.slot_base] = end_value;
}

bool CommandProcessor::IsZPDReportCurrent(const ZPDReport& report) const {
  auto it = zpd_slot_sequences_.find(report.slot_base);
  uint64_t current_sequence = it != zpd_slot_sequences_.end() ? it->second : 0;
  return current_sequence == report.slot_sequence_id;
}

uint32_t CommandProcessor::NormalizeSampleCount(uint64_t samples) const {
  if (samples == 0) {
    return 0;
  }

  uint64_t scale = zpd_draw_resolution_scale_x_ * zpd_draw_resolution_scale_y_;
  // Round, don't truncate. 1 guest sample at 2x = 4 host samples, need >= 1.
  uint64_t normalized = scale <= 1 ? samples : (samples + (scale >> 1)) / scale;

  return static_cast<uint32_t>(std::min<uint64_t>(normalized, UINT32_MAX));
}
#define COMMAND_PROCESSOR CommandProcessor
#include "pm4_command_processor_implement.h"
}  // namespace gpu
}  // namespace xe
