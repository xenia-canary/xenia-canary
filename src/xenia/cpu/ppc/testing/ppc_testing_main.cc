/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH

#if XE_COMPILER_MSVC
#include "xenia/base/platform_win.h"
#else
#include <sys/wait.h>
#include <unistd.h>
#endif  // XE_COMPILER_MSVC

DEFINE_path(test_path, "src/xenia/cpu/ppc/testing/",
            "Directory scanned for test files.", "Other");
DEFINE_path(test_bin_path, "src/xenia/cpu/ppc/testing/bin/",
            "Directory with binary outputs of the test files.", "Other");
DEFINE_path(test_skip_file, "src/xenia/cpu/ppc/testing/skip.txt",
            "File containing test case names to skip (one per line).", "Other");
DEFINE_transient_string(test_name, "", "Test suite name.", "General");

namespace xe {
namespace cpu {
namespace test {

using xe::cpu::ppc::PPCContext;
using namespace xe::literals;

typedef std::vector<std::pair<std::string, std::string>> AnnotationList;

constexpr uint32_t START_ADDRESS = 0x80000000;

// Load skip list from file
std::unordered_set<std::string> LoadSkipList(
    const std::filesystem::path& skip_file_path) {
  std::unordered_set<std::string> skip_list;

  FILE* f = filesystem::OpenFile(skip_file_path, "r");
  if (!f) {
    // Skip file doesn't exist or can't be opened - that's okay
    return skip_list;
  }

  char line_buffer[BUFSIZ];
  while (fgets(line_buffer, sizeof(line_buffer), f)) {
    // Remove trailing whitespace/newline
    char* end = line_buffer + strlen(line_buffer) - 1;
    while (end >= line_buffer &&
           (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
      *end = '\0';
      --end;
    }

    // Skip empty lines and comments
    if (strlen(line_buffer) == 0 || line_buffer[0] == '#') {
      continue;
    }

    skip_list.insert(std::string(line_buffer));
  }

  fclose(f);
  return skip_list;
}

struct TestCase {
  TestCase(uint32_t address, std::string& name)
      : address(address), name(name) {}
  uint32_t address;
  std::string name;
  AnnotationList annotations;
};

class TestSuite {
 public:
  TestSuite(const std::filesystem::path& src_file_path)
      : src_file_path_(src_file_path) {
    auto name = src_file_path.filename();
    name = name.replace_extension();

    name_ = xe::path_to_utf8(name);
    map_file_path_ = cvars::test_bin_path / name.replace_extension(".map");
    bin_file_path_ = cvars::test_bin_path / name.replace_extension(".bin");
  }

  bool Load() {
    if (!ReadMap()) {
      XELOGE("Unable to read map for test {}", src_file_path_);
      return false;
    }
    if (!ReadAnnotations()) {
      XELOGE("Unable to read annotations for test {}", src_file_path_);
      return false;
    }
    return true;
  }

  const std::string& name() const { return name_; }
  const std::filesystem::path& src_file_path() const { return src_file_path_; }
  const std::filesystem::path& map_file_path() const { return map_file_path_; }
  const std::filesystem::path& bin_file_path() const { return bin_file_path_; }
  std::vector<TestCase>& test_cases() { return test_cases_; }

 private:
  std::string name_;
  std::filesystem::path src_file_path_;
  std::filesystem::path map_file_path_;
  std::filesystem::path bin_file_path_;
  std::vector<TestCase> test_cases_;

  TestCase* FindTestCase(const std::string_view name) {
    for (auto& test_case : test_cases_) {
      if (test_case.name == name) {
        return &test_case;
      }
    }
    return nullptr;
  }

  bool ReadMap() {
    FILE* f = filesystem::OpenFile(map_file_path_, "r");
    if (!f) {
      return false;
    }
    char line_buffer[BUFSIZ];
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
      if (!strlen(line_buffer)) {
        continue;
      }
      // 0000000000000000 t test_add1\n
      char* newline = strrchr(line_buffer, '\n');
      if (newline) {
        *newline = 0;
      }
      char* t_test_ = strstr(line_buffer, " t test_");
      if (!t_test_) {
        continue;
      }
      std::string address(line_buffer, t_test_ - line_buffer);
      std::string name(t_test_ + strlen(" t test_"));
      test_cases_.emplace_back(START_ADDRESS + std::stoul(address, 0, 16),
                               name);
    }
    fclose(f);
    return true;
  }

  bool ReadAnnotations() {
    TestCase* current_test_case = nullptr;
    FILE* f = filesystem::OpenFile(src_file_path_, "r");
    if (!f) {
      return false;
    }
    char line_buffer[BUFSIZ];
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
      if (!strlen(line_buffer)) {
        continue;
      }
      // Eat leading whitespace.
      char* start = line_buffer;
      while (*start == ' ') {
        ++start;
      }
      if (strncmp(start, "test_", strlen("test_")) == 0) {
        // Global test label.
        std::string label(start + strlen("test_"), strchr(start, ':'));
        current_test_case = FindTestCase(label);
        if (!current_test_case) {
          XELOGE("Test case {} not found in corresponding map for {}", label,
                 src_file_path_);
          return false;
        }
      } else if (strlen(start) > 3 && start[0] == '#' && start[1] == '_') {
        // Annotation.
        // We don't actually verify anything here.
        char* next_space = strchr(start + 3, ' ');
        if (next_space) {
          // Looks legit.
          std::string key(start + 3, next_space);
          std::string value(next_space + 1);
          while (value.find_last_of(" \t\n") == value.size() - 1) {
            value.erase(value.end() - 1);
          }
          if (!current_test_case) {
            XELOGE("Annotation outside of test case in {}", src_file_path_);
            return false;
          }
          current_test_case->annotations.emplace_back(key, value);
        }
      }
    }
    fclose(f);
    return true;
  }
};

class TestRunner {
 public:
  TestRunner() : memory_size_(64_MiB) {
    memory_.reset(new Memory());
    memory_->Initialize();
  }

  ~TestRunner() {
    thread_state_.reset();
    processor_.reset();
    memory_.reset();
  }

  bool Setup(TestSuite& suite) {
    // Reset thread state first so it can properly deinitialize with the
    // existing processor before we destroy the processor.
    thread_state_.reset();

    // Reset memory.
    memory_->Reset();

    std::unique_ptr<xe::cpu::backend::Backend> backend;
    if (!backend) {
#if XE_ARCH_AMD64
      if (cvars::cpu == "x64") {
        backend.reset(new xe::cpu::backend::x64::X64Backend());
      }
#elif XE_ARCH_ARM64
      if (cvars::cpu == "a64") {
        backend.reset(new xe::cpu::backend::a64::A64Backend());
      }
#endif  // XE_ARCH
      if (cvars::cpu == "any") {
        if (!backend) {
#if XE_ARCH_AMD64
          backend.reset(new xe::cpu::backend::x64::X64Backend());
#elif XE_ARCH_ARM64
          backend.reset(new xe::cpu::backend::a64::A64Backend());
#endif  // XE_ARCH
        }
      }
    }

    // Setup a fresh processor.
    processor_.reset(new Processor(memory_.get(), nullptr));
    processor_->Setup(std::move(backend));
    processor_->set_debug_info_flags(DebugInfoFlags::kDebugInfoAll);

    // Load the binary module.
    auto module = std::make_unique<xe::cpu::RawModule>(processor_.get());
    if (!module->LoadFile(START_ADDRESS, suite.bin_file_path())) {
      XELOGE("Unable to load test binary {}", suite.bin_file_path());
      return false;
    }
    processor_->AddModule(std::move(module));

    processor_->backend()->CommitExecutableRange(START_ADDRESS,
                                                 START_ADDRESS + 1024 * 1024);

    // Add dummy space for memory.
    processor_->memory()->LookupHeap(0)->AllocFixed(
        0x10001000, 0xEFFF, 0,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite);

    // Simulate a thread.
    uint32_t stack_size = 64 * 1024;
    uint32_t stack_address = START_ADDRESS - stack_size;
    uint32_t pcr_address = stack_address - 0x1000;
    thread_state_.reset(
        new ThreadState(processor_.get(), 0x100, stack_address, pcr_address));

    return true;
  }

  bool Run(TestCase& test_case) {
    // Setup test state from annotations.
    if (!SetupTestState(test_case)) {
      fprintf(stderr, "    [%s] Test setup failed\n", test_case.name.c_str());
      fflush(stderr);
      return false;
    }

#if XE_ARCH_AMD64
    // Reset MXCSR and backend flags to default FPU state before each test.
    // Without this, a previous test using VMX mode may leave FTZ/DAZ set,
    // causing subsequent scalar FPU tests to incorrectly flush denormals.
    _mm_setcsr(xe::cpu::backend::x64::DEFAULT_FPU_MXCSR);
    {
      auto* x64_backend = static_cast<xe::cpu::backend::x64::X64Backend*>(
          processor_->backend());
      auto* bctx =
          x64_backend->BackendContextForGuestContext(thread_state_->context());
      bctx->flags &= ~(1U << xe::cpu::backend::x64::kX64BackendMXCSRModeBit);
    }
#elif XE_ARCH_ARM64
    // Reset FPCR and backend flags to default FPU state before each test.
    {
      auto* a64_backend = static_cast<xe::cpu::backend::a64::A64Backend*>(
          processor_->backend());
      auto* bctx =
          a64_backend->BackendContextForGuestContext(thread_state_->context());
      bctx->flags &= ~(1U << xe::cpu::backend::a64::kA64BackendFPCRModeBit);
      // Explicitly reset the hardware FPCR to default FPU mode (0 = round
      // nearest, no flush-to-zero, no default-NaN). Without this, a previous
      // test that set VMX mode (FZ|DN) leaves the hardware FPCR dirty, and
      // subsequent scalar FP tests produce wrong NaN results because DN=1
      // causes ARM64 to return the default NaN instead of propagating inputs.
      a64_backend->SetGuestRoundingMode(thread_state_->context(), 0);
    }
#endif

    // Execute test.
    auto fn = processor_->ResolveFunction(test_case.address);
    if (!fn) {
      fprintf(stderr, "    [%s] Entry function not found\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }

    auto ctx = thread_state_->context();
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state_.get(), uint32_t(ctx->lr));

    // Assert test state expectations.
    bool result = CheckTestResults(test_case);
    if (!result) {
      // Also dump all disasm/etc.
      if (fn->is_guest()) {
        static_cast<xe::cpu::GuestFunction*>(fn)->debug_info()->Dump();
      }
    }

    return result;
  }

  bool SetupTestState(TestCase& test_case) {
    auto ppc_context = thread_state_->context();
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_IN") {
        size_t space_pos = it.second.find(" ");
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        ppc_context->SetRegFromString(reg_name.c_str(), reg_value.c_str());
      } else if (it.first == "MEMORY_IN") {
        size_t space_pos = it.second.find(" ");
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto p = memory_->TranslateVirtual(address);
        const char* c = bytes_str.c_str();
        while (*c) {
          while (*c == ' ') ++c;
          if (!*c) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          uint32_t b = std::strtoul(ccs, nullptr, 16);
          *p = static_cast<uint8_t>(b);
          ++p;
        }
      }
    }
    return true;
  }

  bool CheckTestResults(TestCase& test_case) {
    auto ppc_context = thread_state_->context();

    bool any_failed = false;
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_OUT") {
        size_t space_pos = it.second.find(" ");
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        std::string actual_value;
        if (!ppc_context->CompareRegWithString(
                reg_name.c_str(), reg_value.c_str(), actual_value)) {
          any_failed = true;
          fprintf(stderr, "    [%s] Register %s assert failed:\n",
                  test_case.name.c_str(), reg_name.c_str());
          fprintf(stderr, "      Expected: %s == %s\n", reg_name.c_str(),
                  reg_value.c_str());
          fprintf(stderr, "        Actual: %s == %s\n", reg_name.c_str(),
                  actual_value.c_str());
          fflush(stderr);
        }
      } else if (it.first == "MEMORY_OUT") {
        size_t space_pos = it.second.find(" ");
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto base_address = memory_->TranslateVirtual(address);
        auto p = base_address;
        const char* c = bytes_str.c_str();
        bool failed = false;
        size_t count = 0;
        StringBuffer expecteds;
        StringBuffer actuals;
        while (*c) {
          while (*c == ' ') ++c;
          if (!*c) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          count++;
          uint32_t current_address =
              address + static_cast<uint32_t>(p - base_address);
          uint32_t expected = std::strtoul(ccs, nullptr, 16);
          uint8_t actual = *p;

          expecteds.AppendFormat(" {:02X}", expected);
          actuals.AppendFormat(" {:02X}", actual);

          if (expected != actual) {
            any_failed = true;
            failed = true;
          }
          ++p;
        }
        if (failed) {
          fprintf(stderr, "    [%s] Memory %s assert failed:\n",
                  test_case.name.c_str(), address_str.c_str());
          fprintf(stderr, "      Expected:%s\n", expecteds.to_string().c_str());
          fprintf(stderr, "        Actual:%s\n", actuals.to_string().c_str());
          fflush(stderr);
        }
      }
    }
    return !any_failed;
  }

  size_t memory_size_;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  std::unique_ptr<ThreadState> thread_state_;
};

bool DiscoverTests(const std::filesystem::path& test_path,
                   std::vector<std::filesystem::path>& test_files) {
  auto file_infos = xe::filesystem::ListFiles(test_path);
  for (auto& file_info : file_infos) {
    if (file_info.name.extension() == ".s") {
      // Only include test files (instr_*.s), not helper files
      auto filename = file_info.name.filename().string();
      if (filename.find("instr_") == 0) {
        test_files.push_back(test_path / file_info.name);
      }
    }
  }
  return true;
}

#if XE_COMPILER_MSVC
int filter(unsigned int code) {
  if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
    return EXCEPTION_EXECUTE_HANDLER;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // XE_COMPILER_MSVC

#if !XE_COMPILER_MSVC
// Run test in isolated child process to catch crashes
enum class TestResult {
  kPassed,
  kFailed,
  kCrashed,
};

TestResult RunTestInChildProcess(TestSuite& test_suite, TestCase& test_case) {
  pid_t pid = fork();

  if (pid == -1) {
    // Fork failed
    fprintf(stderr, "  [%s] TEST FAILED (fork failed)\n",
            test_case.name.c_str());
    fflush(stderr);
    return TestResult::kFailed;
  }

  if (pid == 0) {
    // Child process - create a fresh TestRunner to avoid inherited state issues
    // Use a scope block to ensure destructors run before _exit(),
    // otherwise shared memory objects in /dev/shm are never cleaned up.
    int exit_code;
    {
      TestRunner child_runner;
      if (!child_runner.Setup(test_suite)) {
        exit_code = 2;  // Setup failure
      } else if (child_runner.Run(test_case)) {
        exit_code = 0;  // Test passed
      } else {
        exit_code = 1;  // Test failed
      }
    }  // child_runner destructor runs here, cleaning up shm
    _exit(exit_code);
  }

  // Parent process - wait for child
  int status;
  pid_t result = waitpid(pid, &status, 0);

  if (result == -1) {
    fprintf(stderr, "  [%s] TEST FAILED (waitpid failed, pid %d)\n",
            test_case.name.c_str(), pid);
    fflush(stderr);
    return TestResult::kFailed;
  }

  if (WIFEXITED(status)) {
    int exit_code = WEXITSTATUS(status);
    if (exit_code == 0) {
      // Test passed - don't print anything
      return TestResult::kPassed;
    } else if (exit_code == 2) {
      fprintf(stderr, "  [%s] FAILED SETUP (exit code %d)\n",
              test_case.name.c_str(), exit_code);
      fflush(stderr);
      return TestResult::kFailed;
    } else {
      fprintf(stderr, "  [%s] FAILED (exit code %d)\n", test_case.name.c_str(),
              exit_code);
      fflush(stderr);
      return TestResult::kFailed;
    }
  }

  if (WIFSIGNALED(status)) {
    int signal = WTERMSIG(status);
    const char* signal_name = "UNKNOWN";
    switch (signal) {
      case SIGSEGV:
        signal_name = "SIGSEGV";
        break;
      case SIGILL:
        signal_name = "SIGILL";
        break;
      case SIGFPE:
        signal_name = "SIGFPE";
        break;
      case SIGBUS:
        signal_name = "SIGBUS";
        break;
      case SIGABRT:
        signal_name = "SIGABRT";
        break;
      case SIGTRAP:
        signal_name = "SIGTRAP";
        break;
    }
    fprintf(stderr, "  [%s] CRASHED (%s)\n", test_case.name.c_str(),
            signal_name);
    fflush(stderr);
    return TestResult::kCrashed;
  }

  fprintf(stderr, "  [%s] FAILED (unknown reason)\n", test_case.name.c_str());
  fflush(stderr);
  return TestResult::kFailed;
}
#endif  // !XE_COMPILER_MSVC

void ProtectedRunTest(TestSuite& test_suite, TestRunner& runner,
                      TestCase& test_case, int& failed_count,
                      int& passed_count) {
#if XE_COMPILER_MSVC
  try {
    if (!runner.Setup(test_suite)) {
      fprintf(stderr, "  [%s] FAILED SETUP\n", test_case.name.c_str());
      fflush(stderr);
      ++failed_count;
      return;
    }
    if (runner.Run(test_case)) {
      ++passed_count;
    } else {
      fprintf(stderr, "  [%s] FAILED\n", test_case.name.c_str());
      fflush(stderr);
      ++failed_count;
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "  [%s] CRASHED (C++ exception: %s)\n",
            test_case.name.c_str(), e.what());
    fflush(stderr);
    ++failed_count;
  }
#else
  // Use fork to isolate crashes on POSIX systems
  // Note: runner parameter is not used on POSIX
  (void)runner;  // Suppress unused parameter warning
  TestResult result = RunTestInChildProcess(test_suite, test_case);

  if (result == TestResult::kPassed) {
    ++passed_count;
  } else {
    ++failed_count;
  }
#endif  // XE_COMPILER_MSVC
}

bool RunTests(const std::vector<std::string>& test_names) {
  int result_code = 1;
  int failed_count = 0;
  int passed_count = 0;

#if XE_ARCH_AMD64
  XELOGI("Instruction feature mask {}.", cvars::x64_extension_mask);
#endif  // XE_ARCH_AMD64

  // Load skip list
  auto skip_list = LoadSkipList(cvars::test_skip_file);
  if (!skip_list.empty()) {
    fprintf(stderr, "Loaded skip list with %zu test cases to skip.\n",
            skip_list.size());
  } else {
    fprintf(stderr, "Warning: skip list is empty (path: %s)\n",
            cvars::test_skip_file.string().c_str());
  }

  // Build a set of requested test names for fast lookup
  std::unordered_set<std::string> test_name_filter(test_names.begin(),
                                                   test_names.end());

  auto test_path_root = cvars::test_path;
  std::vector<std::filesystem::path> test_files;
  if (!DiscoverTests(test_path_root, test_files)) {
    return false;
  }
  if (!test_files.size()) {
    XELOGE("No tests discovered - invalid path?");
    return false;
  }
  XELOGI("{} tests discovered.", test_files.size());
  XELOGI("");

  std::vector<TestSuite> test_suites;
  bool load_failed = false;
  for (auto& test_path : test_files) {
    TestSuite test_suite(test_path);
    if (!test_name_filter.empty() &&
        test_name_filter.find(test_suite.name()) == test_name_filter.end()) {
      continue;
    }
    if (!test_suite.Load()) {
      XELOGE("TEST SUITE {} FAILED TO LOAD", test_path);
      load_failed = true;
      continue;
    }
    test_suites.push_back(std::move(test_suite));
  }
  if (load_failed) {
    XELOGE("One or more test suites failed to load.");
  }

  XELOGI("{} tests loaded.", test_suites.size());

  // Count test cases across all suites, filtering out skipped tests
  int skipped_count = 0;
  size_t total_cases = 0;
  for (auto& test_suite : test_suites) {
    for (auto& test_case : test_suite.test_cases()) {
      if (skip_list.find(test_case.name) != skip_list.end()) {
        ++skipped_count;
      } else {
        ++total_cases;
      }
    }
  }

  if (skipped_count > 0) {
    fprintf(stderr, "Skipped %d test cases based on skip list.\n",
            skipped_count);
  }
  fprintf(stderr, "Running %zu test suites, %zu test cases...\n",
          test_suites.size(), total_cases);

#if XE_COMPILER_MSVC
  // On Windows, use a single shared test runner
  TestRunner runner;
#else
  // On POSIX, each test will create its own runner in a forked process
  // Pass a dummy value that won't be used
  TestRunner* runner_ptr = nullptr;
  TestRunner& runner = *runner_ptr;  // Never dereferenced on POSIX
#endif

  // Run tests grouped by suite, printing a dot after each suite completes
  for (auto& test_suite : test_suites) {
    bool suite_has_tests = false;
    for (auto& test_case : test_suite.test_cases()) {
      if (skip_list.find(test_case.name) != skip_list.end()) {
        continue;
      }
      suite_has_tests = true;
      ProtectedRunTest(test_suite, runner, test_case, failed_count,
                       passed_count);
    }
    if (suite_has_tests) {
      fprintf(stdout, ".");
      fflush(stdout);
    }
  }

  fprintf(stdout, "\n");
  fflush(stdout);
  fprintf(stderr, "Total tests: %d\n", failed_count + passed_count);
  fprintf(stderr, "Passed: %d\n", passed_count);
  fprintf(stderr, "Failed: %d\n", failed_count);
  fflush(stderr);

  return failed_count ? false : true;
}

int main(const std::vector<std::string>& args) {
  std::vector<std::string> test_names;
  // Collect test names from all positional arguments.
  // argv[0] is the program name, skip it. Also skip --flag arguments
  // since those are handled by cvar parsing.
  for (size_t i = 1; i < args.size(); ++i) {
    if (!args[i].empty() && args[i][0] != '-') {
      test_names.push_back(args[i]);
    }
  }
  // Fall back to --test_name flag if no positional args given
  if (test_names.empty() && !cvars::test_name.empty()) {
    test_names.push_back(cvars::test_name);
  }
  return RunTests(test_names) ? 0 : 1;
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia-cpu-ppc-test", xe::cpu::test::main,
                      "[test names...]", "test_name");
