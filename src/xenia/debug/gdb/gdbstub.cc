/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/debug/gdb/gdbstub.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "xenia/base/clock.h"
#include "xenia/base/debugging.h"
#include "xenia/base/fuzzy.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace debug {
namespace gdb {

using xe::cpu::Breakpoint;
using xe::kernel::XModule;
using xe::kernel::XObject;
using xe::kernel::XThread;

constexpr const char* kGdbReplyOK = "OK";
constexpr const char* kGdbReplyError = "E01";

// must start with l for debugger to accept it
constexpr char kMemoryMapXml[] =
    R"(l<?xml version="1.0"?>
<memory-map>
  <!-- Based on memory.cc Initialize() -->
  <memory type="ram" start="0x10000" length="0x3FFF0000"/>
  <memory type="ram" start="0x40000000" length="0x3F000000"/>
  <memory type="ram" start="0x80000000" length="0x10000000"/>
  <memory type="ram" start="0x90000000" length="0x10000000"/>
  <memory type="ram" start="0xA0000000" length="0x20000000"/>
  <memory type="ram" start="0xC0000000" length="0x20000000"/>
  <memory type="ram" start="0xE0000000" length="0x1FD00000"/>
</memory-map>
)";

// TODO: add power-altivec.xml (and update RegisterRead to support it)
constexpr char kTargetXml[] =
    R"(l<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
<feature name="org.gnu.gdb.power.core">
  <reg name="r0" bitsize="32" type="uint32"/>
  <reg name="r1" bitsize="32" type="uint32"/>
  <reg name="r2" bitsize="32" type="uint32"/>
  <reg name="r3" bitsize="32" type="uint32"/>
  <reg name="r4" bitsize="32" type="uint32"/>
  <reg name="r5" bitsize="32" type="uint32"/>
  <reg name="r6" bitsize="32" type="uint32"/>
  <reg name="r7" bitsize="32" type="uint32"/>
  <reg name="r8" bitsize="32" type="uint32"/>
  <reg name="r9" bitsize="32" type="uint32"/>
  <reg name="r10" bitsize="32" type="uint32"/>
  <reg name="r11" bitsize="32" type="uint32"/>
  <reg name="r12" bitsize="32" type="uint32"/>
  <reg name="r13" bitsize="32" type="uint32"/>
  <reg name="r14" bitsize="32" type="uint32"/>
  <reg name="r15" bitsize="32" type="uint32"/>
  <reg name="r16" bitsize="32" type="uint32"/>
  <reg name="r17" bitsize="32" type="uint32"/>
  <reg name="r18" bitsize="32" type="uint32"/>
  <reg name="r19" bitsize="32" type="uint32"/>
  <reg name="r20" bitsize="32" type="uint32"/>
  <reg name="r21" bitsize="32" type="uint32"/>
  <reg name="r22" bitsize="32" type="uint32"/>
  <reg name="r23" bitsize="32" type="uint32"/>
  <reg name="r24" bitsize="32" type="uint32"/>
  <reg name="r25" bitsize="32" type="uint32"/>
  <reg name="r26" bitsize="32" type="uint32"/>
  <reg name="r27" bitsize="32" type="uint32"/>
  <reg name="r28" bitsize="32" type="uint32"/>
  <reg name="r29" bitsize="32" type="uint32"/>
  <reg name="r30" bitsize="32" type="uint32"/>
  <reg name="r31" bitsize="32" type="uint32"/>

  <reg name="pc" bitsize="32" type="code_ptr" regnum="64"/>
  <reg name="msr" bitsize="32" type="uint32"/>
  <reg name="cr" bitsize="32" type="uint32"/>
  <reg name="lr" bitsize="32" type="code_ptr"/>
  <reg name="ctr" bitsize="32" type="uint32"/>
  <reg name="xer" bitsize="32" type="uint32"/>
</feature>
<feature name="org.gnu.gdb.power.fpu">
  <reg name="f0" bitsize="64" type="ieee_double" regnum="32"/>
  <reg name="f1" bitsize="64" type="ieee_double"/>
  <reg name="f2" bitsize="64" type="ieee_double"/>
  <reg name="f3" bitsize="64" type="ieee_double"/>
  <reg name="f4" bitsize="64" type="ieee_double"/>
  <reg name="f5" bitsize="64" type="ieee_double"/>
  <reg name="f6" bitsize="64" type="ieee_double"/>
  <reg name="f7" bitsize="64" type="ieee_double"/>
  <reg name="f8" bitsize="64" type="ieee_double"/>
  <reg name="f9" bitsize="64" type="ieee_double"/>
  <reg name="f10" bitsize="64" type="ieee_double"/>
  <reg name="f11" bitsize="64" type="ieee_double"/>
  <reg name="f12" bitsize="64" type="ieee_double"/>
  <reg name="f13" bitsize="64" type="ieee_double"/>
  <reg name="f14" bitsize="64" type="ieee_double"/>
  <reg name="f15" bitsize="64" type="ieee_double"/>
  <reg name="f16" bitsize="64" type="ieee_double"/>
  <reg name="f17" bitsize="64" type="ieee_double"/>
  <reg name="f18" bitsize="64" type="ieee_double"/>
  <reg name="f19" bitsize="64" type="ieee_double"/>
  <reg name="f20" bitsize="64" type="ieee_double"/>
  <reg name="f21" bitsize="64" type="ieee_double"/>
  <reg name="f22" bitsize="64" type="ieee_double"/>
  <reg name="f23" bitsize="64" type="ieee_double"/>
  <reg name="f24" bitsize="64" type="ieee_double"/>
  <reg name="f25" bitsize="64" type="ieee_double"/>
  <reg name="f26" bitsize="64" type="ieee_double"/>
  <reg name="f27" bitsize="64" type="ieee_double"/>
  <reg name="f28" bitsize="64" type="ieee_double"/>
  <reg name="f29" bitsize="64" type="ieee_double"/>
  <reg name="f30" bitsize="64" type="ieee_double"/>
  <reg name="f31" bitsize="64" type="ieee_double"/>

  <reg name="fpscr" bitsize="32" group="float" regnum="70"/>
</feature>
</target>
)";

// TODO: move these to string_util.h?
std::string to_hexbyte(uint8_t i) {
  std::string result = "00";
  uint8_t i1 = i & 0xF;
  uint8_t i2 = i >> 4;
  result[0] = i2 > 9 ? 'a' + i2 - 10 : '0' + i2;
  result[1] = i1 > 9 ? 'a' + i1 - 10 : '0' + i1;
  return result;
}

// Convert a hex char (0-9, a-f, A-F) to a byte
uint8_t from_hexchar(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return 0;
}

template <typename T>
inline std::string to_hex_string(const T* data, size_t count) {
  // Ensure that T is trivially copyable
  static_assert(std::is_trivially_copyable<T>::value,
                "T must be a trivially copyable type");

  static const char hex_chars[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(sizeof(T) * count * 2);
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
  for (size_t i = 0; i < sizeof(T) * count; ++i) {
    result += hex_chars[bytes[i] >> 4];
    result += hex_chars[bytes[i] & 0x0F];
  }
  return result;
}

GDBStub::GDBStub(Emulator* emulator, int listen_port)
    : emulator_(emulator),
      processor_(emulator->processor()),
      listen_port_(listen_port) {}

GDBStub::~GDBStub() { stop_thread_ = true; }

std::unique_ptr<GDBStub> GDBStub::Create(Emulator* emulator, int listen_port) {
  std::unique_ptr<GDBStub> debugger(new GDBStub(emulator, listen_port));
  if (!debugger->Initialize()) {
    xe::FatalError("GDBStub::Create: Failed to initialize GDB stub");
    return nullptr;
  }
  return debugger;
}

bool GDBStub::Initialize() {
  socket_ = xe::SocketServer::Create(listen_port_,
                                     [this](std::unique_ptr<Socket> socket) {
                                       GDBClient client;
                                       client.socket = std::move(socket);
                                       Listen(client);
                                     });

  UpdateCache();
  return true;
}

void GDBStub::Listen(GDBClient& client) {
  // Client is connected - pause execution
  ExecutionPause();
  UpdateCache();

  client.socket->set_nonblocking(true);

  std::string receive_buffer;

  while (!stop_thread_) {
    if (!client.socket->is_connected()) {
      break;
    }

    if (!ProcessIncomingData(client)) {
      if (!client.socket->is_connected()) {
        break;
      }
      // No data available, can do other work or sleep
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      // Check if we need to notify client about anything
      {
        std::unique_lock<std::mutex> lock(mtx_);
        if (cache_.notify_stopped) {
          if (cache_.notify_thread_id != -1) {
            cache_.cur_thread_id = cache_.notify_thread_id;
          }

          SignalCode signal = SignalCode::SIGTRAP;
          if (cache_.notify_exception_code.has_value()) {
            if (cache_.notify_exception_code ==
                xe::Exception::Code::kIllegalInstruction) {
              signal = SignalCode::SIGILL;
            } else {
              signal = SignalCode::SIGSEGV;
            }

            cache_.notify_exception_code.reset();
            cache_.notify_exception_access_violation.reset();
          }

          SendPacket(client,
                     GetThreadStateReply(cache_.notify_thread_id, signal));
          cache_.notify_thread_id = -1;
          cache_.notify_stopped = false;
        } else if (cache_.notify_debug_prints.size()) {
          // Send the oldest message in our queue, only send 1 per loop to
          // reduce spamming the client & process any incoming packets
          std::string& message = cache_.notify_debug_prints.front();
          SendPacket(client,
                     "O" + to_hex_string(message.c_str(), message.length()));
          cache_.notify_debug_prints.pop();
        }
      }
    }
  }
}

void GDBStub::SendPacket(GDBClient& client, const std::string& data) {
  std::stringstream ss;
  ss << char(ControlCode::PacketStart) << data << char(ControlCode::PacketEnd);

  uint8_t checksum = 0;
  for (char c : data) {
    checksum += c;
  }

  ss << std::hex << std::setw(2) << std::setfill('0') << (checksum & 0xff);
  std::string packet = ss.str();

  client.socket->Send(packet.c_str(), packet.size());
}

#ifdef DEBUG
std::string GetPacketFriendlyName(const std::string& packetCommand) {
  static const std::unordered_map<std::string, std::string> kGdbCommandNames = {
      {"?", "StartupQuery"},
      {"!", "EnableExtendedMode"},
      {"p", "RegRead"},
      {"P", "RegWrite"},
      {"g", "RegReadAll"},
      {"G", "RegWriteAll"},
      {"C", "Continue"},
      {"c", "continue"},
      {"s", "step"},
      {"vAttach", "vAttach"},
      {"m", "MemRead"},
      {"M", "MemWrite"},
      {"H", "SetThreadId"},
      {"Z", "BreakpointCreate"},
      {"z", "BreakpointDelete"},
      {"qXfer", "Xfer"},
      {"qSupported", "Supported"},
      {"qfThreadInfo", "qfThreadInfo"},
      {"qC", "GetThreadId"},
      {"D", "Detach"},
      {"k", "KillRequest"},
      {"\03", "Break"},
  };

  std::string packet_name = "";
  auto it = kGdbCommandNames.find(packetCommand);
  if (it != kGdbCommandNames.end()) {
    packet_name = it->second;
  }

  return packet_name;
}
#endif

bool GDBStub::ProcessIncomingData(GDBClient& client) {
  char buffer[1024];
  size_t received = client.socket->Receive(buffer, sizeof(buffer));
  if (received == uint64_t(-1) ||  // disconnected
      received == 0) {
    return false;
  }

  client.receive_buffer.append(buffer, received);

  // Hacky interrupt '\03' packet handling, some reason checksum isn't
  // attached to this?
  bool isInterrupt = buffer[0] == char(ControlCode::Interrupt) && received == 1;

  // Check if we've received a full packet yet, if not exit and allow caller
  // to try again
  size_t packet_end;
  while (isInterrupt ||
         (packet_end = client.receive_buffer.find('#')) != std::string::npos) {
    if (isInterrupt || packet_end + 2 < client.receive_buffer.length()) {
      std::string current_packet;
      if (isInterrupt) {
        current_packet = char(ControlCode::Interrupt);
        client.receive_buffer = "";
        isInterrupt = false;
      } else {
        current_packet = client.receive_buffer.substr(0, packet_end + 3);
        client.receive_buffer = client.receive_buffer.substr(packet_end + 3);
      }

      GDBCommand command;
      if (ParsePacket(current_packet, command)) {
#ifdef DEBUG
        auto packet_name = GetPacketFriendlyName(command.cmd);

        debugging::DebugPrint("GDBStub: Packet {}({})\n",
                              packet_name.empty() ? command.cmd : packet_name,
                              command.data);
#endif

        if (!client.no_ack_mode) {
          ControlCode result = ControlCode::Ack;
          client.socket->Send(&result, 1);
        }

        std::string response = HandleGDBCommand(client, command);
        SendPacket(client, response);
      } else {
        if (!client.no_ack_mode) {
          ControlCode result = ControlCode::Nack;
          client.socket->Send(&result, 1);
        }
      }
    } else {
      break;
    }
  }

  return true;
}

bool GDBStub::ParsePacket(const std::string& packet, GDBCommand& out_cmd) {
  // Index to track position in packet
  size_t buffer_index = 0;

  // Read a character from the buffer and increment index
  auto ReadCharFromBuffer = [&]() -> char {
    if (buffer_index >= packet.size()) {
      return '\0';
    }
    return packet[buffer_index++];
  };

  // Parse two hex digits from buffer
  auto ReadHexByteFromBuffer = [&]() -> char {
    if (buffer_index + 2 > packet.size()) {
      return 0;
    }
    char high = packet[buffer_index++];
    char low = packet[buffer_index++];
    return (from_hexchar(high) << 4) | from_hexchar(low);
  };

  // Read the first character from the buffer
  char c = ReadCharFromBuffer();

  // Expecting start of packet '$'
  if (c != char(ControlCode::PacketStart)) {
    // gdb starts conversation with + for some reason
    if (c == char(ControlCode::Ack)) {
      c = ReadCharFromBuffer();
    }
    // and IDA sometimes has double +, grr
    if (c == char(ControlCode::Ack)) {
      c = ReadCharFromBuffer();
    }
    // Interrupt is special, handle it without checking checksum
    if (c == char(ControlCode::Interrupt)) {
      out_cmd.cmd = char(ControlCode::Interrupt);
      out_cmd.data = "";
      out_cmd.checksum = 0;
      return true;
    }
    if (c != char(ControlCode::PacketStart)) {
      return false;
    }
  }

  // Clear packet data
  out_cmd.cmd = "";
  out_cmd.data = "";
  out_cmd.checksum = 0;
  bool cmd_part = true;
  uint8_t checksum = 0;

  // Parse packet content
  while (true) {
    c = ReadCharFromBuffer();

    // If we reach the end of the buffer or hit '#', stop
    if (c == '\0' || c == char(ControlCode::PacketEnd)) {
      break;
    }

    checksum = (checksum + static_cast<uint8_t>(c)) % 256;

    // Handle escaped characters
    if (c == '}') {
      c = ReadCharFromBuffer() ^ 0x20;  // Read next char and XOR with 0x20
      checksum = (checksum + static_cast<uint8_t>(c)) % 256;
    }

    // Command-data splitters: check for ':', '.', or ';'
    if (cmd_part && (c == ':' || c == '.' || c == ';')) {
      cmd_part = false;
    }

    if (cmd_part) {
      out_cmd.cmd += c;

      // Only 'q' and 'v' commands can have multi-char commands
      if (out_cmd.cmd.length() == 1 && c != 'q' && c != 'v') {
        cmd_part = false;
      }
    } else {
      out_cmd.data += c;
    }
  }

  // Now read & compare the checksum
  out_cmd.checksum = ReadHexByteFromBuffer();
  return out_cmd.checksum == checksum;
}

void GDBStub::UpdateCache() {
  auto kernel_state = emulator_->kernel_state();
  auto object_table = kernel_state->object_table();

  std::unique_lock<std::mutex> lock(mtx_);

  cache_.is_stopped =
      processor_->execution_state() != cpu::ExecutionState::kRunning;
  cache_.notify_stopped = cache_.is_stopped;
  if (!cache_.is_stopped) {
    // Early exit - the rest of the data is kept stale on purpose.
    return;
  }

  // Fetch module listing.
  // We hold refs so that none are unloaded.
  cache_.modules =
      object_table->GetObjectsByType<XModule>(XObject::Type::Module);

  cache_.thread_debug_infos = processor_->QueryThreadDebugInfos();
  if (cache_.cur_thread_id == -1) {
    cache_.cur_thread_id = emulator_->main_thread_id();
  }
}

std::string GDBStub::RegisterRead(xe::cpu::ThreadDebugInfo* thread,
                                  uint32_t rid) {
  // Send registers as 32-bit, otherwise some debuggers will switch to 64-bit
  // mode (eg. IDA will switch to 64-bit and refuse to allow decompiler to work
  // with it)
  //
  // TODO: add altivec/VMX registers here
  // TODO: add cvar to allow switch to 64-bit mode? (unsure if any x360 opcodes
  // use the upper 32-bits?)
  //
  // ids from gdb/features/rs6000/powerpc-64.c
  switch (RegisterIndex(rid)) {
    case RegisterIndex::PC: {
      //
      // If we recently hit a BP then debugger is likely asking for registers
      // for it
      //
      // Lie about the PC and say it's the BP addr, since the PC (derived from
      // X64 host addr) might not match expected BP addr
      if (cache_.notify_bp_guest_address != -1) {
        auto ret = string_util::to_hex_string(
            (uint32_t)cache_.notify_bp_guest_address);
        cache_.notify_bp_guest_address = -1;
        return ret;
      }
      // Search for first frame that has guest_pc attached, GDB doesn't care
      // about host
      for (auto& frame : thread->frames) {
        if (frame.guest_pc != 0) {
          return string_util::to_hex_string((uint32_t)frame.guest_pc);
        }
      }
      return string_util::to_hex_string((uint32_t)0);
    }
    case RegisterIndex::MSR:
      return string_util::to_hex_string((uint32_t)thread->guest_context.msr);
    case RegisterIndex::CR:
      return string_util::to_hex_string((uint32_t)thread->guest_context.cr());
    case RegisterIndex::LR:
      return string_util::to_hex_string((uint32_t)thread->guest_context.lr);
    case RegisterIndex::CTR:
      return string_util::to_hex_string((uint32_t)thread->guest_context.ctr);
    case RegisterIndex::XER:
      return std::string(8, 'x');
    case RegisterIndex::FPSCR:
      return string_util::to_hex_string(thread->guest_context.fpscr.value);
  }

  if (rid >= int(RegisterIndex::PC)) {
    return "";
  }

  // fpr
  if (rid >= int(RegisterIndex::FPR0)) {
    return string_util::to_hex_string(thread->guest_context.f[rid - 32]);
  }

  // gpr
  return string_util::to_hex_string((uint32_t)thread->guest_context.r[rid]);
}

std::string GDBStub::RegisterWrite(xe::cpu::ThreadDebugInfo* thread,
                                   uint32_t rid, const std::string_view value) {
  // Have to update the main thread context, and the ThreadDebugInfo context
  auto* guest_context = thread->thread->thread_state()->context();
  switch (RegisterIndex(rid)) {
    case RegisterIndex::PC:
      return kGdbReplyError;  // TODO: figure a way to change this
    case RegisterIndex::MSR:
      guest_context->msr = string_util::from_string<uint32_t>(value, true);
      thread->guest_context.msr = guest_context->msr;
      return kGdbReplyOK;
    case RegisterIndex::CR:
      return kGdbReplyError;  // TODO: figure a way to change this
    case RegisterIndex::LR:
      guest_context->lr = string_util::from_string<uint32_t>(value, true);
      thread->guest_context.lr = guest_context->lr;
      return kGdbReplyOK;
    case RegisterIndex::CTR:
      guest_context->ctr = string_util::from_string<uint32_t>(value, true);
      thread->guest_context.ctr = guest_context->ctr;
      return kGdbReplyOK;
    case RegisterIndex::XER:
      return kGdbReplyError;
    case RegisterIndex::FPSCR:
      guest_context->fpscr.value =
          string_util::from_string<uint32_t>(value, true);
      thread->guest_context.fpscr.value = guest_context->fpscr.value;
      return kGdbReplyOK;
  }

  if (rid >= int(RegisterIndex::PC)) {
    return kGdbReplyError;
  }

  // fpr
  if (rid >= int(RegisterIndex::FPR0)) {
    guest_context->f[rid - 32] = string_util::from_string<double>(value, true);
    thread->guest_context.f[rid - 32] = guest_context->f[rid - 32];
    return kGdbReplyOK;
  }

  // gpr
  guest_context->r[rid] = string_util::from_string<uint32_t>(value, true);
  thread->guest_context.r[rid] = guest_context->r[rid];
  return kGdbReplyOK;
}

std::string GDBStub::RegisterRead(const std::string& data) {
  auto* thread = cache_.cur_thread_info();
  if (!thread) {
    return kGdbReplyError;
  }
  uint32_t rid = string_util::from_string<uint32_t>(data, true);
  std::string result = RegisterRead(thread, rid);
  if (result.empty()) {
    return kGdbReplyError;
  }
  return result;
}

std::string GDBStub::RegisterWrite(const std::string& data) {
  auto* thread = cache_.cur_thread_info();
  if (!thread) {
    return kGdbReplyError;
  }

  auto value_sep = data.find('=');
  if (value_sep == std::string::npos) {
    return kGdbReplyError;
  }

  uint32_t rid =
      string_util::from_string<uint32_t>(data.substr(0, value_sep), true);
  return RegisterWrite(thread, rid, data.data() + value_sep + 1);
}

std::string GDBStub::RegisterReadAll() {
  auto* thread = cache_.cur_thread_info();
  if (!thread) {
    return kGdbReplyError;
  }
  std::string result;
  result.reserve((39 * 8) + (32 * 16));
  for (int i = 0; i < 71; ++i) {
    result += RegisterRead(thread, i);
  }
  return result;
}

std::string GDBStub::RegisterWriteAll(const std::string& data) {
  auto* thread = cache_.cur_thread_info();
  if (!thread) {
    return kGdbReplyError;
  }

  int expected_size = (39 * 8) + (32 * 16);
  if (data.length() != expected_size) {
    return kGdbReplyError;
  }

  int string_offset = 0;
  for (int i = 0; i < 71; ++i) {
    int reg_size = 8;  // 8 hex-nibbles per 32-bit register
    if (i > 31 && i < 64) {
      reg_size = 16;  // 16 hex-nibbles for 64-bit FPR registers
    }

    std::string_view reg_data(data.data() + string_offset, reg_size);
    RegisterWrite(thread, i, reg_data);  // TODO: check return value

    string_offset += reg_size;
  }

  return kGdbReplyOK;
}

std::string GDBStub::ExecutionPause() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: ExecutionPause\n");
#endif
  if (processor_->execution_state() != cpu::ExecutionState::kRunning) {
    return kGdbReplyError;
  }
  processor_->Pause();
  return "";
}

std::string GDBStub::ExecutionContinue() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: ExecutionContinue\n");
#endif
  processor_->Continue();
  return "";
}

std::string GDBStub::ExecutionStep() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: ExecutionStep (thread {})\n",
                        cache_.last_bp_thread_id);
#endif
  std::unique_lock<std::mutex> lock(mtx_);

  if (cache_.last_bp_thread_id != -1) {
    processor_->StepGuestInstruction(cache_.last_bp_thread_id);
  }

  return "";
}

std::string GDBStub::MemoryRead(const std::string& data) {
  auto len_sep = data.find(',');

  if (len_sep == std::string::npos) {
    return kGdbReplyError;
  }

  uint32_t addr =
      string_util::from_string<uint32_t>(data.substr(0, len_sep), true);
  uint32_t len =
      string_util::from_string<uint32_t>(data.substr(len_sep + 1), true);

  std::string result;
  result.reserve(len * 2);

  auto global_lock = global_critical_region_.Acquire();

  // TODO: is there a better way to check if addr is valid?
  auto* heap = processor_->memory()->LookupHeap(addr);
  if (!heap) {
    return kGdbReplyError;
  }
  uint32_t protect = 0;
  if (!heap->QueryProtect(addr, &protect) ||
      (protect & kMemoryProtectRead) != kMemoryProtectRead) {
    return kGdbReplyError;
  }

  auto* mem = processor_->memory()->TranslateVirtual(addr);
  for (uint32_t i = 0; i < len; ++i) {
    result += to_hexbyte(*mem);
    mem++;
  }

  if (len && result.empty()) {
    return kGdbReplyError;  // nothing read
  }

  return result;
}

std::string GDBStub::MemoryWrite(const std::string& data) {
  auto len_sep = data.find(',');
  auto mem_sep = data.find(':');

  if (len_sep == std::string::npos || mem_sep == std::string::npos) {
    return kGdbReplyError;
  }

  uint32_t addr =
      string_util::from_string<uint32_t>(data.substr(0, len_sep), true);
  uint32_t len = string_util::from_string<uint32_t>(
      data.substr(len_sep + 1, mem_sep - (len_sep + 1)), true);

  auto global_lock = global_critical_region_.Acquire();

  auto* heap = processor_->memory()->LookupHeap(addr);
  if (!heap) {
    return kGdbReplyError;
  }

  // Check if they're trying to write to an executable function
  if (processor_->LookupFunction(addr) != nullptr) {
    // TODO: allow the write and ask processor to recompile if no breakpoints
    // are set there?
    return kGdbReplyError;  // error for now as writes here won't have an effect
  }

  uint32_t protect = 0;
  if (!heap->QueryProtect(addr, &protect) ||
      (protect & kMemoryProtectRead) != kMemoryProtectRead) {
    return kGdbReplyError;
  }

  if (len == 0) {
    return kGdbReplyOK;
  }

  uint32_t old_protect = 0;
  bool mem_unprotected =
      heap->Protect(addr, len, protect | kMemoryProtectWrite, &old_protect);
  if (!mem_unprotected) {
    return kGdbReplyError;
  }

  std::vector<uint8_t> mem_data;
  string_util::hex_string_to_array(mem_data,
                                   std::string_view(data.data() + mem_sep + 1));
  auto* mem = processor_->memory()->TranslateVirtual(addr);
  for (uint32_t i = 0; i < len; ++i) {
    mem[i] = mem_data[i];
  }

  heap->Protect(addr, len, old_protect);

  return kGdbReplyOK;
}

std::string GDBStub::BuildThreadList() {
  std::unique_lock<std::mutex> lock(mtx_);

  std::string buffer;
  buffer += "l<?xml version=\"1.0\"?>";
  buffer += "<threads>";

  for (int i = 0; i < cache_.thread_debug_infos.size(); i++) {
    auto& thread = cache_.thread_debug_infos[i];
    buffer += fmt::format(R"*(<thread id="{:x}" name="{}"></thread>)*",
                          thread->thread_id, thread->thread->thread_name());
  }

  buffer += "</threads>";
  return buffer;
}

std::string GDBStub::QueryPacket(GDBClient& client, const std::string& data) {
  if (data == "StartNoAckMode") {
    client.no_ack_mode = true;
    return kGdbReplyOK;
  }
  return kGdbReplyError;
}

std::string GDBStub::GetThreadStateReply(uint32_t thread_id,
                                         SignalCode signal) {
  auto* thread = cache_.thread_info(thread_id);

  if (thread_id != -1 && thread) {
    uint64_t pc_value = 0;
    for (auto& frame : thread->frames) {
      if (frame.guest_pc != 0) {
        pc_value = frame.guest_pc;
        break;
      }
    }

    // If BP was hit use the address of it, so debugger can match it up to its
    // BP list
    if (cache_.notify_bp_guest_address != -1) {
      pc_value = cache_.notify_bp_guest_address;
    }

    return fmt::format("T{:02x}{:02x}:{:08x};{:02x}:{:08x};thread:{:x};",
                       uint8_t(signal), int(RegisterIndex::PC),
                       uint32_t(pc_value), int(RegisterIndex::LR),
                       uint32_t(thread->guest_context.lr), thread_id);
  }
  return "S05";
}

bool GDBStub::CreateCodeBreakpoint(uint64_t address) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: Adding breakpoint: {:X}\n", address);
#endif
  std::unique_lock<std::mutex> lock(mtx_);

  auto* exe_addr = processor_->LookupFunction((uint32_t)address);
  if (!exe_addr) {
    return false;  // TODO: move this check to Breakpoint?
  }

  auto& state = cache_.breakpoints;
  auto breakpoint = std::make_unique<Breakpoint>(
      processor_, Breakpoint::AddressType::kGuest, address,
      [this](Breakpoint* breakpoint, cpu::ThreadDebugInfo* thread_info,
             uint64_t host_address) {
        OnBreakpointHit(breakpoint, thread_info);
      });

  // Fetch list of host addrs used by the new BP
  std::vector<uintptr_t> host_addresses;
  breakpoint->ForEachHostAddress([&host_addresses](uintptr_t host_address) {
    host_addresses.push_back(host_address);
  });

  auto& map = state.code_breakpoints_by_guest_address;
  for (auto& kvp : map) {
    if (kvp.first == breakpoint->guest_address()) {
      return false;  // Already exists!
    }
    for (auto& host_address : host_addresses) {
      if (kvp.second->ContainsHostAddress(host_address)) {
        return false;  // Host addr is in use by another BP already
      }
    }
  }

  map.emplace(breakpoint->guest_address(), breakpoint.get());

  processor_->AddBreakpoint(breakpoint.get());
  state.all_breakpoints.emplace_back(std::move(breakpoint));

  return true;
}

void GDBStub::DeleteCodeBreakpoint(uint64_t address) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: Deleting breakpoint: {:X}\n", address);
#endif
  auto* breakpoint = LookupBreakpointAtAddress(address);
  if (!breakpoint) {
    return;
  }
  DeleteCodeBreakpoint(breakpoint);
}

void GDBStub::DeleteCodeBreakpoint(Breakpoint* breakpoint) {
  std::unique_lock<std::mutex> lock(mtx_);
  auto& state = cache_.breakpoints;
  for (size_t i = 0; i < state.all_breakpoints.size(); ++i) {
    if (state.all_breakpoints[i].get() != breakpoint) {
      continue;
    }
    processor_->RemoveBreakpoint(breakpoint);

    auto& map = state.code_breakpoints_by_guest_address;
    auto it = map.find(breakpoint->guest_address());
    if (it != map.end()) {
      map.erase(it);
    }

    state.all_breakpoints.erase(state.all_breakpoints.begin() + i);
    break;
  }
}

Breakpoint* GDBStub::LookupBreakpointAtAddress(uint64_t address) {
  auto& state = cache_.breakpoints;
  auto& map = state.code_breakpoints_by_guest_address;
  auto it = map.find(static_cast<uint32_t>(address));
  return it == map.end() ? nullptr : it->second;
}

void GDBStub::OnFocus() {}

void GDBStub::OnDetached() {
  UpdateCache();

  std::unique_lock<std::mutex> lock(mtx_);
  // Delete all breakpoints
  auto& state = cache_.breakpoints;

  for (auto& breakpoint : state.all_breakpoints) {
    processor_->RemoveBreakpoint(breakpoint.get());
  }

  state.code_breakpoints_by_guest_address.clear();
  state.all_breakpoints.clear();

  if (processor_->execution_state() == cpu::ExecutionState::kPaused) {
    ExecutionContinue();
  }
}

void GDBStub::OnUnhandledException(Exception* ex) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnUnhandledException {} {}\n",
                        int(ex->code()), int(ex->access_violation_operation()));
#endif
  std::unique_lock<std::mutex> lock(mtx_);
  cache_.notify_exception_code = ex->code();
  cache_.notify_exception_access_violation = ex->access_violation_operation();
  cache_.notify_thread_id = cpu::Thread::GetCurrentThreadId();
}

void GDBStub::OnExecutionPaused() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnExecutionPaused\n");
#endif
  UpdateCache();
}

void GDBStub::OnExecutionContinued() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnExecutionContinued\n");
#endif
  UpdateCache();
}

void GDBStub::OnExecutionEnded() {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnExecutionEnded\n");
#endif
  UpdateCache();
}

void GDBStub::OnStepCompleted(cpu::ThreadDebugInfo* thread_info) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: OnStepCompleted\n");
#endif
  std::unique_lock<std::mutex> lock(mtx_);

  // Some debuggers like IDA will remove the current breakpoint & step into next
  // instruction, only re-adding BP after it's told about the step
  cache_.notify_thread_id = thread_info->thread_id;
  cache_.last_bp_thread_id = thread_info->thread_id;

  UpdateCache();
}

void GDBStub::OnBreakpointHit(Breakpoint* breakpoint,
                              cpu::ThreadDebugInfo* thread_info) {
#ifdef DEBUG
  debugging::DebugPrint("GDBStub: Breakpoint hit at {:X} (thread {})\n",
                        breakpoint->address(), thread_info->thread_id);
#endif

  std::unique_lock<std::mutex> lock(mtx_);

  cache_.notify_bp_guest_address = breakpoint->address();
  cache_.notify_thread_id = thread_info->thread_id;
  cache_.last_bp_thread_id = thread_info->thread_id;

  UpdateCache();
}

void GDBStub::OnDebugPrint(const std::string_view message) {
  std::unique_lock<std::mutex> lock(mtx_);
  cache_.notify_debug_prints.push(std::string(message));
}

std::string GDBStub::HandleGDBCommand(GDBClient& client,
                                      const GDBCommand& command) {
  static const std::unordered_map<std::string,
                                  std::function<std::string(const GDBCommand&)>>
      command_map = {
          // "sent when connection is first established to query the reason the
          // target halted"
          {"?",
           [&](const GDBCommand& cmd) {
             return "S05";  // tell debugger we're currently paused
           }},

          // Detach
          {"D",
           [&](const GDBCommand& cmd) {
             OnDetached();
             return kGdbReplyOK;
           }},

          // Kill request (just treat as detach for now)
          {"k",
           [&](const GDBCommand& cmd) {
             OnDetached();
             return kGdbReplyOK;
           }},

          // Enable extended mode
          {"!", [&](const GDBCommand& cmd) { return kGdbReplyOK; }},

          // Execution continue
          {"C", [&](const GDBCommand& cmd) { return ExecutionContinue(); }},
          // Execution continue
          {"c", [&](const GDBCommand& cmd) { return ExecutionContinue(); }},
          // Execution step
          {"s", [&](const GDBCommand& cmd) { return ExecutionStep(); }},
          // Execution interrupt
          {"\03", [&](const GDBCommand& cmd) { return ExecutionPause(); }},

          // Read memory
          {"m", [&](const GDBCommand& cmd) { return MemoryRead(cmd.data); }},
          // Write memory
          {"M", [&](const GDBCommand& cmd) { return MemoryWrite(cmd.data); }},

          // Read register
          {"p", [&](const GDBCommand& cmd) { return RegisterRead(cmd.data); }},
          // Write register
          {"P", [&](const GDBCommand& cmd) { return RegisterWrite(cmd.data); }},
          // Read all registers
          {"g", [&](const GDBCommand& cmd) { return RegisterReadAll(); }},
          // Write all registers
          {"G",
           [&](const GDBCommand& cmd) { return RegisterWriteAll(cmd.data); }},

          // Query / setting change
          {"Q",
           [&](const GDBCommand& cmd) {
             return QueryPacket(client, cmd.data);
           }},

          // Attach to specific process ID - IDA used to send this, but doesn't
          // after some changes?
          {"vAttach", [&](const GDBCommand& cmd) { return "S05"; }},

          // Get current debugger thread ID
          {"qC",
           [&](const GDBCommand& cmd) {
             std::unique_lock<std::mutex> lock(mtx_);
             auto* thread = cache_.cur_thread_info();
             if (!thread) {
               return std::string(kGdbReplyError);
             }
             return "QC" + std::to_string(thread->thread_id);
           }},
          // Set current debugger thread ID
          {"H",
           [&](const GDBCommand& cmd) {
             std::unique_lock<std::mutex> lock(mtx_);
             int threadId = std::stol(cmd.data.substr(1), 0, 16);

             if (!threadId) {
               // Treat Thread 0 as main thread, seems to work for IDA
               cache_.cur_thread_id = emulator_->main_thread_id();
             } else {
               uint32_t thread_id = -1;

               // Check if the desired thread ID exists
               for (auto& thread : cache_.thread_debug_infos) {
                 if (thread->thread_id == threadId) {
                   thread_id = threadId;
                   break;
                 }
               }

               cache_.cur_thread_id = thread_id;
             }

             return kGdbReplyOK;
           }},

          // Create breakpoint
          {"Z",
           [&](const GDBCommand& cmd) {
             auto& hex_addr = cmd.data.substr(2);
             uint64_t addr = std::stoull(hex_addr.substr(0, hex_addr.find(',')),
                                         nullptr, 16);

             return CreateCodeBreakpoint(addr) ? kGdbReplyOK : kGdbReplyError;
           }},
          // Delete breakpoint
          {"z",
           [&](const GDBCommand& cmd) {
             auto& hex_addr = cmd.data.substr(2);
             uint64_t addr = std::stoull(hex_addr.substr(0, hex_addr.find(',')),
                                         nullptr, 16);
             DeleteCodeBreakpoint(addr);
             return kGdbReplyOK;
           }},

          // Data transfer
          {"qXfer",
           [&](const GDBCommand& cmd) {
             auto param = cmd.data;
             if (param.length() > 0 && param[0] == ':') {
               param = param.substr(1);
             }
             auto sub_cmd = param.substr(0, param.find(':'));
             if (sub_cmd == "features") {
               return std::string(kTargetXml);
             } else if (sub_cmd == "memory-map") {
               return std::string(kMemoryMapXml);
             } else if (sub_cmd == "threads") {
               return BuildThreadList();
             }
             return std::string(kGdbReplyError);
           }},
          // Supported features
          {"qSupported",
           [&](const GDBCommand& cmd) {
             return "PacketSize=1024;qXfer:features:read+;qXfer:threads:read+;"
                    "qXfer:memory-map:read+;QStartNoAckMode+";
           }},
          // Thread list (IDA requests this but ignores in favor of qXfer?)
          {"qfThreadInfo",
           [&](const GDBCommand& cmd) {
             std::unique_lock<std::mutex> lock(mtx_);
             std::string result;
             for (auto& thread : cache_.thread_debug_infos) {
               if (!result.empty()) result += ",";
               result += std::to_string(thread->thread_id);
             }
             return "m" + result;
           }},
      };

  auto it = command_map.find(command.cmd);
  if (it != command_map.end()) {
    return it->second(command);
  }

  return "";
}

}  // namespace gdb
}  // namespace debug
}  // namespace xe
