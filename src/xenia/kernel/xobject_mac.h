/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XOBJECT_MAC_H_
#define XENIA_KERNEL_XOBJECT_MAC_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/threading_mac.h"
#include "xenia/kernel/kernel.h"
#include "xenia/memory_mac.h"
#include "xenia/base/logging.h"

namespace xe {
class ByteStream;
class Emulator;
}  // namespace xe

namespace xe {
namespace kernel {

constexpr fourcc_t kXObjSignature = make_fourcc('X', 'E', 'N', '\0');

class KernelState;
class XThread;

template <typename T>
class object_ref;

// https://www.nirsoft.net/kernel_struct/vista/DISPATCHER_HEADER.html
typedef struct {
  struct {
    uint8_t type;

    union {
      uint8_t abandoned;
      uint8_t absolute;
      uint8_t npx_irql;
      uint8_t signalling;
    };
    union {
      uint8_t size;
      uint8_t hand;
    };
    union {
      uint8_t inserted;
      uint8_t debug_active;
      uint8_t dpc_active;
    };
  };

  xe::be<uint32_t> signal_state;
  X_LIST_ENTRY wait_list;
} X_DISPATCH_HEADER;
static_assert_size(X_DISPATCH_HEADER, 0x10);

// https://www.nirsoft.net/kernel_struct/vista/OBJECT_HEADER.html
struct X_OBJECT_HEADER {
  xe::be<uint32_t> pointer_count;
  union {
    xe::be<uint32_t> handle_count;
    xe::be<uint32_t> next_to_free;
  };
  uint8_t name_info_offset;
  uint8_t handle_info_offset;
  uint8_t quota_info_offset;
  uint8_t flags;
  union {
    xe::be<uint32_t> object_create_info;  // X_OBJECT_CREATE_INFORMATION
    xe::be<uint32_t> quota_block_charged;
  };
  xe::be<uint32_t> object_type_ptr;  // -0x8 POBJECT_TYPE
  xe::be<uint32_t> unk_04;           // -0x4

  // Object lives after this header.
  // (There's actually a body field here which is the object itself)
};

// https://www.nirsoft.net/kernel_struct/vista/OBJECT_CREATE_INFORMATION.html
struct X_OBJECT_CREATE_INFORMATION {
  xe::be<uint32_t> attributes;                  // 0x0
  xe::be<uint32_t> root_directory_ptr;          // 0x4
  xe::be<uint32_t> parse_context_ptr;           // 0x8
  xe::be<uint32_t> probe_mode;                  // 0xC
  xe::be<uint32_t> paged_pool_charge;           // 0x10
  xe::be<uint32_t> non_paged_pool_charge;       // 0x14
  xe::be<uint32_t> security_descriptor_charge;  // 0x18
  xe::be<uint32_t> security_descriptor;         // 0x1C
  xe::be<uint32_t> security_qos_ptr;            // 0x20

  // Security QoS here (SECURITY_QUALITY_OF_SERVICE) too!
};

// Comprehensive Error Handling System for XObject operations
class XObjectErrorHandler {
public:
  enum class ErrorType {
    MEMORY_TRANSLATION_FAILURE,
    INVALID_POINTER_ACCESS,
    THREAD_SYNC_FAILURE,
    MEMORY_CORRUPTION,
    TIMEOUT_EXPIRED,
    ASSERTION_FAILURE,
    EXCEPTION_CAUGHT
  };
  
  struct ErrorReport {
    ErrorType type;
    std::string message;
    std::string function;
    int line;
    uint32_t guest_address;
    uintptr_t host_address;
    std::thread::id thread_id;
    std::chrono::system_clock::time_point timestamp;
    
    ErrorReport() : type(ErrorType::EXCEPTION_CAUGHT), line(0), guest_address(0), 
                   host_address(0), thread_id(), timestamp(std::chrono::system_clock::now()) {}
  };
  
  static void ReportError(const ErrorReport& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    
    error_history_.push_back(error);
    
    // Log the error
    XELOGE("[DEBUG] XObject Error Report:");
    XELOGE("[DEBUG] Type: {}", static_cast<int>(error.type));
    XELOGE("[DEBUG] Message: {}", error.message);
    XELOGE("[DEBUG] Function: {} (line {})", error.function, error.line);
    if (error.guest_address != 0) {
      XELOGE("[DEBUG] Guest Address: {:08X}", error.guest_address);
    }
    if (error.host_address != 0) {
      XELOGE("[DEBUG] Host Address: {:08X}", error.host_address);
    }
    XELOGE("[DEBUG] Thread ID: {}", error.thread_id);
    
    // Call custom error callback if set
    if (error_callback_) {
      error_callback_(error);
    }
    
    // Keep error history manageable
    if (error_history_.size() > 1000) {
      error_history_.erase(error_history_.begin(), error_history_.begin() + 500);
    }
  }
  
  static void LogMemoryAccess(uint32_t guest_addr, uintptr_t host_addr, bool success) {
    if (!success) {
      ErrorReport error;
      error.type = ErrorType::INVALID_POINTER_ACCESS;
      error.message = "Memory access failed";
      error.guest_address = guest_addr;
      error.host_address = host_addr;
      error.function = "MemoryAccess";
      error.line = 0;
      error.thread_id = std::this_thread::get_id();
      
      ReportError(error);
    }
  }
  
  static void SetErrorCallback(std::function<void(const ErrorReport&)> callback) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_callback_ = callback;
  }
  
  static std::vector<ErrorReport> GetErrorHistory() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_history_;
  }
  
  static void ClearErrorHistory() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_history_.clear();
  }
  
private:
  static std::mutex error_mutex_;
  static std::vector<ErrorReport> error_history_;
  static std::function<void(const ErrorReport&)> error_callback_;
};

// Enhanced logging macros for XObject operations
#define XELOG_MEMORY_ERROR(msg, guest_addr, host_addr) \
  XObjectErrorHandler::LogMemoryAccess(guest_addr, host_addr, false); \
  XELOGE("[DEBUG] MEMORY ERROR: " msg " (guest: {:08X}, host: {:08X})", guest_addr, host_addr)

#define XELOG_MEMORY_SUCCESS(msg, guest_addr, host_addr) \
  XObjectErrorHandler::LogMemoryAccess(guest_addr, host_addr, true); \
  XELOGI("[DEBUG] " msg " (guest: {:08X}, host: {:08X})", guest_addr, host_addr)

#define XELOG_XOBJECT_ERROR(type, msg, guest_addr, host_addr) \
  do { \
    XObjectErrorHandler::ErrorReport error; \
    error.type = XObjectErrorHandler::ErrorType::type; \
    error.message = msg; \
    error.function = __FUNCTION__; \
    error.line = __LINE__; \
    error.guest_address = guest_addr; \
    error.host_address = host_addr; \
    error.thread_id = std::this_thread::get_id(); \
    XObjectErrorHandler::ReportError(error); \
  } while(0)

// Memory Corruption Detection System
class MemoryCorruptionDetector {
public:
  struct MemoryGuard {
    uint32_t guest_address;
    size_t size;
    uint32_t checksum;
    std::chrono::system_clock::time_point created_at;
    std::thread::id creator_thread;
    
    MemoryGuard() : guest_address(0), size(0), checksum(0), 
                   created_at(std::chrono::system_clock::now()), creator_thread() {}
  };
  
  static void GuardMemory(uint32_t guest_addr, size_t size, void* data) {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    
    MemoryGuard guard;
    guard.guest_address = guest_addr;
    guard.size = size;
    guard.checksum = CalculateChecksum(data, size);
    guard.created_at = std::chrono::system_clock::now();
    guard.creator_thread = std::this_thread::get_id();
    
    memory_guards_[guest_addr] = guard;
    
    XELOGI("[DEBUG] Memory guard created: {:08X} ({} bytes, checksum: {:08X})",
             guest_addr, size, guard.checksum);
  }
  
  static bool ValidateMemory(uint32_t guest_addr, size_t size, void* data) {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    
    auto it = memory_guards_.find(guest_addr);
    if (it == memory_guards_.end()) {
      return true; // No guard = no validation needed
    }
    
    const auto& guard = it->second;
    if (guard.size != size) {
      XELOGE("[DEBUG] Memory size mismatch for {:08X}: expected {}, got {}",
               guest_addr, guard.size, size);
      return false;
    }
    
    uint32_t current_checksum = CalculateChecksum(data, size);
    if (current_checksum != guard.checksum) {
      XELOGE("[DEBUG] Memory corruption detected at {:08X}: expected {:08X}, got {:08X}",
               guest_addr, guard.checksum, current_checksum);
      
      // Report to error handler
      {
        XObjectErrorHandler::ErrorReport error;
        error.type = XObjectErrorHandler::ErrorType::MEMORY_CORRUPTION;
        error.message = "Memory corruption detected";
        error.function = __FUNCTION__;
        error.line = __LINE__;
        error.guest_address = guest_addr;
        error.host_address = reinterpret_cast<uintptr_t>(data);
        error.thread_id = std::this_thread::get_id();
        XObjectErrorHandler::ReportError(error);
      }
      return false;
    }
    
    return true;
  }
  
  static void RemoveGuard(uint32_t guest_addr) {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    
    auto it = memory_guards_.find(guest_addr);
    if (it != memory_guards_.end()) {
      XELOGI("[DEBUG] Memory guard removed: {:08X}", guest_addr);
      memory_guards_.erase(it);
    }
  }
  
  static void UpdateGuard(uint32_t guest_addr, void* new_data) {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    
    auto it = memory_guards_.find(guest_addr);
    if (it != memory_guards_.end()) {
      auto& guard = it->second;
      guard.checksum = CalculateChecksum(new_data, guard.size);
      guard.creator_thread = std::this_thread::get_id();
      XELOGI("[DEBUG] Memory guard updated: {:08X} (new checksum: {:08X})",
               guest_addr, guard.checksum);
    }
  }
  
  static std::vector<MemoryGuard> GetAllGuards() {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    std::vector<MemoryGuard> result;
    result.reserve(memory_guards_.size());
    
    for (const auto& pair : memory_guards_) {
      result.push_back(pair.second);
    }
    
    return result;
  }
  
  static void ClearAllGuards() {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    size_t count = memory_guards_.size();
    memory_guards_.clear();
    XELOGI("[DEBUG] Cleared {} memory guards", count);
  }
  
  static void DumpCorruptedBlocks() {
    std::lock_guard<std::mutex> lock(guard_mutex_);
    
    XELOGI("[DEBUG] Memory Guard Report:");
    XELOGI("[DEBUG] Total guards: {}", memory_guards_.size());
    
    for (const auto& pair : memory_guards_) {
      const auto& guard = pair.second;
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now() - guard.created_at).count();
      
      XELOGI("[DEBUG] Guard {:08X}: {} bytes, age: {}s, thread: {}",
               guard.guest_address, guard.size, age, guard.creator_thread);
    }
  }
  
private:
  static std::mutex guard_mutex_;
  static std::unordered_map<uint32_t, MemoryGuard> memory_guards_;
  
  static uint32_t CalculateChecksum(void* data, size_t size) {
    if (!data || size == 0) return 0;
    
    uint32_t checksum = 0;
    uint8_t* bytes = static_cast<uint8_t*>(data);
    
    // Simple rolling checksum
    for (size_t i = 0; i < size; ++i) {
      checksum = ((checksum << 1) | (checksum >> 31)) ^ bytes[i];
    }
    
    return checksum;
  }
};

// Hang Prevention System with Timeout Mechanisms
class HangPreventionSystem {
public:
  template<typename Func>
  static auto ExecuteWithTimeout(Func&& func, std::chrono::milliseconds timeout) 
      -> std::optional<decltype(func())> {
    
    using ReturnType = decltype(func());
    std::promise<ReturnType> result_promise;
    std::future<ReturnType> result_future = result_promise.get_future();
    
    std::thread worker_thread([&result_promise, &func]() {
      try {
        if constexpr (std::is_void_v<ReturnType>) {
          func();
          result_promise.set_value();
        } else {
          result_promise.set_value(func());
        }
      } catch (...) {
        result_promise.set_exception(std::current_exception());
      }
    });
    
    if (result_future.wait_for(timeout) == std::future_status::timeout) {
      worker_thread.detach();
      
      XELOGE("[DEBUG] HANG PREVENTION: Operation timed out after {} ms",
             timeout.count());
      
      // Report timeout to error handler
      XObjectErrorHandler::ErrorReport error;
      error.type = XObjectErrorHandler::ErrorType::TIMEOUT_EXPIRED;
      error.message = "Operation timed out - potential hang detected";
      error.function = "ExecuteWithTimeout";
      error.line = 0;
      error.thread_id = std::this_thread::get_id();
      XObjectErrorHandler::ReportError(error);
      
      return std::nullopt;
    }
    
    try {
      if constexpr (std::is_void_v<ReturnType>) {
        result_future.get();
        worker_thread.join();
        return true; // void operation succeeded
      } else {
        auto result = result_future.get();
        worker_thread.join();
        return result;
      }
    } catch (...) {
      worker_thread.join();
      throw;
    }
  }
  
  // Specialized timeout for memory operations
  template<typename T>
  static std::optional<T> SafeMemoryOperation(std::function<T()> operation, 
                                               std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
    return ExecuteWithTimeout(operation, timeout);
  }
  
  // Timeout for thread operations
  static bool WaitForThreadReady(std::function<bool()> check_ready, 
                                 std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto start = std::chrono::steady_clock::now();
    
    while (std::chrono::steady_clock::now() - start < timeout) {
      if (check_ready()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    XELOGE("[DEBUG] THREAD TIMEOUT: Thread failed to become ready within {} ms",
           timeout.count());
    return false;
  }
  
  // Timeout for memory translation
  template<typename T>
  static std::optional<T*> SafeMemoryTranslation(std::function<T*()> translate_func,
                                                  std::chrono::milliseconds timeout = std::chrono::milliseconds(50)) {
    return ExecuteWithTimeout(translate_func, timeout);
  }
  
  // Monitor for potential hangs in critical operations
  class HangMonitor {
  public:
    HangMonitor(std::string operation_name, std::chrono::milliseconds timeout = std::chrono::seconds(10))
        : operation_name_(std::move(operation_name)), timeout_(timeout), start_time_(std::chrono::steady_clock::now()) {
      XELOGI("[DEBUG] HANG MONITOR: Starting operation '{}'", operation_name_);
    }
    
    ~HangMonitor() {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_time_);
      
      if (elapsed > timeout_) {
        XELOGE("[DEBUG] HANG DETECTED: Operation '{}' took {} ms (timeout: {} ms)",
                 operation_name_, elapsed.count(), timeout_.count());
        
        // Report hang
        XObjectErrorHandler::ErrorReport error;
        error.type = XObjectErrorHandler::ErrorType::TIMEOUT_EXPIRED;
        error.message = "Potential hang detected in operation: " + operation_name_;
        error.function = operation_name_;
        error.line = 0;
        error.thread_id = std::this_thread::get_id();
        XObjectErrorHandler::ReportError(error);
      } else {
        XELOGI("[DEBUG] HANG MONITOR: Operation '{}' completed in {} ms",
               operation_name_, elapsed.count());
      }
    }
    
  private:
    std::string operation_name_;
    std::chrono::milliseconds timeout_;
    std::chrono::steady_clock::time_point start_time_;
  };
  
  // RAII-style hang monitoring macro
  #define XE_HANG_MONITOR(name, timeout_ms) \
    HangPreventionSystem::HangMonitor _hang_monitor(name, std::chrono::milliseconds(timeout_ms))
  
  #define XE_HANG_MONITOR_DEFAULT(name) \
    HangPreventionSystem::HangMonitor _hang_monitor(name, std::chrono::seconds(10))
  
private:
  // Prevent instantiation
  HangPreventionSystem() = delete;
  ~HangPreventionSystem() = delete;
};

class XObject {
 public:
  // 45410806 needs proper handle value for certain calculations
  // It gets handle value from TLS (without base handle value is 0x88)
  // and substract 0xF8000088. Without base we're receiving wrong address
  // Instead of receiving address that starts with 0x82... we're receiving
  // one with 0x8A... which causes crash
  static constexpr uint32_t kHandleBase = 0xF8000000;
  static constexpr uint32_t kHandleHostBase = 0x01000000;

  enum class Type : uint32_t {
    Undefined,
    Enumerator,
    Event,
    File,
    IOCompletion,
    Module,
    Mutant,
    NotifyListener,
    Semaphore,
    Session,
    Socket,
    SymbolicLink,
    Thread,
    Timer,
    Device
  };

  static bool HasDispatcherHeader(Type type) {
    switch (type) {
      case Type::Event:
      case Type::Mutant:
      case Type::Semaphore:
      case Type::Thread:
      case Type::Timer:
        return true;
      default:
        return false;
    }
    return false;
  }

  static Type MapGuestTypeToHost(uint16_t type) {
    // todo: this is not fully filled in
    switch (type) {
      case 0:
      case 1:
        return Type::Event;
      case 2:
        return Type::Mutant;
      case 5:
        return Type::Semaphore;
      case 6:
        return Type::Thread;
      case 8:
      case 9:
        return Type::Timer;
    }
    return Type::Undefined;
  }
  XObject(Type type);
  XObject(KernelState* kernel_state, Type type, bool host_object = false);
  virtual ~XObject();

  Emulator* emulator() const;
  KernelState* kernel_state() const;
  Memory* memory() const;

  Type type() const;

  // Returns the primary handle of this object.
  X_HANDLE handle() const { return handles_[0]; }

  // Returns all associated handles with this object.
  std::vector<X_HANDLE> handles() const { return handles_; }
  std::vector<X_HANDLE>& handles() { return handles_; }

  const std::string& name() const { return name_; }
  uint32_t guest_object() const { return guest_object_ptr_; }
  
  // Accessor for pointer reference count (for debugging)
  int32_t pointer_ref_count() const { return pointer_ref_count_.load(); }

  // Has this object been created for use by the host?
  // Host objects are persisted through reloads/etc.
  bool is_host_object() const { return host_object_; }
  void set_host_object(bool host_object) { host_object_ = host_object; }

  template <typename T>
  T* guest_object() {
    return memory()->TranslateVirtual<T*>(guest_object_ptr_);
  }

  void RetainHandle();
  bool ReleaseHandle();
  void Retain();
  void Release();
  X_STATUS Delete();

  virtual bool Save(ByteStream* stream) { return false; }
  static object_ref<XObject> Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream);

  static constexpr bool is_handle_host_object(X_HANDLE handle) {
    return handle > XObject::kHandleHostBase && handle < XObject::kHandleBase;
  };
  // Reference()
  // Dereference()

  void SetAttributes(uint32_t obj_attributes_ptr);

  X_STATUS Wait(uint32_t wait_reason, uint32_t processor_mode,
                uint32_t alertable, uint64_t* opt_timeout);
  static X_STATUS SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout);
  static X_STATUS WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout);

  static object_ref<XObject> GetNativeObject(KernelState* kernel_state,
                                             void* native_ptr,
                                             int32_t as_type = -1,
                                             bool already_locked = false);
  template <typename T>
  static object_ref<T> GetNativeObject(KernelState* kernel_state,
                                       void* native_ptr, int32_t as_type = -1,
                                       bool already_locked = false);

 protected:
  bool SaveObject(ByteStream* stream);
  bool RestoreObject(ByteStream* stream);

  // Called on successful wait.
  virtual void WaitCallback() {}
  virtual xe::threading::WaitHandle* GetWaitHandle() { return nullptr; }

  // Creates the kernel object for guest code to use. Typically not needed.
  uint8_t* CreateNative(uint32_t size);
  void SetNativePointer(uint32_t native_ptr, bool uninitialized = false);

  template <typename T>
  T* CreateNative() {
    return reinterpret_cast<T*>(CreateNative(sizeof(T)));
  }

  // Stash native pointer into X_DISPATCH_HEADER
  static void StashHandle(X_DISPATCH_HEADER* header, uint32_t handle) {
    printf("[DEBUG] BREADCRUMB StashHandle: header=%p, handle=%08X\n",
           static_cast<void*>(header), handle);
    printf("[DEBUG] BREADCRUMB StashHandle: About to write to header->wait_list.flink_ptr\n");
    header->wait_list.flink_ptr = kXObjSignature;
    printf("[DEBUG] BREADCRUMB StashHandle: flink_ptr written successfully\n");
    printf("[DEBUG] BREADCRUMB StashHandle: About to write to header->wait_list.blink_ptr\n");
    header->wait_list.blink_ptr = handle;
    printf("[DEBUG] BREADCRUMB StashHandle: blink_ptr written successfully\n");
    printf("[DEBUG] BREADCRUMB StashHandle: COMPLETED\n");
  }

  static uint32_t TimeoutTicksToMs(int64_t timeout_ticks);

  KernelState* kernel_state_;

  // Host objects are persisted through resets/etc.
  bool host_object_ = false;

 private:
  std::atomic<int32_t> pointer_ref_count_;

  Type type_;
  std::vector<X_HANDLE> handles_;
  std::string name_;  // May be zero length.

  // Guest pointer for kernel object. Remember: X_OBJECT_HEADER precedes this
  // if we allocated it!
  uint32_t guest_object_ptr_ = 0;
  bool allocated_guest_object_ = false;
};

template <typename T>
class object_ref {
 public:
  object_ref() noexcept : value_(nullptr) {}
  object_ref(std::nullptr_t) noexcept  // NOLINT(runtime/explicit)
      : value_(nullptr) {}
  object_ref& operator=(std::nullptr_t) noexcept {
    reset();
    return (*this);
  }

  explicit object_ref(T* value) noexcept : value_(value) {
    // Assumes retained on call.
  }
  explicit object_ref(const object_ref& right) noexcept {
    reset(right.get());
    if (value_) value_->Retain();
  }
  template <class V>
    requires std::is_convertible_v<V*, T*>
  object_ref(const object_ref<V>& right) noexcept {
    reset(right.get());
    if (value_) value_->Retain();
  }

  object_ref(object_ref&& right) noexcept : value_(right.release()) {}
  object_ref& operator=(object_ref&& right) noexcept {
    object_ref(std::move(right)).swap(*this);
    return (*this);
  }
  template <typename V>
  object_ref& operator=(object_ref<V>&& right) noexcept {
    object_ref(std::move(right)).swap(*this);
    return (*this);
  }

  object_ref& operator=(const object_ref& right) noexcept {
    object_ref(right).swap(*this);  // NOLINT(runtime/explicit): misrecognized?
    return (*this);
  }
  template <typename V>
  object_ref& operator=(const object_ref<V>& right) noexcept {
    object_ref(right).swap(*this);  // NOLINT(runtime/explicit): misrecognized?
    return (*this);
  }

  void swap(object_ref& right) noexcept { std::swap(value_, right.value_); }

  ~object_ref() noexcept {
    if (value_) {
      value_->Release();
      value_ = nullptr;
    }
  }

  typename std::add_lvalue_reference<T>::type operator*() const {
    return (*get());
  }

  T* operator->() const noexcept {
    return std::pointer_traits<T*>::pointer_to(**this);
  }

  T* get() const noexcept { return value_; }

  template <typename V>
  V* get() const noexcept {
    return reinterpret_cast<V*>(value_);
  }

  explicit operator bool() const noexcept { return value_ != nullptr; }

  T* release() noexcept {
    T* value = value_;
    value_ = nullptr;
    return value;
  }

  void reset() noexcept { object_ref().swap(*this); }

  void reset(T* value) noexcept { object_ref(value).swap(*this); }

  inline bool operator==(const T* right) const noexcept {
    return value_ == right;
  }

 private:
  T* value_ = nullptr;
};

template <class _Ty>
bool operator==(const object_ref<_Ty>& _Left, std::nullptr_t) noexcept {
  return (_Left.get() == reinterpret_cast<_Ty*>(0));
}

template <class _Ty>
bool operator==(std::nullptr_t, const object_ref<_Ty>& _Right) noexcept {
  return (reinterpret_cast<_Ty*>(0) == _Right.get());
}

template <class _Ty>
bool operator!=(const object_ref<_Ty>& _Left, std::nullptr_t _Right) noexcept {
  return (!(_Left == _Right));
}

template <class _Ty>
bool operator!=(std::nullptr_t _Left, const object_ref<_Ty>& _Right) noexcept {
  return (!(_Left == _Right));
}

template <class T, class... Args>
  requires(!std::is_array_v<T>)
object_ref<T> make_object(Args&&... args) {
  return object_ref<T>(new T(std::forward<Args>(args)...));
}

template <typename T>
object_ref<T> retain_object(T* ptr) {
  if (ptr) ptr->Retain();
  return object_ref<T>(ptr);
}

template <typename T>
object_ref<T> XObject::GetNativeObject(KernelState* kernel_state,
                                       void* native_ptr, int32_t as_type,
                                       bool already_locked) {
  return object_ref<T>(reinterpret_cast<T*>(
      GetNativeObject(kernel_state, native_ptr, as_type, already_locked)
          .release()));
}

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XOBJECT_MAC_H_
