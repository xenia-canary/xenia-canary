/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * XHttp (NetDll_XHttp*) and XamGetToken (XAuth) implementation backed by     *
 * WinHTTP, with an optional host redirect so a title can talk to a private   *
 * server instead of the original (offline) services.                         *
 ******************************************************************************
 */

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#ifdef XE_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#include <windows.h>  // NOLINT(build/include_order)

#include <winhttp.h>  // NOLINT(build/include_order)
#endif

DEFINE_string(xhttp_redirect_host, "",
              "If set, every XHttp connection goes to this host instead of the "
              "one the title asked for (e.g. 127.0.0.1 or your server's "
              "name). The original host is sent in the Host header.",
              "Live");
DEFINE_int32(xhttp_redirect_port, 0,
             "Port used with xhttp_redirect_host (0 = keep the title's port).",
             "Live");
DEFINE_bool(xhttp_force_http, false,
            "Downgrade titles' HTTPS requests to plain HTTP (use together "
            "with xhttp_redirect_host for a simple private server).",
            "Live");
DEFINE_bool(xhttp_ignore_cert_errors, true,
            "Ignore TLS certificate errors on XHttp requests.", "Live");
DEFINE_bool(xhttp_log_bodies, true,
            "Log XHttp request/response bodies (first 4 KB) at info level.",
            "Live");
DEFINE_string(xauth_token, "xenia-dummy-xauth-token",
              "Token string returned by XamGetToken (XAuthGetToken).", "Live");

namespace xe {
namespace kernel {
namespace xam {

#ifdef XE_PLATFORM_WIN32

namespace {

// XHTTP callback status codes (same values as WinHTTP).
constexpr uint32_t kStatusHeadersAvailable = 0x00020000;
constexpr uint32_t kStatusReadComplete = 0x00080000;
constexpr uint32_t kStatusWriteComplete = 0x00100000;
constexpr uint32_t kStatusRequestError = 0x00200000;
constexpr uint32_t kStatusSendRequestComplete = 0x00400000;

constexpr uint32_t kOpenFlagAsync = 0x10000000;
constexpr uint32_t kRequestFlagSecure = 0x00800000;

constexpr uint32_t kQueryFlagNumber = 0x20000000;
constexpr uint32_t kQueryCustom = 65535;

constexpr uint32_t kErrorInsufficientBuffer = 122;
constexpr uint32_t kErrorHeaderNotFound = 12150;  // ERROR_WINHTTP_HEADER_NOT_FOUND

enum class HType { kSession, kConnect, kRequest };

struct HObject {
  HType type;
  uint32_t id = 0;
  std::shared_ptr<HObject> parent;

  // session
  uint32_t open_flags = 0;
  uint32_t callback = 0;
  uint32_t callback_mask = 0;
  HINTERNET h_session = nullptr;

  // connect
  std::string host;  // host the title asked for
  uint16_t port = 0;
  HINTERNET h_connect = nullptr;

  // request
  std::string verb = "GET";
  std::string path;
  uint32_t req_flags = 0;
  uint32_t context = 0;
  std::string req_headers;
  std::vector<uint8_t> body;
  uint32_t total_len = 0;
  uint32_t scratch = 0;  // guest memory used for callback payloads
  bool done = false;
  bool failed = false;
  uint32_t error = 0;
  uint32_t status_code = 0;
  std::wstring raw_headers;  // CRLF separated
  std::vector<uint8_t> response;
  size_t read_pos = 0;
  bool use_tls = false;

  ~HObject() {
    if (h_connect) WinHttpCloseHandle(h_connect);
    if (h_session) WinHttpCloseHandle(h_session);
  }
};

struct Event {
  std::shared_ptr<HObject> request;
  uint32_t status;
  uint32_t info;
  uint32_t info_len;
};

std::mutex g_mutex;
std::map<uint32_t, std::shared_ptr<HObject>> g_objects;
std::deque<Event> g_events;
std::atomic<uint32_t> g_next_id{0x4100};

std::wstring ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string ToNarrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                      nullptr, nullptr);
  return s;
}

std::shared_ptr<HObject> Lookup(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_objects.find(id);
  return it == g_objects.end() ? nullptr : it->second;
}

uint32_t Register(std::shared_ptr<HObject> obj) {
  obj->id = g_next_id.fetch_add(0x10);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_objects[obj->id] = obj;
  return obj->id;
}

std::shared_ptr<HObject> SessionOf(const std::shared_ptr<HObject>& o) {
  auto p = o;
  while (p && p->type != HType::kSession) p = p->parent;
  return p;
}

void QueueEvent(const std::shared_ptr<HObject>& req, uint32_t status,
                uint32_t info, uint32_t info_len) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_events.push_back({req, status, info, info_len});
}

void LogBody(const char* tag, const uint8_t* data, size_t len) {
  if (!cvars::xhttp_log_bodies || !len) return;
  size_t n = std::min<size_t>(len, 4096);
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t c = data[i];
    s += (c >= 0x20 && c < 0x7F) || c == '\n' || c == '\r' ? (char)c : '.';
  }
  XELOGI("XHttp {} ({} bytes): {}", tag, len, s);
}

// Performs the whole HTTP exchange synchronously (called on a worker thread
// for async sessions). Fills the response fields of |r|.
void Perform(const std::shared_ptr<HObject>& r) {
  auto conn = r->parent;
  auto session = SessionOf(r);
  std::string host = conn->host;
  uint16_t port = conn->port;
  bool tls = (r->req_flags & kRequestFlagSecure) != 0;

  if (!cvars::xhttp_redirect_host.empty()) {
    host = cvars::xhttp_redirect_host;
    if (cvars::xhttp_redirect_port) port = (uint16_t)cvars::xhttp_redirect_port;
  }
  if (cvars::xhttp_force_http) {
    if (tls && !cvars::xhttp_redirect_port && !cvars::xhttp_redirect_host.empty())
      port = 80;
    tls = false;
  }
  if (!port) port = tls ? 443 : 80;

  XELOGI("XHttp {} http{}://{}:{}{}  (title asked for {})", r->verb,
         tls ? "s" : "", host, port, r->path, conn->host);

  HINTERNET hconn =
      WinHttpConnect(session->h_session, ToWide(host).c_str(), port, 0);
  if (!hconn) {
    r->failed = true;
    r->error = GetLastError();
    return;
  }
  HINTERNET hreq = WinHttpOpenRequest(
      hconn, ToWide(r->verb).c_str(), ToWide(r->path).c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      tls ? WINHTTP_FLAG_SECURE : 0);
  if (!hreq) {
    r->failed = true;
    r->error = GetLastError();
    WinHttpCloseHandle(hconn);
    return;
  }
  if (cvars::xhttp_ignore_cert_errors) {
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hreq, WINHTTP_OPTION_SECURITY_FLAGS, &flags,
                     sizeof(flags));
  }

  std::string headers = r->req_headers;
  if (!cvars::xhttp_redirect_host.empty() && !conn->host.empty()) {
    headers += "Host: " + conn->host + "\r\n";
  }
  std::wstring wheaders = ToWide(headers);
  LogBody("request headers", (const uint8_t*)headers.data(), headers.size());
  LogBody("request body", r->body.data(), r->body.size());

  BOOL ok = WinHttpSendRequest(
      hreq, wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(),
      wheaders.empty() ? 0 : (DWORD)-1L,
      r->body.empty() ? WINHTTP_NO_REQUEST_DATA : r->body.data(),
      (DWORD)r->body.size(), (DWORD)r->body.size(), 0);
  if (ok) ok = WinHttpReceiveResponse(hreq, nullptr);
  if (!ok) {
    r->failed = true;
    r->error = GetLastError();
    XELOGE("XHttp request failed, WinHTTP error {}", r->error);
    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(hconn);
    return;
  }

  DWORD status = 0, sz = sizeof(status);
  WinHttpQueryHeaders(hreq,
                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                      WINHTTP_NO_HEADER_INDEX);
  r->status_code = status;

  DWORD hsz = 0;
  WinHttpQueryHeaders(hreq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                      WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &hsz,
                      WINHTTP_NO_HEADER_INDEX);
  if (hsz) {
    std::wstring h(hsz / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(hreq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, h.data(), &hsz,
                            WINHTTP_NO_HEADER_INDEX)) {
      h.resize(hsz / sizeof(wchar_t));
      r->raw_headers = h;
    }
  }

  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(hreq, &avail) || !avail) break;
    size_t old = r->response.size();
    r->response.resize(old + avail);
    DWORD got = 0;
    if (!WinHttpReadData(hreq, r->response.data() + old, avail, &got)) break;
    r->response.resize(old + got);
    if (!got) break;
  }
  XELOGI("XHttp response status {} ({} bytes)", status, r->response.size());
  LogBody("response headers", (const uint8_t*)ToNarrow(r->raw_headers).data(),
          ToNarrow(r->raw_headers).size());
  LogBody("response body", r->response.data(), r->response.size());

  WinHttpCloseHandle(hreq);
  WinHttpCloseHandle(hconn);
}

void FinishRequest(const std::shared_ptr<HObject>& r, bool async) {
  Perform(r);
  r->done = true;
  if (!async) return;
  if (r->failed) {
    // XHTTP_ASYNC_RESULT { DWORD_PTR dwResult; DWORD dwError; }
    auto* mem = kernel_memory()->TranslateVirtual<uint8_t*>(r->scratch);
    xe::store_and_swap<uint32_t>(mem + 0, 0);
    xe::store_and_swap<uint32_t>(mem + 4, r->error);
    QueueEvent(r, kStatusRequestError, r->scratch, 8);
  } else {
    QueueEvent(r, kStatusHeadersAvailable, 0, 0);
  }
}

}  // namespace

dword_result_t NetDll_XHttpStartup_entry(dword_t caller, dword_t reserved,
                                         dword_t reserved_ptr) {
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpStartup, kNetworking, kImplemented);

dword_result_t NetDll_XHttpShutdown_entry(dword_t caller) {
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpShutdown, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpen_entry(dword_t caller, lpstring_t user_agent,
                                      dword_t access_type,
                                      lpstring_t proxy_name,
                                      lpstring_t proxy_bypass, dword_t flags) {
  auto s = std::make_shared<HObject>();
  s->type = HType::kSession;
  s->open_flags = flags;
  std::wstring ua = ToWide(user_agent ? std::string((const char*)user_agent)
                                      : std::string("xenia"));
  s->h_session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s->h_session) {
    XThread::SetLastError(GetLastError());
    return 0;
  }
  uint32_t id = Register(s);
  XELOGI("XHttpOpen -> {:08X} (flags {:08X})", id, (uint32_t)flags);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return id;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpen, kNetworking, kImplemented);

dword_result_t NetDll_XHttpCloseHandle_entry(dword_t caller, dword_t handle) {
  std::shared_ptr<HObject> o;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_objects.find(handle);
    if (it == g_objects.end()) return 0;
    o = it->second;
    g_objects.erase(it);
  }
  if (o->type == HType::kRequest && o->scratch) {
    kernel_memory()->SystemHeapFree(o->scratch);
    o->scratch = 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCloseHandle, kNetworking, kImplemented);

dword_result_t NetDll_XHttpConnect_entry(dword_t caller, dword_t session,
                                         lpstring_t server_name, dword_t port,
                                         dword_t flags) {
  auto s = Lookup(session);
  if (!s || s->type != HType::kSession) {
    XThread::SetLastError(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  auto c = std::make_shared<HObject>();
  c->type = HType::kConnect;
  c->parent = s;
  c->host = server_name ? std::string((const char*)server_name) : "";
  c->port = (uint16_t)port;
  uint32_t id = Register(c);
  XELOGI("XHttpConnect {} port {} -> {:08X}", c->host, (uint32_t)port, id);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return id;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpConnect, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetStatusCallback_entry(dword_t caller,
                                                   dword_t handle,
                                                   dword_t callback,
                                                   dword_t flags,
                                                   dword_t reserved) {
  auto o = Lookup(handle);
  if (!o) return 0;
  uint32_t prev = o->callback;
  o->callback = callback;
  o->callback_mask = flags;
  XELOGI("XHttpSetStatusCallback {:08X} cb={:08X} flags={:08X}",
         (uint32_t)handle, (uint32_t)callback, (uint32_t)flags);
  return prev;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetStatusCallback, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpenRequest_entry(
    dword_t caller, dword_t connect, lpstring_t verb, lpstring_t object,
    lpstring_t version, lpstring_t referrer, lpdword_t accept_types,
    dword_t flags) {
  auto c = Lookup(connect);
  if (!c || c->type != HType::kConnect) {
    XThread::SetLastError(6);
    return 0;
  }
  auto r = std::make_shared<HObject>();
  r->type = HType::kRequest;
  r->parent = c;
  r->verb = verb ? std::string((const char*)verb) : "GET";
  if (r->verb.empty()) r->verb = "GET";
  r->path = object ? std::string((const char*)object) : "/";
  if (r->path.empty()) r->path = "/";
  r->req_flags = flags;
  r->scratch = kernel_memory()->SystemHeapAlloc(0x40);
  uint32_t id = Register(r);
  XELOGI("XHttpOpenRequest {} {} flags={:08X} -> {:08X}", r->verb, r->path,
         (uint32_t)flags, id);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return id;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSendRequest_entry(
    dword_t caller, dword_t request, lpstring_t headers, dword_t headers_len,
    lpvoid_t optional, dword_t optional_len, dword_t total_len,
    dword_t context) {
  auto r = Lookup(request);
  if (!r || r->type != HType::kRequest) {
    XThread::SetLastError(6);
    return 0;
  }
  r->context = context;
  r->req_headers.clear();
  if (headers) {
    const char* h = (const char*)headers;
    size_t n = (headers_len == 0xFFFFFFFFu) ? strlen(h) : (size_t)headers_len;
    r->req_headers.assign(h, n);
    if (!r->req_headers.empty() &&
        r->req_headers.compare(r->req_headers.size() >= 2
                                   ? r->req_headers.size() - 2
                                   : 0,
                               2, "\r\n") != 0) {
      r->req_headers += "\r\n";
    }
  }
  r->body.clear();
  if (optional && optional_len) {
    const uint8_t* p = (const uint8_t*)(void*)optional;
    r->body.assign(p, p + optional_len);
  }
  r->total_len = total_len;
  bool async = SessionOf(r)->open_flags & kOpenFlagAsync;
  XELOGI("XHttpSendRequest {:08X} optional={} total={}", (uint32_t)request,
         (uint32_t)optional_len, (uint32_t)total_len);
  if (async) QueueEvent(r, kStatusSendRequestComplete, 0, 0);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSendRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpWriteData_entry(dword_t caller, dword_t request,
                                           lpvoid_t buffer, dword_t length,
                                           lpdword_t written) {
  auto r = Lookup(request);
  if (!r || r->type != HType::kRequest) {
    XThread::SetLastError(6);
    return 0;
  }
  const uint8_t* p = (const uint8_t*)(void*)buffer;
  r->body.insert(r->body.end(), p, p + length);
  if (written) *written = length;
  bool async = SessionOf(r)->open_flags & kOpenFlagAsync;
  if (async) {
    auto* mem = kernel_memory()->TranslateVirtual<uint8_t*>(r->scratch);
    xe::store_and_swap<uint32_t>(mem + 8, length);
    QueueEvent(r, kStatusWriteComplete, r->scratch + 8, 4);
  }
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpWriteData, kNetworking, kImplemented);

dword_result_t NetDll_XHttpReceiveResponse_entry(dword_t caller,
                                                 dword_t request,
                                                 dword_t reserved) {
  auto r = Lookup(request);
  if (!r || r->type != HType::kRequest) {
    XThread::SetLastError(6);
    return 0;
  }
  bool async = SessionOf(r)->open_flags & kOpenFlagAsync;
  if (async) {
    std::thread([r]() { FinishRequest(r, true); }).detach();
  } else {
    FinishRequest(r, false);
    if (r->failed) {
      XThread::SetLastError(r->error);
      return 0;
    }
  }
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReceiveResponse, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryHeaders_entry(dword_t caller, dword_t request,
                                              dword_t info_level,
                                              lpstring_t name,
                                              lpvoid_t buffer,
                                              lpdword_t buffer_len,
                                              lpdword_t index) {
  auto r = Lookup(request);
  if (!r || r->type != HType::kRequest || !r->done || r->failed) {
    XThread::SetLastError(6);
    return 0;
  }
  uint32_t level = info_level;
  uint32_t base = level & 0xFFFF;

  if (level & kQueryFlagNumber) {
    uint32_t value = 0;
    if (base == 19) {  // STATUS_CODE
      value = r->status_code;
    } else if (base == 5) {  // CONTENT_LENGTH
      value = (uint32_t)r->response.size();
    } else {
      XThread::SetLastError(kErrorHeaderNotFound);
      return 0;
    }
    if (!buffer_len || *buffer_len < 4) {
      if (buffer_len) *buffer_len = 4;
      XThread::SetLastError(kErrorInsufficientBuffer);
      return 0;
    }
    xe::store_and_swap<uint32_t>((uint8_t*)(void*)buffer, value);
    *buffer_len = 4;
    return 1;
  }

  std::string out;
  if (base == 19) {
    out = std::to_string(r->status_code);
  } else if (base == 5) {
    out = std::to_string(r->response.size());
  } else if (base == 22 || base == 21) {  // RAW_HEADERS(_CRLF)
    out = ToNarrow(r->raw_headers);
    // RAW_HEADERS (21) uses NUL separators; keep CRLF form for both.
  } else if (base == kQueryCustom && name) {
    std::string wanted((const char*)name);
    std::string all = ToNarrow(r->raw_headers);
    size_t pos = 0;
    bool found = false;
    while (pos < all.size()) {
      size_t eol = all.find("\r\n", pos);
      if (eol == std::string::npos) eol = all.size();
      std::string line = all.substr(pos, eol - pos);
      size_t colon = line.find(':');
      if (colon != std::string::npos && colon == wanted.size()) {
        bool eq = true;
        for (size_t i = 0; i < colon; ++i) {
          if (tolower((unsigned char)line[i]) !=
              tolower((unsigned char)wanted[i])) {
            eq = false;
            break;
          }
        }
        if (eq) {
          size_t v = colon + 1;
          while (v < line.size() && line[v] == ' ') ++v;
          out = line.substr(v);
          found = true;
          break;
        }
      }
      pos = eol + 2;
    }
    if (!found) {
      XThread::SetLastError(kErrorHeaderNotFound);
      return 0;
    }
  } else {
    XThread::SetLastError(kErrorHeaderNotFound);
    return 0;
  }

  uint32_t need = (uint32_t)out.size() + 1;
  if (!buffer_len || *buffer_len < need) {
    if (buffer_len) *buffer_len = need;
    XThread::SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  std::memcpy((void*)buffer, out.c_str(), need);
  *buffer_len = (uint32_t)out.size();
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryHeaders, kNetworking, kImplemented);

dword_result_t NetDll_XHttpReadData_entry(dword_t caller, dword_t request,
                                          lpvoid_t buffer, dword_t length,
                                          lpdword_t bytes_read) {
  auto r = Lookup(request);
  if (!r || r->type != HType::kRequest || !r->done) {
    XThread::SetLastError(6);
    return 0;
  }
  size_t remaining = r->response.size() - r->read_pos;
  uint32_t n = (uint32_t)std::min<size_t>(remaining, length);
  if (n) std::memcpy((void*)buffer, r->response.data() + r->read_pos, n);
  r->read_pos += n;
  if (bytes_read) *bytes_read = n;
  bool async = SessionOf(r)->open_flags & kOpenFlagAsync;
  if (async) QueueEvent(r, kStatusReadComplete, buffer.guest_address(), n);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReadData, kNetworking, kImplemented);

dword_result_t NetDll_XHttpQueryOption_entry(dword_t caller, dword_t handle,
                                             dword_t option, lpvoid_t buffer,
                                             lpdword_t buffer_len) {
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryOption, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetOption_entry(dword_t caller, dword_t handle,
                                           dword_t option, lpvoid_t buffer,
                                           dword_t buffer_len) {
  XELOGI("XHttpSetOption {:08X} option={} len={}", (uint32_t)handle,
         (uint32_t)option, (uint32_t)buffer_len);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetOption, kNetworking, kImplemented);

dword_result_t NetDll_XHttpSetCredentials_entry(dword_t caller,
                                                dword_t request) {
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetCredentials, kNetworking, kImplemented);

// Delivers pending callbacks on the calling guest thread.
dword_result_t NetDll_XHttpDoWork_entry(dword_t caller, dword_t session,
                                        dword_t flags,
                                        const ppc_context_t& ctx) {
  for (int guard = 0; guard < 64; ++guard) {
    Event ev;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_events.empty()) break;
      ev = g_events.front();
      g_events.pop_front();
    }
    // Callback is inherited from the session unless set on the request.
    uint32_t cb = 0;
    for (auto p = ev.request; p && !cb; p = p->parent) cb = p->callback;
    if (!cb) continue;
    uint64_t args[] = {ev.request->id, ev.request->context, ev.status,
                       ev.info, ev.info_len};
    XELOGI("XHttp callback {:08X}(h={:08X}, ctx={:08X}, status={:08X}, "
           "info={:08X}, len={})",
           cb, ev.request->id, ev.request->context, ev.status, ev.info,
           ev.info_len);
    kernel_state()->processor()->Execute(ctx->thread_state, cb, args,
                                         xe::countof(args));
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpDoWork, kNetworking, kImplemented);

// ---------------------------------------------------------------------------
// XAuth: XamGetToken / XamFreeToken. Returns a dummy token synchronously.
// Layout of the guest token (best guess, adjust from logs):
//   +0 u32 length, +4 u32 pointer to data, +8.. NUL-terminated data
// ---------------------------------------------------------------------------
dword_result_t XamGetToken_entry(dword_t user_index, lpstring_t uri,
                                 dword_t uri_len, lpdword_t out_token,
                                 lpvoid_t overlapped) {
  XELOGI("XamGetToken(user={}, uri='{}', len={}, out={:08X}, ov={:08X})",
         (uint32_t)user_index, uri ? (const char*)uri : "(null)",
         (uint32_t)uri_len, out_token.guest_address(),
         overlapped.guest_address());
  std::string tok = cvars::xauth_token;
  uint32_t size = 8 + (uint32_t)tok.size() + 1;
  uint32_t addr = kernel_memory()->SystemHeapAlloc(size);
  auto* mem = kernel_memory()->TranslateVirtual<uint8_t*>(addr);
  xe::store_and_swap<uint32_t>(mem + 0, (uint32_t)tok.size());
  xe::store_and_swap<uint32_t>(mem + 4, addr + 8);
  std::memcpy(mem + 8, tok.c_str(), tok.size() + 1);
  if (out_token) *out_token = addr;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetToken, kNetworking, kImplemented);

dword_result_t XamFreeToken_entry(dword_t token) {
  if (token) kernel_memory()->SystemHeapFree(token);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamFreeToken, kNetworking, kImplemented);

#endif  // XE_PLATFORM_WIN32

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(XHttp);
