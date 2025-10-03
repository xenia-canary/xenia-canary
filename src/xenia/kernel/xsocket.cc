/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "src/xenia/kernel/xsocket.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"

#ifdef XE_PLATFORM_WIN32
// clang-format off
#include "xenia/base/platform_win.h"
#include <WS2tcpip.h>
#include <WinSock2.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xe {
namespace kernel {

XSocket::XSocket(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() { Close(); }

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
#if XE_PLATFORM_WIN32
  int ret = closesocket(native_handle_);
#elif XE_PLATFORM_LINUX
  int ret = close(native_handle_);
#endif

  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t* optlen) {
  int ret =
      getsockopt(native_handle_, level, optname, static_cast<char*>(optval_ptr),
                 reinterpret_cast<socklen_t*>(optlen));
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}
X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  // Translate Xbox socket level to native
  int native_level = level;
  if (level == 0xFFFF) {  // Xbox SOL_SOCKET
    native_level = SOL_SOCKET;
  }

  // Translate Xbox socket options to native
  // Xbox uses Winsock constants which mostly match standard values
  int native_optname = optname;
  if (level == 0xFFFF) {
    switch (optname) {
      case 0x0001:  // SO_DEBUG
        native_optname = SO_DEBUG;
        break;
      case 0x0002:  // SO_ACCEPTCONN
        native_optname = SO_ACCEPTCONN;
        break;
      case 0x0004:  // SO_REUSEADDR
        native_optname = SO_REUSEADDR;
        break;
      case 0x0008:  // SO_KEEPALIVE
        native_optname = SO_KEEPALIVE;
        break;
      case 0x0010:  // SO_DONTROUTE
        native_optname = SO_DONTROUTE;
        break;
      case 0x0020:  // SO_BROADCAST
        native_optname = SO_BROADCAST;
        break;
      case 0x0080:  // SO_LINGER
        native_optname = SO_LINGER;
        break;
      case 0x0100:  // SO_OOBINLINE
        native_optname = SO_OOBINLINE;
        break;
      case 0x1001:  // SO_SNDBUF
        native_optname = SO_SNDBUF;
        break;
      case 0x1002:  // SO_RCVBUF
        native_optname = SO_RCVBUF;
        break;
      case 0x1003:  // SO_SNDLOWAT
        native_optname = SO_SNDLOWAT;
        break;
      case 0x1004:  // SO_RCVLOWAT
        native_optname = SO_RCVLOWAT;
        break;
      case 0x1005:  // SO_SNDTIMEO
        native_optname = SO_SNDTIMEO;
        break;
      case 0x1006:  // SO_RCVTIMEO
        native_optname = SO_RCVTIMEO;
        break;
      case 0x1007:  // SO_ERROR
        native_optname = SO_ERROR;
        break;
      case 0x1008:  // SO_TYPE
        native_optname = SO_TYPE;
        break;
        // Add more translations as needed
    }
  }

  int ret = setsockopt(native_handle_, native_level, native_optname,
                       static_cast<char*>(optval_ptr), optlen);
  if (ret < 0) {
    XELOGE("XSocket::SetOption: setsockopt failed, errno={}", errno);
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
#ifdef XE_PLATFORM_WIN32
  int ret = ioctlsocket(native_handle_, cmd, (u_long*)arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
#elif XE_PLATFORM_LINUX
  // Translate Xbox/Windows ioctl commands to Linux equivalents
  int native_cmd = cmd;
  switch (cmd) {
    case 0x8004667E:  // Windows FIONBIO - set non-blocking mode
      native_cmd = FIONBIO;
      break;
    case 0x4004667F:  // Windows FIONREAD - get bytes available
      native_cmd = FIONREAD;
      break;
    default:
      XELOGW("XSocket::IOControl: unknown cmd={:08X}, passing through", cmd);
      break;
  }

  int ret = ioctl(native_handle_, native_cmd, arg_ptr);
  if (ret < 0) {
    XELOGE("XSocket::IOControl: ioctl failed, cmd={:08X} -> {:08X}, errno={}",
           cmd, native_cmd, errno);
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
#endif
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  // On Linux and Windows (when running under Wine), ports < 1024 require root
  // privileges. Remap to port + 10000 to avoid privilege issues.
  // Note: sin_port is xe::be<uint16_t> which automatically handles endianness,
  // so we use it directly without ntohs/htons.
  const uint16_t original_port = uint16_t(name->sin_port);
  if (original_port < 1024) {
    uint16_t new_port = original_port + 10000;
    name->sin_port = new_port;
    XELOGW("XSocket::Bind: port {} requires privileges, remapping to port {}",
           original_port, new_port);
  }

  int ret = bind(native_handle_, (sockaddr*)name, name_len);

  if (ret < 0) {
#ifdef XE_PLATFORM_WIN32
    XELOGE("XSocket::Bind: bind() failed with WSA error {}", WSAGetLastError());
#else
    XELOGE("XSocket::Bind: bind() failed with errno={}", errno);
#endif
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr;
  socklen_t n_name_len = sizeof(sockaddr);
  uintptr_t ret = accept(native_handle_, &n_sockaddr, &n_name_len);
  if (ret == -1) {
    std::memset(name, 0, *name_len);
    *name_len = 0;
    return nullptr;
  }

  std::memcpy(name, &n_sockaddr, n_name_len);
  *name_len = n_name_len;

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) { return shutdown(native_handle_, how); }

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                      N_XSOCKADDR_IN* from, uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom;
  socklen_t nfromlen = sizeof(sockaddr_in);
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len,
                     flags, (sockaddr*)&nfrom, &nfromlen);
  if (from) {
    from->sin_family = nfrom.sin_family;
    from->sin_addr = ntohl(nfrom.sin_addr.s_addr);  // BE <- BE
    from->sin_port = nfrom.sin_port;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }

  if (from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len,
              flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    N_XSOCKADDR_IN* to, uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  sockaddr_in nto;
  if (to) {
    nto.sin_addr.s_addr = to->sin_addr;
    nto.sin_family = to->sin_family;
    nto.sin_port = to->sin_port;
  }

  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? (sockaddr*)&nto : nullptr, to_len);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

X_STATUS XSocket::GetSockName(uint8_t* buf, int* buf_len) {
  struct sockaddr sa = {};

  int ret = getsockname(native_handle_, &sa, (socklen_t*)buf_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  std::memcpy(buf, &sa, *buf_len);
  return X_STATUS_SUCCESS;
}

uint32_t XSocket::GetLastWSAError() const {
  // Todo(Gliniak): Provide error mapping table
  // Xbox error codes might not match with what we receive from OS
#ifdef XE_PLATFORM_WIN32
  return WSAGetLastError();
#endif
  return errno;
}

}  // namespace kernel
}  // namespace xe
