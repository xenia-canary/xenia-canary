/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_DEBUG_GDB_GDBSTUB_H_
#define XENIA_DEBUG_GDB_GDBSTUB_H_

#include <memory>
#include <queue>
#include <vector>

#include "xenia/base/host_thread_context.h"
#include "xenia/base/socket.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/xbox.h"

namespace xe {
namespace debug {
namespace gdb {

class GDBStub : public cpu::DebugListener {
  enum class ControlCode : char {
    Ack = '+',
    Nack = '-',
    PacketStart = '$',
    PacketEnd = '#',
    Interrupt = '\03',
  };

  enum class SignalCode : uint8_t { SIGILL = 4, SIGTRAP = 5, SIGSEGV = 11 };

  enum class RegisterIndex : int {
    GPR0 = 0,
    FPR0 = 32,
    PC = 64,
    MSR = 65,
    CR = 66,
    LR = 67,
    CTR = 68,
    XER = 69,
    FPSCR = 70
  };

 public:
  virtual ~GDBStub();

  static std::unique_ptr<GDBStub> Create(Emulator* emulator, int listen_port);

  Emulator* emulator() const { return emulator_; }

  void OnFocus() override;
  void OnDetached() override;
  void OnUnhandledException(Exception* ex) override;
  void OnExecutionPaused() override;
  void OnExecutionContinued() override;
  void OnExecutionEnded() override;
  void OnStepCompleted(cpu::ThreadDebugInfo* thread_info) override;
  void OnBreakpointHit(cpu::Breakpoint* breakpoint,
                       cpu::ThreadDebugInfo* thread_info) override;
  void OnDebugPrint(const std::string_view message) override;

 private:
  struct GDBCommand {
    std::string cmd{};
    std::string data{};
    uint8_t checksum{};
  };

  struct GDBClient {
    std::unique_ptr<Socket> socket;
    bool no_ack_mode = false;
    std::string receive_buffer;
  };

  explicit GDBStub(Emulator* emulator, int listen_port);
  bool Initialize();

  void Listen(GDBClient& client);
  void SendPacket(GDBClient& client, const std::string& data);
  bool ProcessIncomingData(GDBClient& client);
  bool ParsePacket(const std::string& packet, GDBCommand& out_cmd);
  std::string HandleGDBCommand(GDBClient& client, const GDBCommand& command);

  void UpdateCache();

  std::string DebuggerDetached();
  std::string RegisterRead(xe::cpu::ThreadDebugInfo* thread, uint32_t rid);
  std::string RegisterWrite(xe::cpu::ThreadDebugInfo* thread, uint32_t rid,
                            const std::string_view value);
  std::string RegisterRead(const std::string& data);
  std::string RegisterWrite(const std::string& data);
  std::string RegisterReadAll();
  std::string RegisterWriteAll(const std::string& data);
  std::string ExecutionPause();
  std::string ExecutionContinue();
  std::string ExecutionStep();
  std::string MemoryRead(const std::string& data);
  std::string MemoryWrite(const std::string& data);
  std::string BuildThreadList();
  std::string QueryPacket(GDBClient& client, const std::string& data);

  std::string GetThreadStateReply(uint32_t thread_id, SignalCode signal);

  bool CreateCodeBreakpoint(uint64_t address);
  void DeleteCodeBreakpoint(uint64_t address);
  void DeleteCodeBreakpoint(cpu::Breakpoint* breakpoint);
  cpu::Breakpoint* LookupBreakpointAtAddress(uint64_t address);

  Emulator* emulator_ = nullptr;
  cpu::Processor* processor_ = nullptr;

  int listen_port_;
  std::unique_ptr<xe::SocketServer> socket_;

  std::mutex mtx_;
  bool stop_thread_ = false;

  xe::global_critical_region global_critical_region_;

  struct EmulatorStateCache {
    uint32_t cur_thread_id = -1;
    uint32_t last_bp_thread_id = -1;

    uint64_t notify_bp_guest_address = -1;
    uint32_t notify_thread_id = -1;
    bool notify_stopped = false;
    std::optional<Exception::AccessViolationOperation>
        notify_exception_access_violation;
    std::optional<Exception::Code> notify_exception_code;

    std::queue<std::string> notify_debug_prints;

    bool is_stopped = false;
    std::vector<kernel::object_ref<kernel::XModule>> modules;
    std::vector<cpu::ThreadDebugInfo*> thread_debug_infos;

    struct {
      std::vector<std::unique_ptr<cpu::Breakpoint>> all_breakpoints;
      std::unordered_map<uint32_t, cpu::Breakpoint*>
          code_breakpoints_by_guest_address;
    } breakpoints;

    cpu::ThreadDebugInfo* thread_info(int threadId) {
      for (auto& thread : thread_debug_infos) {
        if (thread->thread_id == threadId) {
          return thread;
        }
      }
      return nullptr;
    }
    cpu::ThreadDebugInfo* cur_thread_info() {
      return thread_info(cur_thread_id);
    }
  } cache_;
};

}  // namespace gdb
}  // namespace debug
}  // namespace xe

#endif  // XENIA_DEBUG_UI_DEBUG_WINDOW_H_
