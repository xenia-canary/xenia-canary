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
#include <thread>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xevent.h"

namespace xe {
namespace kernel {

namespace {

// Drives async_wait completions on a worker thread for the process lifetime.
class IoContextRunner {
 public:
  IoContextRunner() : work_(asio::make_work_guard(io_context_)) {
    thread_ = std::thread([this]() {
      xe::threading::set_name("Xenia Socket I/O");
      io_context_.run();
    });
  }
  ~IoContextRunner() {
    work_.reset();
    io_context_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  asio::io_context& get() { return io_context_; }

 private:
  asio::io_context io_context_;
  asio::executor_work_guard<asio::io_context::executor_type> work_;
  std::thread thread_;
};

}  // namespace

// Shared io_context for all sockets
static asio::io_context& GetIoContext() {
  static IoContextRunner runner;
  return runner.get();
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

// asio error_code -> Winsock WSAE* code. Guests look up by Winsock value;
// returning raw POSIX errno makes recoverable errors look fatal (e.g. COD4
// MP treats unrecognized recvfrom error as a hard init failure).
uint32_t AsioErrorToWSAError(const asio::error_code& ec) {
  if (!ec) {
    return 0;
  }
  if (ec == asio::error::would_block || ec == asio::error::try_again) {
    return 10035;  // WSAEWOULDBLOCK
  }
  if (ec == asio::error::in_progress) {
    return 10036;  // WSAEINPROGRESS
  }
  if (ec == asio::error::already_started) {
    return 10037;  // WSAEALREADY
  }
  if (ec == asio::error::not_socket) {
    return 10038;  // WSAENOTSOCK
  }
  if (ec == asio::error::message_size) {
    return 10040;  // WSAEMSGSIZE
  }
  if (ec == asio::error::no_protocol_option) {
    return 10042;  // WSAENOPROTOOPT
  }
  if (ec == asio::error::address_family_not_supported) {
    return 10047;  // WSAEAFNOSUPPORT
  }
  if (ec == asio::error::address_in_use) {
    return 10048;  // WSAEADDRINUSE
  }
  if (ec == asio::error::network_down) {
    return 10050;  // WSAENETDOWN
  }
  if (ec == asio::error::network_unreachable) {
    return 10051;  // WSAENETUNREACH
  }
  if (ec == asio::error::network_reset) {
    return 10052;  // WSAENETRESET
  }
  if (ec == asio::error::connection_aborted) {
    return 10053;  // WSAECONNABORTED
  }
  if (ec == asio::error::connection_reset) {
    return 10054;  // WSAECONNRESET
  }
  if (ec == asio::error::no_buffer_space) {
    return 10055;  // WSAENOBUFS
  }
  if (ec == asio::error::already_connected) {
    return 10056;  // WSAEISCONN
  }
  if (ec == asio::error::not_connected) {
    return 10057;  // WSAENOTCONN
  }
  if (ec == asio::error::shut_down) {
    return 10058;  // WSAESHUTDOWN
  }
  if (ec == asio::error::timed_out) {
    return 10060;  // WSAETIMEDOUT
  }
  if (ec == asio::error::connection_refused) {
    return 10061;  // WSAECONNREFUSED
  }
  if (ec == asio::error::host_unreachable) {
    return 10065;  // WSAEHOSTUNREACH
  }
  if (ec == asio::error::access_denied) {
    return 10013;  // WSAEACCES
  }
  if (ec == asio::error::fault) {
    return 10014;  // WSAEFAULT
  }
  if (ec == asio::error::invalid_argument) {
    return 10022;  // WSAEINVAL
  }
  if (ec == asio::error::operation_aborted) {
    return 995;  // WSA_OPERATION_ABORTED
  }
  if (ec == asio::error::interrupted) {
    return 10004;  // WSAEINTR
  }
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
    // Use an acceptor for TCP — it supports bind, listen, and accept natively.
    // If Connect() is called later, we transition to a tcp_socket at that
    // point.
    acceptor_.emplace(GetIoContext());
    acceptor_->open(asio::ip::tcp::v4(), ec);
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

  // Allow port reuse so that in-process relaunches can rebind ports
  // immediately without waiting for TIME_WAIT to expire.  Duplicate active
  // binds are caught by the object-table check in Bind() instead.
  int reuse = 1;
  setsockopt(static_cast<int>(native_handle()), SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&reuse), sizeof(reuse));

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
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
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

  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
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
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // FIONBIO - set non-blocking mode
  if (cmd == 0x8004667E) {
    uint32_t value = *reinterpret_cast<uint32_t*>(arg_ptr);
    bool non_blocking = (value != 0);

    if (acceptor_) {
      acceptor_->non_blocking(non_blocking, ec);
    } else if (tcp_socket_) {
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
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  auto* addr_in = reinterpret_cast<N_XSOCKADDR_IN*>(name);
  asio::ip::address_v4 addr(addr_in->sin_addr);
  uint16_t port = addr_in->sin_port;

  asio::error_code ec;

  if (acceptor_) {
    // Transition from acceptor to tcp_socket for client connection.
    // The acceptor was created in Initialize() before we knew whether this
    // socket would listen or connect.
    acceptor_->close(ec);
    acceptor_.reset();

    tcp_socket_.emplace(GetIoContext());
    tcp_socket_->open(asio::ip::tcp::v4(), ec);
    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }

    asio::ip::tcp::endpoint endpoint(addr, port);
    tcp_socket_->connect(endpoint, ec);
  } else if (tcp_socket_) {
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
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
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

  // Reject duplicate active binds (Xbox 360 behaviour).  SO_REUSEADDR is set
  // on the OS socket for TIME_WAIT reclaiming, but games expect a second bind
  // to the same port to fail with WSAEADDRINUSE.  Query the kernel's live
  // object table so the check is naturally correct across in-process relaunches
  // (Shutdown destroys the old object table).
  if (port != 0) {
    auto sockets =
        kernel_state()->object_table()->GetObjectsByType<XSocket>(kObjectType);
    for (auto& s : sockets) {
      if (s.get() != this && s->bound_ && s->bound_port_ == port &&
          s->type_ == type_) {
        last_error_ = 10048;  // WSAEADDRINUSE
        return X_STATUS_UNSUCCESSFUL;
      }
    }
  }

  asio::error_code ec;

  if (acceptor_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    acceptor_->bind(endpoint, ec);
  } else if (tcp_socket_) {
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
  if (acceptor_) {
    bound_port_ = acceptor_->local_endpoint(ec).port();
  } else if (tcp_socket_) {
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
  if (!acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

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

// Winsock FD_* flags split into asio wait_read / wait_write groups.
namespace {
constexpr uint32_t kFD_READ = 1;
constexpr uint32_t kFD_WRITE = 2;
constexpr uint32_t kFD_OOB = 4;
constexpr uint32_t kFD_ACCEPT = 8;
constexpr uint32_t kFD_CONNECT = 16;
constexpr uint32_t kFD_CLOSE = 32;
constexpr uint32_t kReadEvents = kFD_READ | kFD_ACCEPT | kFD_CLOSE | kFD_OOB;
constexpr uint32_t kWriteEvents = kFD_WRITE | kFD_CONNECT;
}  // namespace

int XSocket::WSAEventSelect(object_ref<XEvent> event, uint32_t flags) {
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    last_error_ = uint32_t(X_WSAError::X_WSAENOTSOCK);
    return -1;
  }

  asio::error_code ec;

  // WSAEventSelect implicitly sets the socket to non-blocking.
  if (acceptor_) {
    acceptor_->non_blocking(true, ec);
  } else if (tcp_socket_) {
    tcp_socket_->non_blocking(true, ec);
  } else if (udp_socket_) {
    udp_socket_->non_blocking(true, ec);
  }

  std::lock_guard<std::mutex> lock(select_mutex_);

  // Cancel pending waits from any prior selection.
  if (selected_event_) {
    asio::error_code cancel_ec;
    if (acceptor_) {
      acceptor_->cancel(cancel_ec);
    } else if (tcp_socket_) {
      tcp_socket_->cancel(cancel_ec);
    } else if (udp_socket_) {
      udp_socket_->cancel(cancel_ec);
    }
  }

  selected_event_ = std::move(event);
  selected_event_flags_ = flags;

  if (flags == 0 || !selected_event_) {
    return 0;
  }

  const bool want_read = (flags & kReadEvents) != 0;
  const bool want_write = (flags & kWriteEvents) != 0;

  // Capture strong refs so the socket and event outlive any pending wait.
  auto handler = [self = retain_object(this),
                  ev = selected_event_](const asio::error_code& wait_ec) {
    if (wait_ec) {
      return;  // cancelled or socket closed
    }
    ev->Set(0, false);
  };

  if (want_read) {
    if (acceptor_) {
      acceptor_->async_wait(asio::socket_base::wait_read, handler);
    } else if (tcp_socket_) {
      tcp_socket_->async_wait(asio::socket_base::wait_read, handler);
    } else if (udp_socket_) {
      udp_socket_->async_wait(asio::socket_base::wait_read, handler);
    }
  }
  if (want_write) {
    if (acceptor_) {
      acceptor_->async_wait(asio::socket_base::wait_write, handler);
    } else if (tcp_socket_) {
      tcp_socket_->async_wait(asio::socket_base::wait_write, handler);
    } else if (udp_socket_) {
      udp_socket_->async_wait(asio::socket_base::wait_write, handler);
    }
  }

  return 0;
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
