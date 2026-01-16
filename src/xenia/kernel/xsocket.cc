/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsocket.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"

namespace xe {
namespace kernel {

// Shared io_context for all sockets
static asio::io_context& GetIoContext() {
  static asio::io_context io_context;
  return io_context;
}

// Translate socket options to native
// Note:
// SO_DONTLINGER = ~SO_LINGER
// SO_EXCLUSIVEADDRUSE = ~SO_REUSEADDR
// TODO: Check SO_DONTLINGER and SO_EXCLUSIVEADDRUSE usage on linux
const std::map<uint32_t, int> supported_socket_options = {
    {0x0004, SO_REUSEADDR}, {0x0020, SO_BROADCAST}, {0x0080, SO_LINGER},
    {0x1001, SO_SNDBUF},    {0x1002, SO_RCVBUF},    {0x1005, SO_SNDTIMEO},
    {0x1006, SO_RCVTIMEO},  {~0x0080u, ~SO_LINGER}, {~0x0004u, ~SO_REUSEADDR}};

// Translate socket TCP options to native
const std::map<uint32_t, int> supported_tcp_options = {{0x0001, TCP_NODELAY}};

// Translate ioctl commands to native
const std::map<uint32_t, uint32_t> supported_controls = {
    {0x8004667E, FIONBIO}, {0x4004667F, FIONREAD}};

// Translate socket levels to native
const std::map<uint32_t, int> supported_levels = {{0xFFFF, SOL_SOCKET},
                                                  {0x6, IPPROTO_TCP}};

// TODO(Gliniak): Provide error mapping table.
// Xbox error codes might not match with what we receive from OS.
// TODO(has207): On Linux, asio returns POSIX errno values which games won't
// understand. Needs POSIX -> WSAError mapping for proper cross-platform
// support.
uint32_t AsioErrorToWSAError(const asio::error_code& ec) {
  if (!ec) return 0;
  return static_cast<uint32_t>(ec.value());
}

XSocket::XSocket(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, asio::ip::tcp::socket socket)
    : XObject(kernel_state, kObjectType), tcp_socket_(std::move(socket)) {
  af_ = AddressFamily::X_AF_INET;
  type_ = Type::X_SOCK_STREAM;
  proto_ = Protocol::X_IPPROTO_TCP;
}

XSocket::~XSocket() { Close(); }

uint64_t XSocket::native_handle() {
  if (tcp_socket_ && tcp_socket_->is_open()) {
    return static_cast<uint64_t>(tcp_socket_->native_handle());
  }
  if (udp_socket_ && udp_socket_->is_open()) {
    return static_cast<uint64_t>(udp_socket_->native_handle());
  }
  if (acceptor_ && acceptor_->is_open()) {
    return static_cast<uint64_t>(acceptor_->native_handle());
  }
  return static_cast<uint64_t>(-1);
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  asio::error_code ec;

  if (type == Type::X_SOCK_STREAM) {
    tcp_socket_.emplace(GetIoContext());
    tcp_socket_->open(asio::ip::tcp::v4(), ec);
  } else if (type == Type::X_SOCK_DGRAM) {
    udp_socket_.emplace(GetIoContext());
    udp_socket_->open(asio::ip::udp::v4(), ec);
  } else {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  asio::error_code ec;

  if (tcp_socket_) {
    if (tcp_socket_->is_open()) {
      tcp_socket_->shutdown(asio::socket_base::shutdown_both, ec);
      tcp_socket_->close(ec);
    }
    tcp_socket_.reset();
  }

  if (udp_socket_) {
    if (udp_socket_->is_open()) {
      udp_socket_->close(ec);
    }
    udp_socket_.reset();
  }

  if (acceptor_) {
    if (acceptor_->is_open()) {
      acceptor_->close(ec);
    }
    acceptor_.reset();
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t* optlen) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Map Xbox socket levels to native
  int native_level = level;
  if (supported_levels.contains(level)) {
    native_level = supported_levels.at(level);
  }

  // Map Xbox socket options to native
  int native_optname = optname;
  if (level == 0xFFFF && supported_socket_options.contains(optname)) {
    native_optname = supported_socket_options.at(optname);
  } else if (level == IPPROTO_TCP && supported_tcp_options.contains(optname)) {
    native_optname = supported_tcp_options.at(optname);
  }

  asio::error_code ec;
  socklen_t native_optlen = static_cast<socklen_t>(*optlen);

  int native_handle_val = static_cast<int>(native_handle());
  if (native_handle_val == -1) {
    return X_STATUS_INVALID_HANDLE;
  }

  int ret = getsockopt(native_handle_val, native_level, native_optname,
                       static_cast<char*>(optval_ptr), &native_optlen);
  if (ret < 0) {
    last_error_ = AsioErrorToWSAError(
        asio::error_code(errno, asio::error::get_system_category()));
    return X_STATUS_UNSUCCESSFUL;
  }

  *optlen = static_cast<uint32_t>(native_optlen);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Map Xbox socket levels to native
  int native_level = level;
  if (supported_levels.contains(level)) {
    native_level = supported_levels.at(level);
  }

  // Map Xbox socket options to native
  int native_optname = optname;
  if (level == 0xFFFF && supported_socket_options.contains(optname)) {
    native_optname = supported_socket_options.at(optname);
  } else if (level == IPPROTO_TCP && supported_tcp_options.contains(optname)) {
    native_optname = supported_tcp_options.at(optname);
  }

  int native_handle_val = static_cast<int>(native_handle());
  if (native_handle_val == -1) {
    return X_STATUS_INVALID_HANDLE;
  }

  int ret = setsockopt(native_handle_val, native_level, native_optname,
                       static_cast<char*>(optval_ptr), optlen);
  if (ret < 0) {
    last_error_ = AsioErrorToWSAError(
        asio::error_code(errno, asio::error::get_system_category()));
    XELOGE("XSocket::SetOption: failed with error {:08X}", last_error_);
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // FIONBIO - set non-blocking mode
  if (cmd == 0x8004667E) {
    uint32_t value = *reinterpret_cast<uint32_t*>(arg_ptr);
    bool non_blocking = (value != 0);

    if (tcp_socket_) {
      tcp_socket_->non_blocking(non_blocking, ec);
    } else if (udp_socket_) {
      udp_socket_->non_blocking(non_blocking, ec);
    }

    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }

  // FIONREAD - get bytes available
  if (cmd == 0x4004667F) {
    size_t available = 0;
    if (tcp_socket_) {
      available = tcp_socket_->available(ec);
    } else if (udp_socket_) {
      available = udp_socket_->available(ec);
    }

    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }

    *reinterpret_cast<uint32_t*>(arg_ptr) = static_cast<uint32_t>(available);
    return X_STATUS_SUCCESS;
  }

  XELOGE("XSocket::IOControl: unsupported command {:08X}", cmd);
  return X_STATUS_INVALID_PARAMETER;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  auto* addr_in = reinterpret_cast<N_XSOCKADDR_IN*>(name);
  asio::ip::address_v4 addr(addr_in->sin_addr);
  uint16_t port = addr_in->sin_port;

  asio::error_code ec;

  if (tcp_socket_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    tcp_socket_->connect(endpoint, ec);
  } else if (udp_socket_) {
    asio::ip::udp::endpoint endpoint(addr, port);
    udp_socket_->connect(endpoint, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  // On Linux and Windows (when running under Wine), ports < 1024 require root
  // privileges. Remap to port + 10000 to avoid privilege issues.
  // Note: N_XSOCKADDR_IN uses xe::be<> which auto-converts to/from host endian.
  const uint16_t original_port = static_cast<uint16_t>(name->sin_port);
  if (original_port < 1024 && original_port != 0) {
    uint16_t new_port = original_port + 10000;
    name->sin_port = new_port;
    XELOGW("XSocket::Bind: port {} requires privileges, remapping to port {}",
           original_port, new_port);
  }

  asio::ip::address_v4 addr(name->sin_addr);
  uint16_t port = name->sin_port;

  asio::error_code ec;

  if (tcp_socket_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    tcp_socket_->bind(endpoint, ec);
  } else if (udp_socket_) {
    asio::ip::udp::endpoint endpoint(addr, port);
    udp_socket_->bind(endpoint, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;

  // Get the actual bound port (important when binding to port 0)
  if (tcp_socket_) {
    bound_port_ = tcp_socket_->local_endpoint(ec).port();
  } else if (udp_socket_) {
    bound_port_ = udp_socket_->local_endpoint(ec).port();
  }

  if (ec) {
    bound_port_ = port;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  if (!tcp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // Create an acceptor and transfer the bound socket's native handle to it
  acceptor_.emplace(GetIoContext());
  acceptor_->assign(asio::ip::tcp::v4(), tcp_socket_->native_handle(), ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    acceptor_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }

  // Release the socket's handle since the acceptor now owns it
  tcp_socket_->release();
  tcp_socket_.reset();

  // Start listening
  acceptor_->listen(backlog, ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  if (!acceptor_) {
    return nullptr;
  }

  asio::error_code ec;

  // Accept a new connection
  asio::ip::tcp::socket new_socket(GetIoContext());
  asio::ip::tcp::endpoint peer_endpoint;
  acceptor_->accept(new_socket, peer_endpoint, ec);

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    if (name) {
      std::memset(name, 0, *name_len);
    }
    *name_len = 0;
    return nullptr;
  }

  // Fill in the client address
  if (name && *name_len >= static_cast<int>(sizeof(sockaddr_in))) {
    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(name);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(peer_endpoint.port());
    addr->sin_addr.s_addr = htonl(peer_endpoint.address().to_v4().to_uint());
    std::memset(reinterpret_cast<char*>(addr) + 8, 0, 8);  // Zero sin_zero
  }
  *name_len = sizeof(sockaddr_in);

  // Create a kernel object to represent the new socket
  auto socket =
      object_ref<XSocket>(new XSocket(kernel_state_, std::move(new_socket)));
  socket->bound_ = true;

  return socket;
}

int XSocket::Shutdown(int how) {
  if (!tcp_socket_ && !udp_socket_) {
    return -1;
  }

  asio::error_code ec;
  asio::socket_base::shutdown_type shutdown_type;

  switch (how) {
    case 0:
      shutdown_type = asio::socket_base::shutdown_receive;
      break;
    case 1:
      shutdown_type = asio::socket_base::shutdown_send;
      break;
    case 2:
    default:
      shutdown_type = asio::socket_base::shutdown_both;
      break;
  }

  if (tcp_socket_) {
    tcp_socket_->shutdown(shutdown_type, ec);
  }
  // UDP sockets don't support shutdown (connectionless protocol)

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return 0;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (!tcp_socket_ && !udp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_received = 0;

  if (udp_socket_) {
    bytes_received =
        udp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
  } else if (tcp_socket_) {
    bytes_received =
        tcp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_received);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                      N_XSOCKADDR_IN* from, uint32_t* from_len) {
  if (!udp_socket_ && !tcp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_received = 0;

  if (udp_socket_) {
    asio::ip::udp::endpoint sender_endpoint;
    bytes_received = udp_socket_->receive_from(asio::buffer(buf, buf_len),
                                               sender_endpoint, 0, ec);

    if (!ec && from) {
      from->sin_family = AF_INET;
      from->sin_addr = sender_endpoint.address().to_v4().to_uint();
      from->sin_port = sender_endpoint.port();
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
    }
    if (from_len) {
      *from_len = sizeof(N_XSOCKADDR_IN);
    }
  } else if (tcp_socket_) {
    bytes_received =
        tcp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_received);
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (!tcp_socket_ && !udp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_sent = 0;

  if (udp_socket_) {
    bytes_sent = udp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
  } else if (tcp_socket_) {
    bytes_sent = tcp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_sent);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    N_XSOCKADDR_IN* to, uint32_t to_len) {
  if (!udp_socket_ && !tcp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_sent = 0;

  if (udp_socket_) {
    if (to) {
      asio::ip::address_v4 addr(to->sin_addr);
      uint16_t port = to->sin_port;
      asio::ip::udp::endpoint endpoint(addr, port);

      bytes_sent =
          udp_socket_->send_to(asio::buffer(buf, buf_len), endpoint, flags, ec);
    } else {
      // Send to connected endpoint
      bytes_sent = udp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
    }
  } else if (tcp_socket_) {
    bytes_sent = tcp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_sent);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = static_cast<uint16_t>(len);
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push(reinterpret_cast<uint8_t*>(pkt));

  // TODO: Limit on number of incoming packets?
  return true;
}

X_STATUS XSocket::GetSockName(uint8_t* buf, int* buf_len) {
  auto handle = native_handle();
  if (handle == static_cast<uint64_t>(-1)) {
    return X_STATUS_INVALID_HANDLE;
  }

  socklen_t len = static_cast<socklen_t>(*buf_len);
  int result = getsockname(static_cast<int>(handle),
                           reinterpret_cast<sockaddr*>(buf), &len);
  if (result == -1) {
    last_error_ = AsioErrorToWSAError(
        asio::error_code(errno, asio::error::get_system_category()));
    return X_STATUS_UNSUCCESSFUL;
  }

  *buf_len = static_cast<int>(len);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetPeerName(uint8_t* buf, int* buf_len) {
  auto handle = native_handle();
  if (handle == static_cast<uint64_t>(-1)) {
    return X_STATUS_INVALID_HANDLE;
  }

  socklen_t len = static_cast<socklen_t>(*buf_len);
  int result = getpeername(static_cast<int>(handle),
                           reinterpret_cast<sockaddr*>(buf), &len);
  if (result == -1) {
    last_error_ = AsioErrorToWSAError(
        asio::error_code(errno, asio::error::get_system_category()));
    return X_STATUS_UNSUCCESSFUL;
  }

  *buf_len = static_cast<int>(len);
  return X_STATUS_SUCCESS;
}

uint32_t XSocket::GetLastWSAError() const { return last_error_; }

}  // namespace kernel
}  // namespace xe
