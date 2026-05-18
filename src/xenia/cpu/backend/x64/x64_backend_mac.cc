/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/x64/x64_backend_mac.h"

#include <cstddef>
#include "third_party/capstone/include/capstone/capstone.h"
#include "third_party/capstone/include/capstone/x86.h"

// Include new optimization systems
#include "xenia/cpu/backend/x64/cpu_features.h"
#include "xenia/cpu/backend/x64/thunk_optimizer.h"

#include "xenia/base/exception_handler.h"
#include "xenia/base/logging.h"
#include "xenia/base/xsimd_avx_support_mac.h"

DECLARE_bool(rosetta_force_avx);
#include "xenia/cpu/backend/x64/x64_assembler.h"
#include "xenia/cpu/backend/x64/x64_code_cache_mac.h"
#include "xenia/cpu/backend/x64/x64_emitter_mac.h"
#include "xenia/cpu/backend/x64/x64_function.h"
#include "xenia/cpu/backend/x64/performance_manager.h"
#include "xenia/cpu/backend/x64/x64_sequences_mac.h"
#include "xenia/cpu/backend/x64/x64_stack_layout.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/xex_module_mac.h"

DEFINE_bool(record_mmio_access_exceptions, true,
            "For guest addresses records whether we caught any mmio accesses "
            "for them. This info can then be used on a subsequent run to "
            "instruct the recompiler to emit checks",
            "x64");

DEFINE_int64(max_stackpoints, 65536,
             "Max number of host->guest stack mappings we can record.", "x64");

DEFINE_bool(enable_host_guest_stack_synchronization, true,
            "Records entries for guest/host stack mappings at function starts "
            "and checks for reentry at return sites. Has slight performance "
            "impact, but fixes crashes in games that use setjmp/longjmp.",
            "x64");

DEFINE_bool(enable_performance_monitoring, true,
            "Enable comprehensive JIT performance monitoring and profiling.", 
            "x64");

DEFINE_int32(perf_monitor_hot_threshold, 1000,
            "Execution count threshold for hot function detection.", "x64");

DEFINE_bool(perf_monitor_enable_vtune, false,
            "Enable VTune integration for JIT profiling.", "x64");

DEFINE_bool(perf_monitor_detailed_metrics, false,
            "Enable detailed performance metrics collection.", "x64");
#if XE_X64_PROFILER_AVAILABLE == 1
DECLARE_bool(instrument_call_times);
#endif

namespace xe {
namespace cpu {
namespace backend {
namespace x64 {

class X64HelperEmitter : public X64Emitter {
 public:
  struct _code_offsets {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  };
  X64HelperEmitter(X64Backend* backend, XbyakAllocator* allocator);
  ~X64HelperEmitter() override;
  HostToGuestThunk EmitHostToGuestThunk();
  GuestToHostThunk EmitGuestToHostThunk();
  ResolveFunctionThunk EmitResolveFunctionThunk();
  void* EmitGuestAndHostSynchronizeStackHelper();
  // 1 for loading byte, 2 for halfword and 4 for word.
  // these specialized versions save space in the caller
  void* EmitGuestAndHostSynchronizeStackSizeLoadThunk(
      void* sync_func, unsigned stack_element_size);

  void* EmitTryAcquireReservationHelper();
  void* EmitReservedStoreHelper(bool bit64 = false);

  void* EmitScalarVRsqrteHelper();
  void* EmitVectorVRsqrteHelper(void* scalar_helper);

  void* EmitFrsqrteHelper();

 private:
  void* EmitCurrentForOffsets(const _code_offsets& offsets,
                              size_t stack_size = 0);
  // The following four functions provide save/load functionality for registers.
  // They assume at least StackLayout::THUNK_STACK_SIZE bytes have been
  // allocated on the stack.
  void EmitSaveVolatileRegs();
  void EmitLoadVolatileRegs();
  void EmitSaveNonvolatileRegs();
  void EmitLoadNonvolatileRegs();
};

#if XE_PLATFORM_WIN32
static constexpr unsigned char guest_trampoline_template[] = {
    0x48, 0xBA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0x49,
    0xB8, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0x48, 0xB9,
    0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0x48, 0xB8, 0x99,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0xFF, 0xE0};

#else
// sysv x64 abi, exact same offsets for args
static constexpr unsigned char guest_trampoline_template[] = {
    0x48, 0xBF, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0x48,
    0xBE, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0x48, 0xB9,
    0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0x48, 0xB8, 0x99,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x00, 0xFF, 0xE0};
#endif
static constexpr uint32_t guest_trampoline_template_offset_arg1 = 2,
                          guest_trampoline_template_offset_arg2 = 0xC,
                          guest_trampoline_template_offset_rcx = 0x16,
                          guest_trampoline_template_offset_rax = 0x20;
X64Backend::X64Backend() : Backend(), code_cache_(nullptr), thunk_optimizer_(nullptr) {
  // Initialize performance monitor if enabled
  if (::cvars::enable_performance_monitoring) {
    performance_monitor_ = std::make_unique<X64PerformanceMonitor>();
  }
  if (cs_open(CS_ARCH_X86, CS_MODE_64, &capstone_handle_) != CS_ERR_OK) {
    assert_always("Failed to initialize capstone");
  }
  cs_option(capstone_handle_, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);
  cs_option(capstone_handle_, CS_OPT_DETAIL, CS_OPT_ON);
  cs_option(capstone_handle_, CS_OPT_SKIPDATA, CS_OPT_OFF);
  uint32_t base_address = 0x10000;
#ifdef __APPLE__
  // Start higher on macOS to avoid PAGEZERO and executable segments
  base_address = 0x40000000;
#endif

  void* buf_trampoline_code = nullptr;
  size_t allocation_size = sizeof(guest_trampoline_template) * MAX_GUEST_TRAMPOLINES;
  fprintf(stderr, "[X64Backend] Allocating trampoline memory: search_start=0x%x, len=0x%zx (%d trampolines)\n", 
          base_address, allocation_size, MAX_GUEST_TRAMPOLINES);
  
  int allocation_attempts = 0;
#ifdef __APPLE__
  // On macOS with MAP_JIT, we shouldn't force a specific address as the OS
  // manages the JIT region placement.
  fprintf(stderr, "[X64Backend] macOS detected: Letting OS select trampoline address (MAP_JIT safe)...\n");
  buf_trampoline_code = memory::AllocFixed(
      nullptr,
      allocation_size,
      xe::memory::AllocationType::kReserveCommit,
      xe::memory::PageAccess::kExecuteReadWrite);
  
  if (buf_trampoline_code) {
      allocation_attempts = 1;
      fprintf(stderr, "[X64Backend] Successfully allocated trampolines at %p\n", buf_trampoline_code);
  }
#else
  while (base_address < 0x80000000) {
    allocation_attempts++;
    fprintf(stderr, "[X64Backend] Attempt %d: Allocating at 0x%x...\n", allocation_attempts, base_address);
    
    buf_trampoline_code = memory::AllocFixed(
        reinterpret_cast<void*>(static_cast<uintptr_t>(base_address)),
        allocation_size,
        xe::memory::AllocationType::kReserveCommit,
        xe::memory::PageAccess::kExecuteReadWrite);
        
    if (!buf_trampoline_code) {
      fprintf(stderr, "[X64Backend] Allocation failed at 0x%x", base_address);
      base_address += 65536;
      fprintf(stderr, ", next attempt at 0x%x (64KB step)\n", base_address);
    } else {
      fprintf(stderr, "[X64Backend] Successfully allocated trampolines at %p after %d attempts\n", 
              buf_trampoline_code, allocation_attempts);
      fprintf(stderr, "[X64Backend] JIT Memory range: %p - %p (size: 0x%zx)\n",
              buf_trampoline_code,
              reinterpret_cast<uint8_t*>(buf_trampoline_code) + allocation_size,
              allocation_size);
      break;
    }
  }
#endif
  
  if (!buf_trampoline_code) {
    fprintf(stderr, "[X64Backend] CRITICAL ERROR: Failed to allocate trampoline memory after %d attempts in range 0x10000 - 0x80000000\n", allocation_attempts);
    fprintf(stderr, "[X64Backend] This indicates a failure in MAP_JIT entitlements or memory allocation\n");
  }
  xenia_assert(buf_trampoline_code);
  guest_trampoline_memory_ = reinterpret_cast<uint8_t*>(buf_trampoline_code);
  guest_trampoline_address_bitmap_.Resize(MAX_GUEST_TRAMPOLINES);
  
  fprintf(stderr, "[X64Backend] JIT Infrastructure initialized successfully:\n");
  fprintf(stderr, "  - Trampoline memory: %p (size: 0x%zx)\n", guest_trampoline_memory_, allocation_size);
  fprintf(stderr, "  - Max trampolines: %d\n", MAX_GUEST_TRAMPOLINES);
  fprintf(stderr, "  - Trampoline template size: 0x%zx\n", sizeof(guest_trampoline_template));
  fprintf(stderr, "  - Bitmap entries: %zu\n", guest_trampoline_address_bitmap_.data().size() * 64);
}

X64Backend::~X64Backend() {
  // Shutdown performance monitor
  if (performance_monitor_) {
    performance_monitor_->Shutdown();
    performance_monitor_.reset();
  }
  
  // Clean up thunk optimizer
  delete thunk_optimizer_;
  thunk_optimizer_ = nullptr;

  if (capstone_handle_) {
    cs_close(&capstone_handle_);
  }

  X64Emitter::FreeConstData(emitter_data_);
  ExceptionHandler::Uninstall(&ExceptionCallbackThunk, this);
  if (guest_trampoline_memory_) {
    memory::DeallocFixed(
        guest_trampoline_memory_,
        sizeof(guest_trampoline_template) * MAX_GUEST_TRAMPOLINES,
        memory::DeallocationType::kRelease);
    guest_trampoline_memory_ = nullptr;
  }
}

static void ForwardMMIOAccessForRecording(void* context, void* hostaddr) {
  reinterpret_cast<X64Backend*>(context)
      ->RecordMMIOExceptionForGuestInstruction(hostaddr);
}
#if XE_X64_PROFILER_AVAILABLE == 1
// todo: better way of passing to atexit. maybe do in destructor instead?
// nope, destructor is never called
static GuestProfilerData* backend_profiler_data = nullptr;

static uint64_t nanosecond_lifetime_start = 0;
static void WriteGuestProfilerData() {
  if (cvars::instrument_call_times) {
    uint64_t end = Clock::QueryHostSystemTime();

    uint64_t total = end - nanosecond_lifetime_start;

    double totaltime_divisor = static_cast<double>(total);

    FILE* output_file = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> unsorted_profile{};
    for (auto&& entry : *backend_profiler_data) {
      if (entry.second) {  // skip times of 0
        unsorted_profile.emplace_back(entry.first, entry.second);
      }
    }

    std::sort(unsorted_profile.begin(), unsorted_profile.end(),
              [](auto& x, auto& y) { return x.second < y.second; });

    fopen_s(&output_file, "profile_times.txt", "w");
    FILE* idapy_file = nullptr;
    fopen_s(&idapy_file, "profile_print_times.py", "w");

    for (auto&& sorted_entry : unsorted_profile) {
      // double time_in_seconds =
      //    static_cast<double>(sorted_entry.second) / 10000000.0;
      double time_in_milliseconds =
          static_cast<double>(sorted_entry.second) / (10000000.0 / 1000.0);

      double slice = static_cast<double>(sorted_entry.second) /
                     static_cast<double>(totaltime_divisor);

      fprintf(output_file,
              "%X took %.20f milliseconds, totaltime slice percentage %.20f \n",
              sorted_entry.first, time_in_milliseconds, slice);

      fprintf(idapy_file,
              "print(get_name(0x%X) + ' took %.20f ms, %.20f percent')\n",
              sorted_entry.first, time_in_milliseconds, slice);
    }

    fclose(output_file);
    fclose(idapy_file);
  }
}

static void GuestProfilerUpdateThreadProc() {
  nanosecond_lifetime_start = Clock::QueryHostSystemTime();

  do {
    xe::threading::Sleep(std::chrono::seconds(30));
    WriteGuestProfilerData();
  } while (true);
}
static std::unique_ptr<xe::threading::Thread> g_profiler_update_thread{};
#endif

bool X64Backend::Initialize(Processor* processor) {
  XELOGI("X64Backend::Initialize() starting...");
  
  XELOGI("X64Backend::Initialize() calling Backend::Initialize()...");
  if (!Backend::Initialize(processor)) {
    XELOGE("X64Backend::Initialize() failed - Backend::Initialize() returned false");
    return false;
  }
  XELOGI("X64Backend::Initialize() Backend::Initialize() completed");

  XELOGI("X64Backend::Initialize() checking AVX support...");
  xe::xsimd_support::InitializeSIMDSupport();
  bool is_rosetta = xe::xsimd_support::IsRunningUnderRosetta2();
  bool avx_ok = xe::xsimd_support::IsAVXSupported();
  if (!avx_ok) {
    if (is_rosetta && cvars::rosetta_force_avx) {
      XELOGW("Rosetta 2: AVX probe failed but rosetta_force_avx=TRUE - continuing (may SIGILL)");
    } else if (is_rosetta) {
      // SIGILL autodetect: AVX/AVX2 probe failed → use SSE-only codegen
      XELOGI(
          "SIGILL autodetect: AVX probe failed under Rosetta 2 → x64 backend "
          "using SSE-only codegen (no VEX/AVX).");
      XELOGI(
          "All VEX/AVX/AVX2 instructions routed to SSE4.2 (verified).");
    } else {
      XELOGE("AVX instructions are not executable on this host (probe failed).");
      XELOGE("Xenia's x64 backend emits VEX instructions and requires AVX.");
      return false;
    }
  } else if (is_rosetta) {
    XELOGI("Rosetta 2: AVX probe passed - proceeding with AVX-capable codegen");
  } else {
    XELOGI("X64Backend::Initialize() AVX support confirmed");
  }

  XELOGI("X64Backend::Initialize() performing CPU feature detection...");
  // Still use Xbyak for specific feature detection
  Xbyak::util::Cpu cpu;

  // Need movbe to do advanced LOAD/STORE tricks.
  if (::cvars::x64_extension_mask & kX64EmitMovbe) {
    machine_info_.supports_extended_load_store =
        cpu.has(Xbyak::util::Cpu::tMOVBE);
  } else {
    machine_info_.supports_extended_load_store = false;
  }

  XELOGI("X64Backend::Initialize() setting up register sets...");
  auto& gprs = machine_info_.register_sets[0];
  gprs.id = 0;
  std::strcpy(gprs.name, "gpr");
  gprs.types = MachineInfo::RegisterSet::INT_TYPES;
  gprs.count = X64Emitter::GPR_COUNT;

  auto& xmms = machine_info_.register_sets[1];
  xmms.id = 1;
  std::strcpy(xmms.name, "xmm");
  xmms.types = MachineInfo::RegisterSet::FLOAT_TYPES |
               MachineInfo::RegisterSet::VEC_TYPES;
  xmms.count = X64Emitter::XMM_COUNT;

  XELOGI("X64Backend::Initialize() creating code cache...");
  code_cache_ = X64CodeCache::Create();
  Backend::code_cache_ = code_cache_.get();
  code_cache_->SetBackend(this);
  XELOGI("X64Backend::Initialize() initializing code cache...");
  if (!code_cache_->Initialize()) {
    XELOGE("X64Backend::Initialize() failed - code_cache_->Initialize() returned false");
    return false;
  }
  XELOGI("X64Backend::Initialize() code cache initialized successfully");
  XELOGI("X64Backend::Initialize() committing executable range...");
  // HV range
  code_cache()->CommitExecutableRange(GUEST_TRAMPOLINE_BASE,
                                      GUEST_TRAMPOLINE_END);
  XELOGI("X64Backend::Initialize() allocating emitter constant data...");
  // Allocate emitter constant data.
  emitter_data_ = X64Emitter::PlaceConstData();

  XELOGI("X64Backend::Initialize() generating transition thunks...");
  // Generate thunks used to transition between jitted code and host code.
  XbyakAllocator allocator;
  X64HelperEmitter thunk_emitter(this, &allocator);
  host_to_guest_thunk_ = thunk_emitter.EmitHostToGuestThunk();
  guest_to_host_thunk_ = thunk_emitter.EmitGuestToHostThunk();
  resolve_function_thunk_ = thunk_emitter.EmitResolveFunctionThunk();
  XELOGI("X64Backend::Initialize() transition thunks generated successfully");

  if (::cvars::enable_host_guest_stack_synchronization) {
    synchronize_guest_and_host_stack_helper_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackHelper();

    synchronize_guest_and_host_stack_helper_size8_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackSizeLoadThunk(
            synchronize_guest_and_host_stack_helper_, 1);
    synchronize_guest_and_host_stack_helper_size16_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackSizeLoadThunk(
            synchronize_guest_and_host_stack_helper_, 2);
    synchronize_guest_and_host_stack_helper_size32_ =
        thunk_emitter.EmitGuestAndHostSynchronizeStackSizeLoadThunk(
            synchronize_guest_and_host_stack_helper_, 4);
  }
  try_acquire_reservation_helper_ =
      thunk_emitter.EmitTryAcquireReservationHelper();
  reserved_store_32_helper = thunk_emitter.EmitReservedStoreHelper(false);
  reserved_store_64_helper = thunk_emitter.EmitReservedStoreHelper(true);
  vrsqrtefp_scalar_helper = thunk_emitter.EmitScalarVRsqrteHelper();
  vrsqrtefp_vector_helper =
      thunk_emitter.EmitVectorVRsqrteHelper(vrsqrtefp_scalar_helper);
  frsqrtefp_helper = thunk_emitter.EmitFrsqrteHelper();
  // Set the code cache to use the ResolveFunction thunk for default
  // indirections.
  assert_zero(uint64_t(resolve_function_thunk_) & 0xFFFFFFFF00000000ull);
  code_cache_->set_indirection_default(
      uint32_t(uint64_t(resolve_function_thunk_)));

  // Allocate some special indirections.
  code_cache_->CommitExecutableRange(0x9FFF0000, 0x9FFFFFFF);

  // Setup exception callback
  ExceptionHandler::Install(&ExceptionCallbackThunk, this);
  if (::cvars::record_mmio_access_exceptions) {
    processor->memory()->SetMMIOExceptionRecordingCallback(
        ForwardMMIOAccessForRecording, reinterpret_cast<void*>(this));
  }

  // Initialize performance monitor
  if (performance_monitor_) {
    if (!performance_monitor_->Initialize()) {
      XELOGW("Failed to initialize performance monitor");
      performance_monitor_.reset();
    } else {
      XELOGI("Performance monitor initialized successfully");
      
      // Initialize performance manager
      fprintf(stderr, "[x64_backend] About to initialize Performance Manager...\n");
      fflush(stderr);
      auto& perf_manager = PerformanceManager::Instance();
      perf_manager.Initialize(this);
      fprintf(stderr, "[x64_backend] Performance Manager initialized successfully\n");
      fflush(stderr);
      
      // Initialize new optimization systems
      fprintf(stderr, "[x64_backend] About to initialize CPU feature detection...\n");
      fflush(stderr);
      CPUFeatureDetector::InitializeCPUInfo();
      fprintf(stderr, "[x64_backend] CPU feature detection initialized\n");
      fflush(stderr);
      const auto& cpu_info = CPUFeatureDetector::GetCPUInfo();
      fprintf(stderr, "[x64_backend] Got CPU info, platform.is_apple_silicon=%d, platform.is_rosetta2=%d\n",
              cpu_info.platform.is_apple_silicon, cpu_info.platform.is_rosetta2);
      fflush(stderr);
      
      // Configure optimization systems based on CPU features
      auto thunk_optimizer = std::make_unique<ThunkOptimizer>(this, performance_monitor_.get());
      if (thunk_optimizer->Initialize()) {
        XELOGI("Thunk optimizer initialized successfully");
        
        // Set optimization level based on CPU capabilities
        if (cpu_info.supported_features & static_cast<uint64_t>(CPUFeature::kAVX512F)) {
          thunk_optimizer->SetOptimizationLevel(ThunkOptimizationLevel::kAggressive);
          XELOGI("Using aggressive optimization (AVX-512 detected)");
        } else if (cpu_info.supported_features & static_cast<uint64_t>(CPUFeature::kAVX2)) {
          thunk_optimizer->SetOptimizationLevel(ThunkOptimizationLevel::kBalanced);
          XELOGI("Using balanced optimization (AVX2 detected)");
        } else {
          thunk_optimizer->SetOptimizationLevel(ThunkOptimizationLevel::kMinimal);
          XELOGI("Using minimal optimization (baseline CPU)");
        }
        
        // Configure dispatch strategy based on platform
        if (cpu_info.platform.is_apple_silicon) {
          thunk_optimizer->SetDispatchStrategy(DispatchStrategy::kHybrid);
          thunk_optimizer->SetHotThreshold(500); // Lower threshold for Apple Silicon
          XELOGI("Using hybrid dispatch strategy for Apple Silicon");
        } else {
          thunk_optimizer->SetDispatchStrategy(DispatchStrategy::kAdaptive);
          thunk_optimizer->SetHotThreshold(1000); // Standard threshold
          XELOGI("Using adaptive dispatch strategy");
        }
        
        // Store thunk optimizer instance
        thunk_optimizer_ = thunk_optimizer.get();
      } else {
        XELOGW("Failed to initialize thunk optimizer");
      }
      
      // Initialize CPU feature detection
      CPUFeatureDetector::InitializeCPUInfo();
      
      // Print optimization recommendations
      auto recommendations = CPUFeatureDetector::GetOptimizationRecommendations();
      if (!recommendations.empty()) {
        XELOGI("CPU Optimization Recommendations:");
        for (const auto& rec : recommendations) {
          XELOGI("  - {}", rec);
        }
      }
    }
  }

#if XE_X64_PROFILER_AVAILABLE == 1
  if (cvars::instrument_call_times) {
    backend_profiler_data = &profiler_data_;
    xe::threading::Thread::CreationParameters slimparams;

    slimparams.create_suspended = false;
    slimparams.initial_priority = xe::threading::ThreadPriority::kLowest;
    slimparams.stack_size = 65536 * 4;

    g_profiler_update_thread = std::move(xe::threading::Thread::Create(
        slimparams, GuestProfilerUpdateThreadProc));
  }
#endif

  XELOGI("X64Backend::Initialize() completed successfully!");
  return true;
}

void x64::X64Backend::CommitExecutableRange(uint32_t guest_low,
                                       uint32_t guest_high) {
  code_cache_->CommitExecutableRange(guest_low, guest_high);
}

std::unique_ptr<Assembler> x64::X64Backend::CreateAssembler() {
  return std::make_unique<X64Assembler>(this);
}

std::unique_ptr<GuestFunction> x64::X64Backend::CreateGuestFunction(
    Module* module, uint32_t address) {
  return std::make_unique<X64Function>(module, address);
}

const CPUInfo& x64::X64Backend::GetCPUInfo() const {
  return CPUFeatureDetector::GetCPUInfo();
}

uint64_t ReadCapstoneReg(HostThreadContext* context, x86_reg reg) {
  switch (reg) {
    case X86_REG_RAX:
      return context->rax;
    case X86_REG_RCX:
      return context->rcx;
    case X86_REG_RDX:
      return context->rdx;
    case X86_REG_RBX:
      return context->rbx;
    case X86_REG_RSP:
      return context->rsp;
    case X86_REG_RBP:
      return context->rbp;
    case X86_REG_RSI:
      return context->rsi;
    case X86_REG_RDI:
      return context->rdi;
    case X86_REG_R8:
      return context->r8;
    case X86_REG_R9:
      return context->r9;
    case X86_REG_R10:
      return context->r10;
    case X86_REG_R11:
      return context->r11;
    case X86_REG_R12:
      return context->r12;
    case X86_REG_R13:
      return context->r13;
    case X86_REG_R14:
      return context->r14;
    case X86_REG_R15:
      return context->r15;
    default:
      assert_unhandled_case(reg);
      return 0;
  }
}

#define X86_EFLAGS_CF 0x00000001  // Carry Flag
#define X86_EFLAGS_PF 0x00000004  // Parity Flag
#define X86_EFLAGS_ZF 0x00000040  // Zero Flag
#define X86_EFLAGS_SF 0x00000080  // Sign Flag
#define X86_EFLAGS_OF 0x00000800  // Overflow Flag
bool TestCapstoneEflags(uint32_t eflags, uint32_t insn) {
  // https://www.felixcloutier.com/x86/Jcc.html
  switch (insn) {
    case X86_INS_JAE:
      // CF=0 && ZF=0
      return ((eflags & X86_EFLAGS_CF) == 0) && ((eflags & X86_EFLAGS_ZF) == 0);
    case X86_INS_JA:
      // CF=0
      return (eflags & X86_EFLAGS_CF) == 0;
    case X86_INS_JBE:
      // CF=1 || ZF=1
      return ((eflags & X86_EFLAGS_CF) == X86_EFLAGS_CF) ||
             ((eflags & X86_EFLAGS_ZF) == X86_EFLAGS_ZF);
    case X86_INS_JB:
      // CF=1
      return (eflags & X86_EFLAGS_CF) == X86_EFLAGS_CF;
    case X86_INS_JE:
      // ZF=1
      return (eflags & X86_EFLAGS_ZF) == X86_EFLAGS_ZF;
    case X86_INS_JGE:
      // SF=OF
      return (eflags & X86_EFLAGS_SF) == (eflags & X86_EFLAGS_OF);
    case X86_INS_JG:
      // ZF=0 && SF=OF
      return ((eflags & X86_EFLAGS_ZF) == 0) &&
             ((eflags & X86_EFLAGS_SF) == (eflags & X86_EFLAGS_OF));
    case X86_INS_JLE:
      // ZF=1 || SF!=OF
      return ((eflags & X86_EFLAGS_ZF) == X86_EFLAGS_ZF) ||
             ((eflags & X86_EFLAGS_SF) != X86_EFLAGS_OF);
    case X86_INS_JL:
      // SF!=OF
      return (eflags & X86_EFLAGS_SF) != (eflags & X86_EFLAGS_OF);
    case X86_INS_JNE:
      // ZF=0
      return (eflags & X86_EFLAGS_ZF) == 0;
    case X86_INS_JNO:
      // OF=0
      return (eflags & X86_EFLAGS_OF) == 0;
    case X86_INS_JNP:
      // PF=0
      return (eflags & X86_EFLAGS_PF) == 0;
    case X86_INS_JNS:
      // SF=0
      return (eflags & X86_EFLAGS_SF) == 0;
    case X86_INS_JO:
      // OF=1
      return (eflags & X86_EFLAGS_OF) == X86_EFLAGS_OF;
    case X86_INS_JP:
      // PF=1
      return (eflags & X86_EFLAGS_PF) == X86_EFLAGS_PF;
    case X86_INS_JS:
      // SF=1
      return (eflags & X86_EFLAGS_SF) == X86_EFLAGS_SF;
    default:
      assert_unhandled_case(insn);
      return false;
  }
}

uint64_t x64::X64Backend::CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                                  uint64_t current_pc) {
  auto machine_code_ptr = reinterpret_cast<const uint8_t*>(current_pc);
  size_t remaining_machine_code_size = 64;
  uint64_t host_address = current_pc;
  cs_insn insn = {0};
  cs_detail all_detail = {0};
  insn.detail = &all_detail;
  cs_disasm_iter(capstone_handle_, &machine_code_ptr,
                 &remaining_machine_code_size, &host_address, &insn);
  auto& detail = all_detail.x86;
  switch (insn.id) {
    default:
      // Not a branching instruction - just move over it.
      return current_pc + insn.size;
    case X86_INS_CALL: {
      assert_true(detail.op_count == 1);
      assert_true(detail.operands[0].type == X86_OP_REG);
      uint64_t target_pc =
          ReadCapstoneReg(&thread_info->host_context, detail.operands[0].reg);
      return target_pc;
    }
    case X86_INS_RET: {
      assert_zero(detail.op_count);
      auto stack_ptr =
          reinterpret_cast<uint64_t*>(thread_info->host_context.rsp);
      uint64_t target_pc = stack_ptr[0];
      return target_pc;
    }
    case X86_INS_JMP: {
      assert_true(detail.op_count == 1);
      if (detail.operands[0].type == X86_OP_IMM) {
        uint64_t target_pc = static_cast<uint64_t>(detail.operands[0].imm);
        return target_pc;
      } else if (detail.operands[0].type == X86_OP_REG) {
        uint64_t target_pc =
            ReadCapstoneReg(&thread_info->host_context, detail.operands[0].reg);
        return target_pc;
      } else {
        // TODO(benvanik): find some more uses of this.
        assert_always("jmp branch emulation not yet implemented");
        return current_pc + insn.size;
      }
    }
    case X86_INS_JCXZ:
    case X86_INS_JECXZ:
    case X86_INS_JRCXZ:
      assert_always("j*cxz branch emulation not yet implemented");
      return current_pc + insn.size;
    case X86_INS_JAE:
    case X86_INS_JA:
    case X86_INS_JBE:
    case X86_INS_JB:
    case X86_INS_JE:
    case X86_INS_JGE:
    case X86_INS_JG:
    case X86_INS_JLE:
    case X86_INS_JL:
    case X86_INS_JNE:
    case X86_INS_JNO:
    case X86_INS_JNP:
    case X86_INS_JNS:
    case X86_INS_JO:
    case X86_INS_JP:
    case X86_INS_JS: {
      assert_true(detail.op_count == 1);
      assert_true(detail.operands[0].type == X86_OP_IMM);
      uint64_t target_pc = static_cast<uint64_t>(detail.operands[0].imm);
      bool test_passed =
          TestCapstoneEflags(thread_info->host_context.eflags, insn.id);
      if (test_passed) {
        return target_pc;
      } else {
        return current_pc + insn.size;
      }
    }
  }
}

void x64::X64Backend::InstallBreakpoint(Breakpoint* breakpoint) {
  breakpoint->ForEachHostAddress([breakpoint](uint64_t host_address) {
    auto ptr = reinterpret_cast<void*>(host_address);
    auto original_bytes = xe::load_and_swap<uint16_t>(ptr);
    assert_true(original_bytes != 0x0F0B);
    xe::store_and_swap<uint16_t>(ptr, 0x0F0B);
    breakpoint->backend_data().emplace_back(host_address, original_bytes);
  });
}

void x64::X64Backend::InstallBreakpoint(Breakpoint* breakpoint, Function* fn) {
  assert_true(breakpoint->address_type() == Breakpoint::AddressType::kGuest);
  assert_true(fn->is_guest());
  auto guest_function = reinterpret_cast<cpu::GuestFunction*>(fn);
  auto host_address =
      guest_function->MapGuestAddressToMachineCode(breakpoint->guest_address());
  if (!host_address) {
    assert_always();
    return;
  }

  // Assume we haven't already installed a breakpoint in this spot.
  auto ptr = reinterpret_cast<void*>(host_address);
  auto original_bytes = xe::load_and_swap<uint16_t>(ptr);
  assert_true(original_bytes != 0x0F0B);
  xe::store_and_swap<uint16_t>(ptr, 0x0F0B);
  breakpoint->backend_data().emplace_back(host_address, original_bytes);
}

void x64::X64Backend::UninstallBreakpoint(Breakpoint* breakpoint) {
  for (auto& pair : breakpoint->backend_data()) {
    auto ptr = reinterpret_cast<uint8_t*>(pair.first);
    auto instruction_bytes = xe::load_and_swap<uint16_t>(ptr);
    assert_true(instruction_bytes == 0x0F0B);
    xe::store_and_swap<uint16_t>(ptr, static_cast<uint16_t>(pair.second));
  }
  breakpoint->backend_data().clear();
}

bool x64::X64Backend::ExceptionCallbackThunk(Exception* ex, void* data) {
  auto backend = reinterpret_cast<X64Backend*>(data);
  return backend->ExceptionCallback(ex);
}
void x64::X64Backend::RecordMMIOExceptionForGuestInstruction(void* host_address) {
  uint64_t host_addr_u64 = (uint64_t)host_address;

  auto fnfor = code_cache()->LookupFunction(host_addr_u64);
  if (fnfor) {
    uint32_t guestaddr = fnfor->MapMachineCodeToGuestAddress(host_addr_u64);

    Module* guest_module = fnfor->module();
    if (guest_module) {
      XexModule* xex_guest_module = dynamic_cast<XexModule*>(guest_module);

      if (xex_guest_module) {
        cpu::InfoCacheFlags* icf =
            xex_guest_module->GetInstructionAddressFlags(guestaddr);

        if (icf) {
          icf->accessed_mmio = true;
        }
      }
    }
  }
}
bool x64::X64Backend::ExceptionCallback(Exception* ex) {
  if (ex->code() != Exception::Code::kIllegalInstruction) {
    // We only care about illegal instructions. Other things will be handled by
    // other handlers (probably). If nothing else picks it up we'll be called
    // with OnUnhandledException to do real crash handling.
    return false;
  }

  // processor_->memory()->LookupVirtualMappedRange()

  // Verify an expected illegal instruction.
  auto instruction_bytes =
      xe::load_and_swap<uint16_t>(reinterpret_cast<void*>(ex->pc()));
  if (instruction_bytes != 0x0F0B) {
    // Not our ud2 - not us.
    return false;
  }

  // Let the processor handle things.
  return processor()->OnThreadBreakpointHit(ex);
}

x64::X64HelperEmitter::X64HelperEmitter(x64::X64Backend* backend,
                                         XbyakAllocator* allocator)
    : X64Emitter(backend, allocator) {}

x64::X64HelperEmitter::~X64HelperEmitter() {}
void* x64::X64HelperEmitter::EmitCurrentForOffsets(const _code_offsets& code_offsets,
                                                 size_t stack_size) {
  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = stack_size;

  void* fn = Emplace(func_info);
  return fn;
}
x64::HostToGuestThunk x64::X64HelperEmitter::EmitHostToGuestThunk() {
#ifdef XE_PLATFORM_WIN32
  // rcx = target
  // rdx = arg0 (context)
  // r8 = arg1 (guest return address)

  _code_offsets code_offsets = {};

  constexpr size_t stack_size = StackLayout::THUNK_STACK_SIZE;

  code_offsets.prolog = getSize();

  // rsp + 0 = return address
  mov(qword[rsp + 8 * 3], r8);
  mov(qword[rsp + 8 * 2], rdx);
  mov(qword[rsp + 8 * 1], rcx);
  sub(rsp, stack_size);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // Save nonvolatile registers.
  EmitSaveNonvolatileRegs();

  mov(rax, rcx);
  mov(rsi, rdx);                                                    // context
  mov(rdi, ptr[rdx + offsetof(ppc::PPCContext, virtual_membase)]);  // membase
  mov(rcx, r8);  // return address
  call(rax);
  vzeroupper();
  EmitLoadNonvolatileRegs();

  code_offsets.epilog = getSize();

  add(rsp, stack_size);
  mov(rcx, qword[rsp + 8 * 1]);
  mov(rdx, qword[rsp + 8 * 2]);
  mov(r8, qword[rsp + 8 * 3]);
  ret();
#elif XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // System-V ABI args:
  // rdi = target
  // rsi = arg0 (context)
  // rdx = arg1 (guest return address)

  struct _code_offsets {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  constexpr size_t stack_size = StackLayout::THUNK_STACK_SIZE;

  code_offsets.prolog = getSize();
  // rsp + 0 = return address
  sub(rsp, stack_size);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // Save nonvolatile registers.
  EmitSaveNonvolatileRegs();

  mov(rax, rdi);
  // mov(rsi, rsi);   // context
  mov(rdi, ptr[rsi + offsetof(ppc::PPCContext, virtual_membase)]);  // membase
  mov(rcx, rdx);  // return address
  call(rax);

  EmitLoadNonvolatileRegs();

  code_offsets.epilog = getSize();

  add(rsp, stack_size);
  ret();
#else
  assert_always("Unknown platform ABI in host to guest thunk!");
#endif

  code_offsets.tail = getSize();

  assert_zero(code_offsets.prolog);
  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = stack_size;

  void* fn = Emplace(func_info);
  return (HostToGuestThunk)fn;
}

x64::GuestToHostThunk x64::X64HelperEmitter::EmitGuestToHostThunk() {
#if XE_PLATFORM_WIN32
  // rcx = target function
  // rdx = arg0
  // r8  = arg1
  // r9  = arg2

  _code_offsets code_offsets = {};

  constexpr size_t stack_size = StackLayout::THUNK_STACK_SIZE;

  code_offsets.prolog = getSize();

  // rsp + 0 = return address
  sub(rsp, stack_size);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  // chrispy: added this for proper vmsum impl, avx2 bitshifts
  vzeroupper();
  // Save off volatile registers.
  EmitSaveVolatileRegs();

  mov(rax, rcx);              // function
  mov(rcx, GetContextReg());  // context
  call(rax);

  EmitLoadVolatileRegs();

  code_offsets.epilog = getSize();

  add(rsp, stack_size);
  ret();
#elif XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // This function is being called using the Microsoft ABI from CallNative
  // rcx = target function
  // rdx = arg0
  // r8  = arg1
  // r9  = arg2

  // Must be translated to System-V ABI:
  // rdi = target function
  // rsi = arg0
  // rdx = arg1
  // rcx = arg2
  // r8, r9 - unused argument registers

  struct _code_offsets {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};

  constexpr size_t stack_size = StackLayout::THUNK_STACK_SIZE;

  code_offsets.prolog = getSize();

  // rsp + 0 = return address
  sub(rsp, stack_size);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // Save off volatile registers.
  EmitSaveVolatileRegs();

  mov(rax, rcx);              // function
  mov(rdi, GetContextReg());  // context
  mov(rsi, rdx);              // arg0
  mov(rdx, r8);               // arg1
  mov(rcx, r9);               // arg2
  call(rax);

  EmitLoadVolatileRegs();

  code_offsets.epilog = getSize();

  add(rsp, stack_size);
  ret();
#else
  assert_always("Unknown platform ABI in guest to host thunk!")
#endif

  code_offsets.tail = getSize();

  assert_zero(code_offsets.prolog);
  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = stack_size;

  void* fn = Emplace(func_info);
  return (GuestToHostThunk)fn;
}

// X64Emitter handles actually resolving functions.
uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

x64::ResolveFunctionThunk x64::X64HelperEmitter::EmitResolveFunctionThunk() {
#if XE_PLATFORM_WIN32
  // ebx = target PPC address
  // rcx = context

  _code_offsets code_offsets = {};

  constexpr size_t stack_size = StackLayout::THUNK_STACK_SIZE;

  code_offsets.prolog = getSize();

  // rsp + 0 = return address
  sub(rsp, stack_size);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // Save volatile registers
  EmitSaveVolatileRegs();

  mov(rcx, rsi);  // context
  mov(rdx, rbx);
  mov(rax, reinterpret_cast<uint64_t>(&ResolveFunction));
  call(rax);

  EmitLoadVolatileRegs();

  code_offsets.epilog = getSize();

  add(rsp, stack_size);
  jmp(rax);
#elif XE_PLATFORM_LINUX || XE_PLATFORM_MAC
  // Function is called with the following params:
  // ebx = target PPC address
  // rsi = context

  // System-V ABI args:
  // rdi = context
  // rsi = target PPC address

  struct _code_offsets {
    size_t prolog;
    size_t prolog_stack_alloc;
    size_t body;
    size_t epilog;
    size_t tail;
  } code_offsets = {};
  constexpr size_t stack_size = StackLayout::THUNK_STACK_SIZE;

  code_offsets.prolog = getSize();

  // rsp + 0 = return address
  sub(rsp, stack_size);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();

  // Save volatile registers
  EmitSaveVolatileRegs();
  mov(rdi, rsi);  // context
  mov(rsi, rbx);  // target PPC address
  mov(rax, reinterpret_cast<uint64_t>(&ResolveFunction));
  call(rax);

  EmitLoadVolatileRegs();

  code_offsets.epilog = getSize();

  add(rsp, stack_size);
  jmp(rax);
#else
  assert_always("Unknown platform ABI in resolve function!");
#endif

  code_offsets.tail = getSize();

  assert_zero(code_offsets.prolog);
  EmitFunctionInfo func_info = {};
  func_info.code_size.total = getSize();
  func_info.code_size.prolog = code_offsets.body - code_offsets.prolog;
  func_info.code_size.body = code_offsets.epilog - code_offsets.body;
  func_info.code_size.epilog = code_offsets.tail - code_offsets.epilog;
  func_info.code_size.tail = getSize() - code_offsets.tail;
  func_info.prolog_stack_alloc_offset =
      code_offsets.prolog_stack_alloc - code_offsets.prolog;
  func_info.stack_size = stack_size;

  void* fn = Emplace(func_info);
  return (ResolveFunctionThunk)fn;
}
// r11 = size of callers stack, r8 = return address w/ adjustment
// i'm not proud of this code, but it shouldn't be executed frequently at all
void* X64HelperEmitter::EmitGuestAndHostSynchronizeStackHelper() {
  _code_offsets code_offsets = {};
  code_offsets.prolog = getSize();
  push(rbx);
  mov(rbx, GetBackendCtxPtr(offsetof(x64::X64BackendContext, stackpoints)));
  mov(eax,
      GetBackendCtxPtr(offsetof(x64::X64BackendContext, current_stackpoint_depth)));

  lea(ecx, ptr[eax - 1]);
  mov(r9d, ptr[GetContextReg() + offsetof(ppc::PPCContext, r[1])]);

  Xbyak::Label looper{};
  Xbyak::Label loopout{};
  Xbyak::Label signed_underflow{};
  xor_(r12d, r12d);

  // todo: should use Loop instruction here if hasFastLoop,
  // currently xbyak does not support it but its super easy to modify xbyak to
  //  have it
  L(looper);
  imul(edx, ecx, sizeof(x64::X64BackendStackpoint));
  mov(r10d, ptr[rbx + rdx + offsetof(x64::X64BackendStackpoint, guest_stack_)]);

  cmp(r10d, r9d);

  jge(loopout, T_NEAR);

  inc(r12d);

  if (IsFeatureEnabled(kX64FlagsIndependentVars)) {
    dec(ecx);
  } else {
    sub(ecx, 1);
  }
  js(signed_underflow, T_NEAR);  // should be impossible!!

  jmp(looper, T_NEAR);
  L(loopout);
  Xbyak::Label skip_adjust{};
  cmp(r12d, 1);  // should never happen?
  jle(skip_adjust, T_NEAR);
  Xbyak::Label we_good{};

  // now we need to make sure that the return address matches

  // mov(r9d, ptr[GetContextReg() + offsetof(ppc::PPCContext, lr)]);
  pop(r9);  // guest retaddr
  // r10d = the guest_stack
  // while guest_stack is equal and return address is not equal, decrement

  Xbyak::Label search_for_retaddr{};
  Xbyak::Label we_good_but_increment{};
  L(search_for_retaddr);

  imul(edx, ecx, sizeof(x64::X64BackendStackpoint));

  cmp(r10d, ptr[rbx + rdx + offsetof(x64::X64BackendStackpoint, guest_stack_)]);

  jnz(we_good_but_increment, T_NEAR);

  cmp(r9d,
      ptr[rbx + rdx + offsetof(x64::X64BackendStackpoint, guest_return_address_)]);
  jz(we_good, T_NEAR);  // stack is equal, return address is equal, we've got
                        // our destination stack
  dec(ecx);
  jmp(search_for_retaddr, T_NEAR);
  Xbyak::Label checkbp{};

  L(we_good_but_increment);
  add(edx, sizeof(x64::X64BackendStackpoint));
  inc(ecx);
  jmp(checkbp, T_NEAR);
  L(we_good);
  // we're popping this return address, so go down by one
  sub(edx, sizeof(x64::X64BackendStackpoint));
  dec(ecx);
  L(checkbp);
  mov(rsp, ptr[rbx + rdx + offsetof(x64::X64BackendStackpoint, host_stack_)]);
  if (IsFeatureEnabled(kX64FlagsIndependentVars)) {
    inc(ecx);
  } else {
    add(ecx, 1);
  }

  sub(rsp, r11);  // adjust stack

  mov(GetBackendCtxPtr(offsetof(x64::X64BackendContext, current_stackpoint_depth)),
      ecx);  // set next stackpoint index to be after the one we restored to
  jmp(r8);
  L(skip_adjust);
  pop(rbx);
  jmp(r8);  // return to caller
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();

  L(signed_underflow);
  // find a good, compact way to signal error here
  //  maybe an invalid opcode that we execute, then detect in an exception
  // handler?

  this->DebugBreak();
  return EmitCurrentForOffsets(code_offsets);
}

void* X64HelperEmitter::EmitGuestAndHostSynchronizeStackSizeLoadThunk(
    void* sync_func, unsigned stack_element_size) {
  _code_offsets code_offsets = {};
  code_offsets.prolog = getSize();
  pop(r8);  // return address

  switch (stack_element_size) {
    case 4:
      mov(r11d, ptr[r8]);
      break;
    case 2:
      movzx(r11d, word[r8]);
      break;
    case 1:
      movzx(r11d, byte[r8]);
      break;
  }
  add(r8, stack_element_size);
  jmp(sync_func, T_NEAR);
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();
  return EmitCurrentForOffsets(code_offsets);
}

void* X64HelperEmitter::EmitScalarVRsqrteHelper() {
  _code_offsets code_offsets = {};

  Xbyak::Label L18, L2, L35, L4, L9, L8, L10, L11, L12, L13, L1;
  Xbyak::Label LC1, _LCPI3_1;
  Xbyak::Label handle_denormal_input;
  Xbyak::Label specialcheck_1, convert_to_signed_inf_and_ret,
      handle_oddball_denormal;

  auto emulate_lzcnt_helper_unary_reg = [this](auto& reg, auto& scratch_reg) {
    inLocalLabel();
    Xbyak::Label end_lzcnt;
    bsr(scratch_reg, reg);
    mov(reg, 0x20);
    jz(end_lzcnt);
    xor_(scratch_reg, 0x1F);
    mov(reg, scratch_reg);
    L(end_lzcnt);
    outLocalLabel();
  };

  vmovd(r8d, xmm0);
  vmovaps(xmm1, xmm0);
  mov(ecx, r8d);
  // extract mantissa
  and_(ecx, 0x7fffff);
  mov(edx, ecx);
  cmp(r8d, 0xff800000);
  jz(specialcheck_1, CodeGenerator::T_NEAR);
  // is exponent zero?
  test(r8d, 0x7f800000);
  jne(L18);
  test(ecx, ecx);
  jne(L2);

  L(L18);
  // extract biased exponent and unbias
  mov(r9d, r8d);
  shr(r9d, 23);
  movzx(r9d, r9b);
  lea(eax, ptr[r9 - 127]);
  cmp(r9d, 255);
  jne(L4);
  jmp(L35);

  L(L2);

  bt(GetBackendFlagsPtr(), kX64BackendNJMOn);
  jnc(handle_denormal_input, CodeGenerator::T_NEAR);

  // handle denormal input with NJM on
  // denorms get converted to zero w/ input sign, jump to our label
  // that handles inputs of 0 for this

  jmp(convert_to_signed_inf_and_ret);
  L(L35);

  vxorps(xmm0, xmm0, xmm0);
  mov(eax, 128);
  vcomiss(xmm1, xmm0);
  jb(L4);
  test(ecx, ecx);
  jne(L8);
  ret();

  L(L4);
  cmp(eax, 128);
  jne(L9);
  vxorps(xmm0, xmm0, xmm0);
  vcomiss(xmm0, xmm1);
  jbe(L9);
  vmovss(xmm2, ptr[rip + LC1]);
  vandps(xmm1, GetXmmConstPtr(XMMSignMaskF32));

  test(edx, edx);
  jne(L8);
  vorps(xmm0, xmm2, xmm2);
  ret();

  L(L9);
  test(edx, edx);
  je(L10);
  cmp(eax, 128);
  jne(L11);
  L(L8);
  or_(r8d, 0x400000);
  vmovd(xmm0, r8d);
  ret();
  L(L10);
  test(r9d, r9d);
  jne(L11);
  L(convert_to_signed_inf_and_ret);
  not_(r8d);
  shr(r8d, 31);

  lea(rdx, ptr[rip + _LCPI3_1]);
  shl(r8d, 2);
  vmovss(xmm0, ptr[r8 + rdx]);
  ret();

  L(L11);
  vxorps(xmm2, xmm2, xmm2);
  vmovss(xmm0, ptr[rip + LC1]);
  vcomiss(xmm2, xmm1);
  ja(L1, CodeGenerator::T_NEAR);
  mov(ecx, 127);
  sal(eax, 4);
  sub(ecx, r9d);
  mov(r9d, edx);
  and_(eax, 16);
  shr(edx, 9);
  shr(r9d, 19);
  and_(edx, 1023);
  sar(ecx, 1);
  or_(eax, r9d);
  xor_(eax, 16);
  mov(r9d, ptr[backend()->LookupXMMConstantAddress32(XMMVRsqrteTableStart) +
               rax * 4]);
  mov(eax, r9d);
  shr(r9d, 16);
  imul(edx, r9d);
  sal(eax, 10);
  and_(eax, 0x3fffc00);
  sub(eax, edx);
  bt(eax, 25);
  jc(L12);
  mov(edx, eax);
  add(ecx, 6);
  and_(edx, 0x1ffffff);

  if (IsFeatureEnabled(kX64EmitLZCNT)) {
    lzcnt(edx, edx);
  } else {
    emulate_lzcnt_helper_unary_reg(edx, r9d);
  }

  lea(r9d, ptr[rdx - 6]);
  sub(ecx, edx);
  if (IsFeatureEnabled(kX64EmitBMI2)) {
    shlx(eax, eax, r9d);
  } else {
    xchg(ecx, r9d);
    shl(eax, cl);
    xchg(ecx, r9d);
  }

  L(L12);
  test(al, 5);
  je(L13);
  test(al, 2);
  je(L13);
  add(eax, 4);

  L(L13);
  sal(ecx, 23);
  and_(r8d, 0x80000000);
  shr(eax, 2);
  add(ecx, 0x3f800000);
  and_(eax, 0x7fffff);
  vxorps(xmm1, xmm1);
  or_(ecx, r8d);
  or_(ecx, eax);
  vmovd(xmm0, ecx);
  vaddss(xmm0, xmm1);  // apply DAZ behavior to output

  L(L1);
  ret();

  L(handle_denormal_input);
  mov(r9d, r8d);
  and_(r9d, 0x7FFFFFFF);
  cmp(r9d, 0x400000);
  jz(handle_oddball_denormal);
  if (IsFeatureEnabled(kX64EmitLZCNT)) {
    lzcnt(ecx, ecx);
  } else {
    emulate_lzcnt_helper_unary_reg(ecx, r9d);
  }

  mov(r9d, 9);
  mov(eax, -118);
  lea(edx, ptr[rcx - 8]);
  sub(r9d, ecx);
  sub(eax, ecx);
  if (IsFeatureEnabled(kX64EmitBMI2)) {
    shlx(edx, r8d, edx);
  } else {
    xchg(ecx, edx);
    // esi is just the value of xmm0's low word, so we can restore it from there
    shl(r8d, cl);
    mov(ecx,
        edx);  // restore ecx, dont xchg because we're going to spoil edx anyway
    mov(edx, r8d);
    vmovd(r8d, xmm0);
  }
  and_(edx, 0x7ffffe);
  jmp(L4);

  L(specialcheck_1);
  // should be extremely rare
  vmovss(xmm0, ptr[rip + LC1]);
  ret();

  L(handle_oddball_denormal);
  not_(r8d);
  lea(r9, ptr[rip + LC1]);

  shr(r8d, 31);
  movss(xmm0, ptr[r9 + r8 * 4]);
  ret();

  L(_LCPI3_1);
  dd(0xFF800000);
  dd(0x7F800000);
  L(LC1);
  // the position of 7FC00000 here matters, this address will be indexed in
  // handle_oddball_denormal
  dd(0x7FC00000);
  dd(0x5F34FD00);

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.prolog = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();
  return EmitCurrentForOffsets(code_offsets);
}

void* X64HelperEmitter::EmitVectorVRsqrteHelper(void* scalar_helper) {
  _code_offsets code_offsets = {};
  Xbyak::Label check_scalar_operation_in_vmx, actual_vector_version;
  auto result_ptr =
      GetBackendCtxPtr(offsetof(x64::X64BackendContext, helper_scratch_xmms[0]));
  auto counter_ptr =
      GetBackendCtxPtr(offsetof(x64::X64BackendContext, helper_scratch_u64s[2]));
  counter_ptr.setBit(64);

  // shuffle and xor to check whether all lanes are equal
  // sadly has to leave the float pipeline for the vptest, which is moderate
  // yikes
  vmovhlps(xmm2, xmm0, xmm0);
  vmovsldup(xmm1, xmm0);
  vxorps(xmm1, xmm1, xmm0);
  vxorps(xmm2, xmm2, xmm0);
  vorps(xmm2, xmm1, xmm2);
  vptest(xmm2, xmm2);
  jnz(check_scalar_operation_in_vmx);
  // jmp(scalar_helper, CodeGenerator::T_NEAR);
  call(scalar_helper);
  vshufps(xmm0, xmm0, xmm0, 0);
  ret();

  L(check_scalar_operation_in_vmx);

  vptest(xmm0, ptr[backend()->LookupXMMConstantAddress(XMMThreeFloatMask)]);
  jnz(actual_vector_version);
  vshufps(xmm0, xmm0, xmm0, _MM_SHUFFLE(3, 3, 3, 3));
  call(scalar_helper);
  // this->DebugBreak();
  vinsertps(xmm0, xmm0, (3 << 4));

  vblendps(xmm0, xmm0, ptr[backend()->LookupXMMConstantAddress(XMMFloatInf)],
           0b0111);

  ret();

  L(actual_vector_version);

  xor_(ecx, ecx);
  vmovaps(result_ptr, xmm0);

  mov(counter_ptr, rcx);
  Xbyak::Label loop;

  L(loop);
  lea(rax, result_ptr);
  vmovss(xmm0, ptr[rax + rcx * 4]);
  call(scalar_helper);
  mov(rcx, counter_ptr);
  lea(rax, result_ptr);
  vmovss(ptr[rax + rcx * 4], xmm0);
  inc(ecx);
  cmp(ecx, 4);
  mov(counter_ptr, rcx);
  jl(loop);
  vmovaps(xmm0, result_ptr);
  ret();
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();
  code_offsets.prolog = getSize();
  return EmitCurrentForOffsets(code_offsets);
}

void* X64HelperEmitter::EmitFrsqrteHelper() {
  _code_offsets code_offsets = {};
  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();
  code_offsets.prolog = getSize();

  Xbyak::Label L2, L7, L6, L9, L1, L12, L24, L3, L25, frsqrte_table2, LC1;
  bt(GetBackendFlagsPtr(), kX64BackendNonIEEEMode);
  vmovq(rax, xmm0);
  jc(L24, CodeGenerator::T_NEAR);
  L(L2);
  mov(rcx, rax);
  add(rcx, rcx);
  je(L3, CodeGenerator::T_NEAR);
  mov(rdx, 0x7ff0000000000000ULL);
  vxorpd(xmm1, xmm1, xmm1);
  if (IsFeatureEnabled(kX64EmitBMI1)) {
    andn(rcx, rax, rdx);
  } else {
    mov(rcx, rax);
    not_(rcx);
    and_(rcx, rdx);
  }

  jne(L6);
  cmp(rax, rdx);
  je(L1, CodeGenerator::T_NEAR);
  mov(r8, rax);
  sal(r8, 12);
  jne(L7);
  vcomisd(xmm0, xmm1);
  jb(L12, CodeGenerator::T_NEAR);

  L(L7);
  mov(rdx, 0x7ff8000000000000ULL);
  or_(rax, rdx);
  vmovq(xmm1, rax);
  vmovapd(xmm0, xmm1);
  ret();

  L(L6);
  vcomisd(xmm1, xmm0);
  ja(L12, CodeGenerator::T_NEAR);
  mov(rcx, rax);
  mov(rdx, 0xfffffffffffffULL);
  shr(rcx, 52);
  and_(ecx, 2047);
  and_(rax, rdx);
  je(L9);
  test(ecx, ecx);
  je(L25, CodeGenerator::T_NEAR);

  L(L9);
  lea(edx, ptr[0 + rcx * 8]);
  shr(rax, 49);
  sub(ecx, 1023);
  and_(edx, 8);
  and_(eax, 7);
  shr(ecx, 1);
  or_(eax, edx);
  mov(edx, 1022);
  xor_(eax, 8);
  sub(edx, ecx);
  lea(rcx, ptr[rip + frsqrte_table2]);
  movzx(eax, byte[rax + rcx]);
  sal(rdx, 52);
  sal(rax, 44);
  or_(rax, rdx);
  vmovq(xmm1, rax);

  L(L1);
  vmovapd(xmm0, xmm1);
  ret();

  L(L12);
  vmovsd(xmm1, qword[rip + LC1]);
  vmovapd(xmm0, xmm1);
  ret();

  L(L24);
  mov(r8, rax);
  sal(r8, 12);
  je(L2);
  mov(rdx, 0x7ff0000000000000);
  test(rax, rdx);
  jne(L2);
  mov(rdx, 0x8000000000000000ULL);
  and_(rax, rdx);

  L(L3);
  mov(rdx, 0x8000000000000000ULL);
  and_(rax, rdx);
  mov(rdx, 0x7ff0000000000000ULL);
  or_(rax, rdx);
  vmovq(xmm1, rax);
  vmovapd(xmm0, xmm1);
  ret();

  L(L25);
  if (IsFeatureEnabled(kX64EmitLZCNT)) {
    lzcnt(rdx, rax);
  } else {
    Xbyak::Label end_lzcnt;
    bsr(rcx, rax);
    mov(rdx, 0x40);
    jz(end_lzcnt);
    xor_(rcx, 0x3F);
    mov(rdx, rcx);
    L(end_lzcnt);
  }
  lea(ecx, ptr[rdx - 11]);
  if (IsFeatureEnabled(kX64EmitBMI2)) {
    shlx(rax, rax, rcx);
  } else {
    shl(rax, cl);
  }
  mov(ecx, 12);
  sub(ecx, edx);
  jmp(L9, CodeGenerator::T_NEAR);

  L(frsqrte_table2);
  static constexpr unsigned char table_values[] = {
      241u, 216u, 192u, 168u, 152u, 136u, 128u, 112u,
      96u,  76u,  60u,  48u,  32u,  24u,  16u,  8u};
  db(table_values, sizeof(table_values));

  L(LC1);
  dd(0);
  dd(0x7ff80000);
  return EmitCurrentForOffsets(code_offsets);
}

void* X64HelperEmitter::EmitTryAcquireReservationHelper() {
  _code_offsets code_offsets = {};
  code_offsets.prolog = getSize();

  Xbyak::Label already_has_a_reservation;
  Xbyak::Label acquire_new_reservation;

  btr(GetBackendFlagsPtr(), kX64BackendHasReserveBit);
  mov(r8, GetBackendCtxPtr(offsetof(x64::X64BackendContext, reserve_helper_)));
  jc(already_has_a_reservation);

  shr(ecx, RESERVE_BLOCK_SHIFT);
  xor_(r9d, r9d);
  mov(edx, ecx);
  shr(edx, 6);  // divide by 64
  lea(rdx, ptr[r8 + rdx * 8]);
  and_(ecx, 64 - 1);

  lock();
  bts(qword[rdx], rcx);
  // set flag on local backend context for thread to indicate our previous
  // attempt to get the reservation succeeded
  setnc(r9b);  // success = bitmap did not have a set bit at the idx
  shl(r9b, kX64BackendHasReserveBit);

  mov(GetBackendCtxPtr(offsetof(x64::X64BackendContext, cached_reserve_offset)),
      rdx);
  mov(GetBackendCtxPtr(offsetof(x64::X64BackendContext, cached_reserve_bit)), ecx);

  or_(GetBackendCtxPtr(offsetof(x64::X64BackendContext, flags)), r9d);
  ret();
  L(already_has_a_reservation);
  DebugBreak();

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();
  return EmitCurrentForOffsets(code_offsets);
}
// ecx=guest addr
// r9 = host addr
// r8 = value
// if ZF is set and CF is set, we succeeded
void* X64HelperEmitter::EmitReservedStoreHelper(bool bit64) {
  _code_offsets code_offsets = {};
  code_offsets.prolog = getSize();
  Xbyak::Label done;
  Xbyak::Label reservation_isnt_for_our_addr;
  Xbyak::Label somehow_double_cleared;
  // carry must be set + zero flag must be set

  btr(GetBackendFlagsPtr(), kX64BackendHasReserveBit);

  jnc(done);

  mov(rax, GetBackendCtxPtr(offsetof(x64::X64BackendContext, reserve_helper_)));

  shr(ecx, RESERVE_BLOCK_SHIFT);
  mov(edx, ecx);
  shr(edx, 6);  // divide by 64
  lea(rdx, ptr[rax + rdx * 8]);
  // begin acquiring exclusive access to cacheline containing our bit
  prefetchw(ptr[rdx]);

  cmp(GetBackendCtxPtr(offsetof(x64::X64BackendContext, cached_reserve_offset)),
      rdx);
  jnz(reservation_isnt_for_our_addr);

  mov(rax,
      GetBackendCtxPtr(offsetof(x64::X64BackendContext, cached_reserve_value_)));

  // we need modulo bitsize, it turns out bittests' modulus behavior for the
  // bitoffset only applies for register operands, for memory ones we bug out
  // todo: actually, the above note may not be true, double check it
  and_(ecx, 64 - 1);
  cmp(GetBackendCtxPtr(offsetof(x64::X64BackendContext, cached_reserve_bit)), ecx);
  jnz(reservation_isnt_for_our_addr);

  // was our memory modified by kernel code or something?
  lock();
  if (bit64) {
    cmpxchg(ptr[r9], r8);

  } else {
    cmpxchg(ptr[r9], r8d);
  }
  // the ZF flag is unaffected by BTR! we exploit this for the retval

  // cancel our lock on the 65k block
  lock();
  btr(qword[rdx], rcx);

  jnc(somehow_double_cleared);

  L(done);
  // i don't care that theres a dependency on the prev value of rax atm
  // sadly theres no CF&ZF condition code
  setz(al);
  setc(ah);
  cmp(ax, 0x0101);
  ret();

  // could be the same label, but otherwise we don't know where we came from
  // when one gets triggered
  L(reservation_isnt_for_our_addr);
  DebugBreak();

  L(somehow_double_cleared);  // somehow, something else cleared our reserve??
  DebugBreak();

  code_offsets.prolog_stack_alloc = getSize();
  code_offsets.body = getSize();
  code_offsets.epilog = getSize();
  code_offsets.tail = getSize();
  return EmitCurrentForOffsets(code_offsets);
}

void X64HelperEmitter::EmitSaveVolatileRegs() {
  // Save off volatile registers.
  // mov(qword[rsp + offsetof(StackLayout::Thunk, r[0])], rax);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[1])], rcx);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[2])], rdx);
#if XE_PLATFORM_LINUX
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[3])], rsi);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[4])], rdi);
#endif
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[5])], r8);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[6])], r9);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[7])], r10);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[8])], r11);

  // vmovaps(qword[rsp + offsetof(StackLayout::Thunk, xmm[0])], xmm0);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[1])], xmm1);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[2])], xmm2);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[3])], xmm3);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[4])], xmm4);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[5])], xmm5);
}

void X64HelperEmitter::EmitLoadVolatileRegs() {
  // mov(rax, qword[rsp + offsetof(StackLayout::Thunk, r[0])]);
  mov(rcx, qword[rsp + offsetof(StackLayout::Thunk, r[1])]);
  mov(rdx, qword[rsp + offsetof(StackLayout::Thunk, r[2])]);
#if XE_PLATFORM_LINUX
  mov(rsi, qword[rsp + offsetof(StackLayout::Thunk, r[3])]);
  mov(rdi, qword[rsp + offsetof(StackLayout::Thunk, r[4])]);
#endif
  mov(r8, qword[rsp + offsetof(StackLayout::Thunk, r[5])]);
  mov(r9, qword[rsp + offsetof(StackLayout::Thunk, r[6])]);
  mov(r10, qword[rsp + offsetof(StackLayout::Thunk, r[7])]);
  mov(r11, qword[rsp + offsetof(StackLayout::Thunk, r[8])]);

  // vmovaps(xmm0, qword[rsp + offsetof(StackLayout::Thunk, xmm[0])]);
  vmovups(xmm1, qword[rsp + offsetof(StackLayout::Thunk, xmm[1])]);
  vmovups(xmm2, qword[rsp + offsetof(StackLayout::Thunk, xmm[2])]);
  vmovups(xmm3, qword[rsp + offsetof(StackLayout::Thunk, xmm[3])]);
  vmovups(xmm4, qword[rsp + offsetof(StackLayout::Thunk, xmm[4])]);
  vmovups(xmm5, qword[rsp + offsetof(StackLayout::Thunk, xmm[5])]);
}

void X64HelperEmitter::EmitSaveNonvolatileRegs() {
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[0])], rbx);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[1])], rbp);
#if XE_PLATFORM_WIN32
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[2])], rcx);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[3])], rsi);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[4])], rdi);
#endif
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[5])], r12);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[6])], r13);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[7])], r14);
  mov(qword[rsp + offsetof(StackLayout::Thunk, r[8])], r15);

  // SysV does not have nonvolatile XMM registers.
#if XE_PLATFORM_WIN32
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[0])], xmm6);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[1])], xmm7);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[2])], xmm8);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[3])], xmm9);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[4])], xmm10);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[5])], xmm11);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[6])], xmm12);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[7])], xmm13);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[8])], xmm14);
  vmovups(qword[rsp + offsetof(StackLayout::Thunk, xmm[9])], xmm15);
#endif
}

void X64HelperEmitter::EmitLoadNonvolatileRegs() {
  mov(rbx, qword[rsp + offsetof(StackLayout::Thunk, r[0])]);
  mov(rbp, qword[rsp + offsetof(StackLayout::Thunk, r[1])]);
#if XE_PLATFORM_WIN32
  mov(rcx, qword[rsp + offsetof(StackLayout::Thunk, r[2])]);
  mov(rsi, qword[rsp + offsetof(StackLayout::Thunk, r[3])]);
  mov(rdi, qword[rsp + offsetof(StackLayout::Thunk, r[4])]);
#endif
  mov(r12, qword[rsp + offsetof(StackLayout::Thunk, r[5])]);
  mov(r13, qword[rsp + offsetof(StackLayout::Thunk, r[6])]);
  mov(r14, qword[rsp + offsetof(StackLayout::Thunk, r[7])]);
  mov(r15, qword[rsp + offsetof(StackLayout::Thunk, r[8])]);

#if XE_PLATFORM_WIN32
  vmovups(xmm6, qword[rsp + offsetof(StackLayout::Thunk, xmm[0])]);
  vmovups(xmm7, qword[rsp + offsetof(StackLayout::Thunk, xmm[1])]);
  vmovups(xmm8, qword[rsp + offsetof(StackLayout::Thunk, xmm[2])]);
  vmovups(xmm9, qword[rsp + offsetof(StackLayout::Thunk, xmm[3])]);
  vmovups(xmm10, qword[rsp + offsetof(StackLayout::Thunk, xmm[4])]);
  vmovups(xmm11, qword[rsp + offsetof(StackLayout::Thunk, xmm[5])]);
  vmovups(xmm12, qword[rsp + offsetof(StackLayout::Thunk, xmm[6])]);
  vmovups(xmm13, qword[rsp + offsetof(StackLayout::Thunk, xmm[7])]);
  vmovups(xmm14, qword[rsp + offsetof(StackLayout::Thunk, xmm[8])]);
  vmovups(xmm15, qword[rsp + offsetof(StackLayout::Thunk, xmm[9])]);
#endif
}
void x64::X64Backend::InitializeBackendContext(void* ctx) {
  x64::X64BackendContext* bctx = BackendContextForGuestContext(ctx);
  bctx->mxcsr_fpu =
      DEFAULT_FPU_MXCSR;  // idk if this is right, check on rgh what the
                          // rounding on ppc is at startup

  /*
          todo: stackpoint arrays should be pooled virtual memory at the very
     least there may be some fancy virtual address tricks we can do here

  */

  bctx->stackpoints = ::cvars::enable_host_guest_stack_synchronization
                          ? new x64::X64BackendStackpoint[::cvars::max_stackpoints]
                          : nullptr;
  bctx->current_stackpoint_depth = 0;
  bctx->mxcsr_vmx = DEFAULT_VMX_MXCSR;
  bctx->flags = (1U << kX64BackendNJMOn);  // NJM on by default
  // https://media.discordapp.net/attachments/440280035056943104/1000765256643125308/unknown.png
  bctx->Ox1000 = 0x1000;
  bctx->guest_tick_count = Clock::GetGuestTickCountPointer();
  bctx->reserve_helper_ = &reserve_helper_;
}
void x64::X64Backend::DeinitializeBackendContext(void* ctx) {
  x64::X64BackendContext* bctx = BackendContextForGuestContext(ctx);

  if (bctx->stackpoints) {
    delete[] bctx->stackpoints;
    bctx->stackpoints = nullptr;
  }
}

void x64::X64Backend::PrepareForReentry(void* ctx) {
  x64::X64BackendContext* bctx = BackendContextForGuestContext(ctx);

  bctx->current_stackpoint_depth = 0;
}

constexpr uint32_t mxcsr_table[8] = {
    0x1F80, 0x7F80, 0x5F80, 0x3F80, 0x9F80, 0xFF80, 0xDF80, 0xBF80,
};

void x64::X64Backend::SetGuestRoundingMode(void* ctx, unsigned int mode) {
  x64::X64BackendContext* bctx = BackendContextForGuestContext(ctx);

  uint32_t control = mode & 7;
  _mm_setcsr(mxcsr_table[control]);
  bctx->mxcsr_fpu = mxcsr_table[control];
  auto ppc_context = reinterpret_cast<ppc::PPCContext*>(ctx);
  ppc_context->fpscr.bits.rn = control;
  ppc_context->fpscr.bits.ni = control >> 2;
}

bool x64::X64Backend::PopulatePseudoStacktrace(GuestPseudoStackTrace* st) {
  if (!::cvars::enable_host_guest_stack_synchronization) {
    return false;
  }

  ThreadState* thrd_state = ThreadState::Get();
  if (!thrd_state) {
    return false;  // we're not a guest!
  }
  ppc::PPCContext* ctx = thrd_state->context();

  x64::X64BackendContext* backend_ctx = BackendContextForGuestContext(ctx);

  uint32_t depth = backend_ctx->current_stackpoint_depth - 1;
  if (static_cast<int32_t>(depth) < 1) {
    return false;
  }
  uint32_t num_entries_to_populate =
      std::min(MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES, depth);

  st->count = num_entries_to_populate;
  st->truncated_flag = num_entries_to_populate < depth ? 1 : 0;

  x64::X64BackendStackpoint* current_stackpoint =
      &backend_ctx->stackpoints[backend_ctx->current_stackpoint_depth - 1];

  for (uint32_t stp_index = 0; stp_index < num_entries_to_populate;
       ++stp_index) {
    st->return_addrs[stp_index] = current_stackpoint->guest_return_address_;
    current_stackpoint--;
  }
  return true;
}

#if XE_X64_PROFILER_AVAILABLE == 1
uint64_t* x64::X64Backend::GetProfilerRecordForFunction(uint32_t guest_address) {
  // who knows, we might want to compile different versions of a function one
  // day
  auto entry = profiler_data_.find(guest_address);

  if (entry != profiler_data_.end()) {
    return &entry->second;
  } else {
    profiler_data_[guest_address] = 0;

    return &profiler_data_[guest_address];
  }
}

#endif

// todo:flush cache
uint32_t x64::X64Backend::CreateGuestTrampoline(GuestTrampolineProc proc,
                                           void* userdata1, void* userdata2,
                                           bool longterm) {
  size_t new_index;
  if (longterm) {
    new_index = guest_trampoline_address_bitmap_.AcquireFromBack();
  } else {
    new_index = guest_trampoline_address_bitmap_.Acquire();
  }

  xenia_assert(new_index != (size_t)-1);

  uint8_t* write_pos =
      &guest_trampoline_memory_[sizeof(guest_trampoline_template) * new_index];

  memcpy(write_pos, guest_trampoline_template,
         sizeof(guest_trampoline_template));

  *reinterpret_cast<void**>(&write_pos[guest_trampoline_template_offset_arg1]) =
      userdata1;
  *reinterpret_cast<void**>(&write_pos[guest_trampoline_template_offset_arg2]) =
      userdata2;
  *reinterpret_cast<GuestTrampolineProc*>(
      &write_pos[guest_trampoline_template_offset_rcx]) = proc;
  *reinterpret_cast<GuestToHostThunk*>(
      &write_pos[guest_trampoline_template_offset_rax]) = guest_to_host_thunk_;

  uint32_t indirection_guest_addr =
      GUEST_TRAMPOLINE_BASE +
      (static_cast<uint32_t>(new_index) * GUEST_TRAMPOLINE_MIN_LEN);

  code_cache()->AddIndirection(
      indirection_guest_addr,
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(write_pos)));

  return indirection_guest_addr;
}

void x64::X64Backend::FreeGuestTrampoline(uint32_t trampoline_addr) {
  xenia_assert(trampoline_addr >= GUEST_TRAMPOLINE_BASE &&
               trampoline_addr < GUEST_TRAMPOLINE_END);
  size_t index =
      (trampoline_addr - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;
  guest_trampoline_address_bitmap_.Release(index);
}
}  // namespace x64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
