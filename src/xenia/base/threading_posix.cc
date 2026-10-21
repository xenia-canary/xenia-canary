/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"

#include "xenia/base/assert.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading_timer_queue.h"

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#if XE_PLATFORM_LINUX
#include <linux/futex.h>
#endif
#include <unistd.h>
#include <array>
#include <climits>
#include <cstddef>
#include <ctime>
#include <optional>

#include "logging.h"

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>

#include "xenia/base/main_android.h"
#include "xenia/base/string_util.h"
#endif

#if XE_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define XE_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define XE_HAS_SIGEV_THREAD_ID 0
#endif

namespace xe {
namespace threading {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf,
                                          size_t n);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ =
          reinterpret_cast<decltype(android_pthread_getname_np_)>(
              dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Thread interruption is done using user-defined signals
// This implementation uses the SIGRTMAX - SIGRTMIN to signal to a thread
// gdb tip, for SIG = SIGRTMIN + SignalType : handle SIG nostop
// lldb tip, for SIG = SIGRTMIN + SignalType : process handle SIG -s false
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
  // Interrupts a thread whose termination was requested, so it exits if it
  // was stopped at a safe point. Not using pthread_cancel, as asynchronous
  // cancellation unwinding through a noexcept frame aborts the process.
  kThreadTerminate,
  k_Count
};

int GetSystemSignal(SignalType num) {
  auto result = SIGRTMIN + static_cast<int>(num);
  assert_true(result < SIGRTMAX);
  return result;
}

SignalType GetSystemSignalType(int num) {
  return static_cast<SignalType>(num - SIGRTMIN);
}

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)>
    signal_handler_installed = {};

static void signal_handler(int signal, siginfo_t* info, void* context);

void install_signal_handler(SignalType type) {
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)]
           .compare_exchange_strong(expected, true)) {
    return;  // Already installed
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO;
  // Blocking system calls of a thread being terminated return EINTR, so it can
  // get back to a point where it can exit.
  if (type != SignalType::kThreadTerminate) {
    action.sa_flags |= SA_RESTART;
  }
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    // Failed to install, reset the flag
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

std::atomic<TerminationSafeCodeCheck> termination_safe_code_check = nullptr;

void SetTerminationSafeCodeCheck(TerminationSafeCodeCheck check) {
  termination_safe_code_check = check;
}

class PosixConditionBase;

// Termination of the calling thread requested by another thread, implemented
// with the thread class below. Blocking waits and sleeps are safe points: the
// thread unwinds from there with only call sites on its stack.
static bool IsTerminationRequested();
static void ExitIfTerminationRequested();
static void RegisterTerminableWait(PosixConditionBase* condition);
static void UnregisterTerminableWait();
static void EnterBlockingCall();
static void LeaveBlockingCall();

// Marks a wait or a sleep. A thread suspended inside it is at a safe point:
// once terminated it exits there without returning to its caller (see
// Thread::IsAtTerminationSafePoint).
struct BlockingCallScope {
  BlockingCallScope() { EnterBlockingCall(); }
  ~BlockingCallScope() { LeaveBlockingCall(); }
};

// Lets Terminate wake the calling thread while it waits on the condition.
struct TerminableWaitScope {
  explicit TerminableWaitScope(PosixConditionBase* condition) {
    RegisterTerminableWait(condition);
  }
  ~TerminableWaitScope() { UnregisterTerminableWait(); }
};

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(syscall(SYS_gettid));
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() { __sync_synchronize(); }

static void SleepFor(timespec rqtp) {
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  BlockingCallScope blocking_scope;
  do {
    // Termination interrupts the sleep with EINTR.
    ExitIfTerminationRequested();
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
  // A thread terminated as the sleep ended doesn't return to its caller.
  ExitIfTerminationRequested();
}

void Sleep(std::chrono::microseconds duration) {
  SleepFor(DurationToTimeSpec(duration));
}

void NanoSleep(int64_t duration) {
  // Not Sleep(nanoseconds(duration)): the conversion to microseconds
  // truncates, so anything under 1000 ns became nanosleep(0).
  if (duration <= 0) {
    return;
  }
  SleepFor({static_cast<time_t>(duration / 1000000000LL),
            static_cast<long>(duration % 1000000000LL)});
}

void NanoSleepPrecise(int64_t ns) {
#if XE_PLATFORM_MAC
  // Darwin's nanosleep can oversleep by 100-500us under load. Land precisely
  // on the deadline by using mach_wait_until for the bulk of the wait and
  // busy-waiting the last ~200us.
  if (ns <= 0) {
    return;
  }
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  constexpr uint64_t kSpinTailNs = 200'000;
  const uint64_t deadline =
      mach_absolute_time() +
      static_cast<uint64_t>((static_cast<__uint128_t>(ns) * tb.denom) /
                            tb.numer);
  const uint64_t spin_tail = static_cast<uint64_t>(
      (static_cast<__uint128_t>(kSpinTailNs) * tb.denom) / tb.numer);
  if (deadline > mach_absolute_time() + spin_tail) {
    mach_wait_until(deadline - spin_tail);
  }
  while (mach_absolute_time() < deadline) {
  }
#else
  NanoSleep(ns);
#endif
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  Sleep(duration);
  alertable_state_ = false;
  return SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) { return pthread_key_delete(handle) == 0; }

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(handle, reinterpret_cast<void*>(value)) == 0;
}

// A multi-object wait blocks on this counter instead of polling. Every signal
// bumps it and, on Linux, wakes the waiters with a futex. Neither side takes a
// lock, so signalling stays safe from a user callback running in a signal
// handler. With no multi-object wait in progress, signalling is one load.
std::atomic<uint32_t> multi_wait_epoch{0};
std::atomic<uint32_t> multi_wait_waiters{0};

void NotifyMultiObjectWaiters() {
  if (multi_wait_waiters.load(std::memory_order_seq_cst) == 0) {
    return;
  }
  multi_wait_epoch.fetch_add(1, std::memory_order_seq_cst);
#if XE_PLATFORM_LINUX
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&multi_wait_epoch),
          FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif
}

// Blocks until the epoch differs from `expected` or `timeout` passes.
void WaitForMultiObjectSignal(uint32_t expected,
                              std::chrono::milliseconds timeout) {
#if XE_PLATFORM_LINUX
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  ts.tv_nsec = static_cast<long>((timeout.count() % 1000) * 1000000);
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&multi_wait_epoch),
          FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
#else
  if (multi_wait_epoch.load(std::memory_order_seq_cst) == expected) {
    std::this_thread::sleep_for(
        std::min(timeout, std::chrono::milliseconds(1)));
  }
#endif
}

class PosixConditionBase {
 public:
  PosixConditionBase() {
    // Initialize as robust mutex to handle thread termination gracefully
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);

    // Get the native handle and set it as robust
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    pthread_mutex_destroy(native_mutex);      // Destroy default mutex
    pthread_mutex_init(native_mutex, &attr);  // Reinit as robust
    pthread_mutexattr_destroy(&attr);
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    BlockingCallScope blocking_scope;
    // A thread being terminated exits instead of blocking. If termination is
    // requested while it waits, Terminate wakes it through NotifyTermination.
    ExitIfTerminationRequested();

    WaitResult result = WaitResult::kTimeout;
    {
      // Polls don't block, so they need no waking and skip the registration.
      // Unregistered only once the mutex is released, so Terminate (which
      // takes the registration lock, then the mutex) can't deadlock with it.
      std::optional<TerminableWaitScope> wait_scope;
      if (timeout != std::chrono::milliseconds::zero()) {
        wait_scope.emplace(this);
      }

      auto predicate = [this] {
        return this->signaled() || IsTerminationRequested();
      };

      if (!LockNativeMutex()) {
        return WaitResult::kFailed;
      }
      std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);

      if (!predicate()) {
        if (timeout == std::chrono::milliseconds::max()) {
          cond_.wait(lock, predicate);
        } else {
          cond_.wait_for(lock, timeout, predicate);
        }
      }
      if (signaled()) {
        post_execution();
        result = WaitResult::kSuccess;
      }
    }

    // Even if the wait was satisfied: a thread suspended in it as it was
    // terminated must not run on.
    ExitIfTerminationRequested();
    return result;
  }

  // Wakes the threads waiting on this condition so a thread being terminated
  // can exit. Doesn't block, the mutex may be held by a thread suspended for
  // good. Terminate is then called again to retry.
  void NotifyTermination() {
    if (!TryLockNativeMutex()) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
    cond_.notify_all();
  }

  static std::pair<WaitResult, size_t> WaitMultiple(
      std::vector<PosixConditionBase*>&& handles, bool wait_all,
      std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    // For single handle, just use the normal Wait path
    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    BlockingCallScope blocking_scope;
    auto result = WaitMultipleBlocking(handles, wait_all, timeout);
    // As in Wait, a thread being terminated doesn't return to its caller.
    ExitIfTerminationRequested();
    return result;
  }

 private:
  static std::pair<WaitResult, size_t> WaitMultipleBlocking(
      const std::vector<PosixConditionBase*>& handles, bool wait_all,
      std::chrono::milliseconds timeout) {
    // Check the handles in turn and block on the multi-object epoch between
    // checks.
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    // Registered before the first check, so a later signal bumps the epoch.
    multi_wait_waiters.fetch_add(1, std::memory_order_seq_cst);
    struct MultiWaitRegistration {
      ~MultiWaitRegistration() {
        multi_wait_waiters.fetch_sub(1, std::memory_order_seq_cst);
      }
    } multi_wait_registration;
    while (true) {
      // Terminate bumps the epoch, waking the wait below.
      ExitIfTerminationRequested();

      // A signal while the handles are read changes the epoch, and the wait
      // below then returns at once.
      uint32_t epoch_before_check =
          multi_wait_epoch.load(std::memory_order_seq_cst);

      // Check all handles to see if any/all are signaled
      // Use try_lock to avoid deadlocks from lock ordering issues
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;

      // Try to acquire all locks without blocking
      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());
      bool all_locked = true;

      for (size_t i = 0; i < handles.size(); ++i) {
        if (!handles[i]->TryLockNativeMutex()) {
          // Couldn't acquire lock
          all_locked = false;
          break;
        }
        locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
      }

      // If we couldn't acquire all locks, release what we have and retry
      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }

      // Now we have all locks, check the condition
      if (wait_all) {
        // For wait_all, check if ALL are signaled
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        // For wait_any, check if ANY is signaled
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        // Execute post_execution for the signaled handle(s)
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      // Release locks before sleeping
      locks.clear();

      // Check timeout
      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
      WaitForMultiObjectSignal(
          epoch_before_check,
          std::min(remaining, std::chrono::milliseconds(50)));
    }
  }

 public:
  [[nodiscard]] virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  [[nodiscard]] inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;

  // Locks mutex_, recovering it if its owner died.
  bool LockNativeMutex() {
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    return RecoverNativeMutexLock(native_mutex,
                                  pthread_mutex_lock(native_mutex));
  }

  // Locks mutex_ if it is free, recovering it if its owner died.
  bool TryLockNativeMutex() {
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    return RecoverNativeMutexLock(native_mutex,
                                  pthread_mutex_trylock(native_mutex));
  }

  static bool RecoverNativeMutexLock(pthread_mutex_t* native_mutex,
                                     int lock_result) {
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return false;
    }
    return true;
  }

  std::condition_variable cond_;
  std::mutex mutex_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  ~PosixCondition() override = default;

  bool Signal() override {
    auto lock = std::unique_lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock(mutex_);
    signal_ = false;
  }

  EventInfo Query() {
    auto lock = std::unique_lock(mutex_);
    // EVENT_TYPE: NotificationEvent = 0, SynchronizationEvent = 1.
    return EventInfo{manual_reset_ ? 0u : 1u, signal_ ? 1u : 0u};
  }

 private:
  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> final : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock(mutex_);
    // Validate that releasing would not exceed the maximum count
    if (count_ + release_count > maximum_count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
    return true;
  }

 private:
  [[nodiscard]] bool signaled() const override { return count_ > 0; }
  void post_execution() override {
    count_--;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> final : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        cond_.notify_all();
        NotifyMultiObjectWaiters();
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] void* native_handle() const override {
    return const_cast<std::mutex&>(mutex_).native_handle();
  }

 private:
  [[nodiscard]] bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> final : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  ~PosixCondition() override { Cancel(); }

  bool Signal() override {
    std::lock_guard lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time,
               std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period,
                    std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ =
        QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() const {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  [[nodiscard]] void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = static_cast<PosixCondition*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> final : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if XE_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params,
                  ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      return false;
    }
    if (pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    if (params.initial_priority != 0) {
      sched_param sched{};
      sched.sched_priority = params.initial_priority + 1;
      if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
      if (pthread_attr_setschedparam(&attr, &sched) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
    }
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        tid_(static_cast<pid_t>(syscall(SYS_gettid))),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0),
        adopted_(true) {
    sem_init(&suspend_sem_, 0, 0);
#if XE_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  ~PosixCondition() override {
    // FIXME(RodoMa92): This causes random crashes.
    //  The proper way to handle them according to the webs is properly shutdown
    //  instead on relying on killing them using pthread_cancel.
    /*
    if (thread_ && !signaled_) {
#if XE_PLATFORM_ANDROID
      if (pthread_kill(thread_,
                       GetSystemSignal(SignalType::kThreadTerminate)) != 0) {
        assert_always();
      }
#else
      if (pthread_cancel(thread_) != 0) {
        assert_always();
      }
#endif
      if (pthread_join(thread_, nullptr) != 0) {
        assert_always();
      }
    }
    */
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if XE_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(),
                                        result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        std::strcpy(result.data(), android_pre_api_26_name_);
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) const {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
      pthread_setname_np(thread_, std::string(name).c_str());
#if XE_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
#endif
    }
  }

#if XE_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    xe::string_util::copy_truncating(android_pre_api_26_name_, name,
                                     xe::countof(android_pre_api_26_name_));
  }
#endif

  uint32_t system_id() const { return static_cast<uint32_t>(thread_); }

  uint64_t affinity_mask() const {
    WaitStarted();
    cpu_set_t cpu_set;
#if XE_PLATFORM_ANDROID
    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t),
                          &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
    uint64_t result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (auto i = 0u; i < cpu_count; i++) {
      // CPU_ISSET returns int, so `set << i` was an int shift: undefined from
      // bit 31 and wrapping above it.
      if (CPU_ISSET(i, &cpu_set)) {
        result |= uint64_t(1) << i;
      }
    }
    return result;
  }

  void set_affinity_mask(uint64_t mask) const {
    WaitStarted();
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (uint64_t(1) << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if XE_PLATFORM_ANDROID
    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t),
                          &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
  }

  int priority() const {
    WaitStarted();
    if (!fifo_failed_) {
      int policy;
      sched_param param{};
      int ret = pthread_getschedparam(thread_, &policy, &param);
      if (ret != 0) {
        return -1;
      }
      return param.sched_priority;
    }
    // When using nice values, map back to the SCHED_FIFO range (1-32)
    // so callers see a consistent priority space.
    int nice_val = getpriority(PRIO_PROCESS, tid_);
    // nice -19..19 → fifo 32..1
    return 16 - nice_val;
  }

  void set_priority(int new_priority) const {
    WaitStarted();
    if (!fifo_failed_) {
      // Try real-time SCHED_FIFO for best priority control.
      sched_param param{};
      param.sched_priority = new_priority;
      int res = pthread_setschedparam(thread_, SCHED_FIFO, &param);
      if (res == 0) {
        return;
      }
      if (res == EPERM) {
        fifo_failed_ = true;
      } else {
        XELOGW("Unexpected error {} while setting SCHED_FIFO priority", res);
        fifo_failed_ = true;
      }
    }
    // Fall back to nice values under SCHED_OTHER.
    // Map SCHED_FIFO range (1-32) to nice range (19 to -19).
    // Center: fifo 16 → nice 0.
    int nice_val = 16 - new_priority;
    // Clamp to valid nice range.
    if (nice_val < -20) {
      nice_val = -20;
    }
    if (nice_val > 19) {
      nice_val = 19;
    }
    if (tid_ > 0) {
      setpriority(PRIO_PROCESS, tid_, nice_val);
    }
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    std::unique_lock lock(callback_mutex_);
    user_callback_ = std::move(callback);
    sigval value{};
    value.sival_ptr = this;
#if XE_PLATFORM_ANDROID
    sigqueue(pthread_gettid_np(thread_),
             GetSystemSignal(SignalType::kThreadUserCallback), value);
#else
    pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback),
                     value);
#endif
  }

  void CallUserCallback() const {
    std::unique_lock lock(callback_mutex_);
    user_callback_();
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock lock(state_mutex_);
    // Check if thread has any suspend count (Windows allows resume even if
    // running)
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    // If suspend count reaches 0, transition to running and wake the thread
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Post to the semaphore to wake the thread from WaitSuspended.
      // sem_post is async-signal-safe, so this is safe even if called
      // from unusual contexts.
      sem_post(&suspend_sem_);
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();

    // Check if we're trying to suspend ourselves
    bool is_current_thread = pthread_self() == thread_;
    bool already_suspended = false;

    {
      std::unique_lock lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      already_suspended = (state_ == State::kSuspended);
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    // If already suspended, just increment the count — don't send another
    // signal. A second pthread_kill while the thread is in sem_wait would
    // nest signal handlers and create multiple outstanding sem_waits, but
    // Resume only posts once when count reaches 0.
    if (already_suspended) {
      return true;
    }

    if (is_current_thread) {
      // Self-suspension: Instead of sending a signal, directly park. This
      // avoids the signal handler complexity for the self-suspend case. The
      // caller may hold host locks, so it doesn't exit from here.
      ParkSuspended(false);
      return true;
    }

    int result =
        pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
  }

  void Terminate(int exit_code) {
    if (pthread_self() == thread_) {
      pending_exit_code_ = exit_code;
      ExitCurrentThread();
    }

    WaitStarted();
    // Held until the signal is sent: the thread marks itself finished under it
    // as it exits, so it can't have exited (and been joined) meanwhile.
    std::unique_lock lock(state_mutex_);
    if (state_ == State::kFinished) {
      return;
    }

    // Killing the thread wherever it is may abort the process (an unwind
    // through a noexcept frame) or leave locks held. Ask it to exit instead,
    // and wake it from waits, and from a suspension at a safe point (see
    // ParkSuspended), so it exits there. A running thread exits at its next
    // safe point.
    if (!terminate_requested_) {
      pending_exit_code_ = exit_code;
      terminate_requested_ = true;
    }
    sem_post(&suspend_sem_);
    {
      std::lock_guard wait_lock(wait_mutex_);
      if (waiting_on_) {
        waiting_on_->NotifyTermination();
      }
    }
    NotifyMultiObjectWaiters();
    pthread_kill(thread_, GetSystemSignal(SignalType::kThreadTerminate));
  }

  // False once the thread unwinds, so a wait or a sleep in a destructor on the
  // way doesn't start another unwind (which aborts the process).
  bool termination_requested() const {
    return terminate_requested_ && !exiting_;
  }

  // Unwinds the calling thread, which must be this thread. ThreadStartRoutine
  // signals the handle on the way out.
  [[noreturn]] void ExitCurrentThread() {
    exiting_ = true;
    if (adopted_) {
      // Not started by ThreadStartRoutine (the main thread).
      OnExit();
    }
    pthread_exit(reinterpret_cast<void*>(
        static_cast<intptr_t>(pending_exit_code_.load())));
  }

  void RegisterWait(PosixConditionBase* condition) {
    std::lock_guard lock(wait_mutex_);
    waiting_on_ = condition;
  }

  void UnregisterWait() {
    std::lock_guard lock(wait_mutex_);
    waiting_on_ = nullptr;
  }

  // Called on the thread itself as it exits, by returning or unwinding.
  void OnExit() {
    {
      std::unique_lock lock(state_mutex_);
      state_ = State::kFinished;
    }
    if (!LockNativeMutex()) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
    exit_code_ = pending_exit_code_;
    signaled_ = true;
    cond_.notify_all();
    NotifyMultiObjectWaiters();
  }

  void WaitStarted() const {
    std::unique_lock lock(state_mutex_);
    state_signal_.wait(lock,
                       [this] { return state_ != State::kUninitialized; });
  }

  // Only changed by the thread itself, and read by it from the suspend signal
  // handler.
  void EnterBlockingCall() {
    blocking_depth_.store(blocking_depth_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  void LeaveBlockingCall() {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    blocking_depth_.store(blocking_depth_.load(std::memory_order_relaxed) - 1,
                          std::memory_order_relaxed);
  }

  // Whether the thread, interrupted at host_pc, can exit there once
  // terminated: it is in a wait or a sleep (which exits once the signal
  // handler returns), in code accepted by the TerminationSafeCodeCheck, or
  // already unwinding. Called on the thread itself, from a signal handler.
  bool IsSafeToExitAt(uintptr_t host_pc) const {
    if (exiting_ || blocking_depth_.load(std::memory_order_relaxed) != 0) {
      return true;
    }
    TerminationSafeCodeCheck check = termination_safe_code_check;
    return check && host_pc && check(host_pc);
  }

  // Suspends the calling thread. Whether it can exit where it is suspended is
  // published for IsAtTerminationSafePoint, and if it can, termination ends
  // the suspension. Otherwise it stays suspended until resumed: running on
  // after the title is gone is worse than never exiting.
  void ParkSuspended(bool can_exit) {
    parked_can_exit_.store(can_exit, std::memory_order_relaxed);
    parked_.store(true, std::memory_order_release);
    WaitSuspended(can_exit);
    parked_.store(false, std::memory_order_release);
  }

  bool IsAtTerminationSafePoint() {
    // The suspend signal is delivered asynchronously, give the thread a moment
    // to stop in its handler.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    while (!parked_.load(std::memory_order_acquire)) {
      {
        std::unique_lock lock(state_mutex_);
        if (state_ == State::kFinished) {
          return true;
        }
        if (state_ != State::kSuspended) {
          return false;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      sched_yield();
    }
    return parked_can_exit_.load(std::memory_order_relaxed);
  }

  /// Set state to suspended and wait until it is reset by another thread.
  /// Uses sem_wait which is async-signal-safe, allowing this to be called
  /// from a signal handler (e.g., the SIGRTMIN suspend signal handler)
  /// without risking deadlock or heap corruption from non-reentrant
  /// mutex/condvar operations.
  void WaitSuspended(bool exit_on_termination) {
    // The suspend signal does not queue, and a Resume() whose sem_post lands
    // before the signal is delivered leaves a token behind. Re-check the
    // count rather than trusting one token, or that stale post releases the
    // next suspend immediately.
    // A thread being terminated where it can exit resumes to do so.
    while (suspend_count_.load(std::memory_order_acquire) > 0 &&
           !(exit_on_termination &&
             terminate_requested_.load(std::memory_order_acquire))) {
      int ret;
      do {
        ret = sem_wait(&suspend_sem_);
      } while (ret == -1 && errno == EINTR);
      if (ret == -1) {
        // The semaphore is gone (the thread has been joined). Do not spin.
        break;
      }
    }
  }

  void* native_handle() const override {
    return reinterpret_cast<void*>(thread_);
  }

 private:
  static void* ThreadStartRoutine(void* parameter);
  bool signaled() const override { return signaled_; }
  void post_execution() override {
    // Every wait that a thread handle satisfies used to land here, so a
    // handle waited on twice joined the same thread twice, which is
    // undefined once the id can be reused, and tore down the suspend
    // semaphore each time rather than once after the join.
    bool expected = false;
    if (thread_ && joined_.compare_exchange_strong(expected, true)) {
      pthread_join(thread_, nullptr);
      sem_destroy(&suspend_sem_);
    }
  }
  pthread_t thread_;
  pid_t tid_ = 0;                     // Kernel TID for setpriority() fallback
  mutable bool fifo_failed_ = false;  // True after SCHED_FIFO was rejected
  bool signaled_;
  int exit_code_;
  State state_;  // Protected by state_mutex_
  // Written under state_mutex_, but read from the suspend signal handler.
  std::atomic<uint32_t> suspend_count_;
  std::atomic<bool> joined_{false};  // Prevents a second pthread_join
  sem_t suspend_sem_;  // Async-signal-safe suspend/resume semaphore
  // Set by Terminate from another thread, read by the thread at safe points
  // and from signal handlers.
  std::atomic<bool> terminate_requested_{false};
  std::atomic<int> pending_exit_code_{0};
  // Condition the thread is blocked on, so Terminate can wake it.
  std::mutex wait_mutex_;
  PosixConditionBase* waiting_on_ = nullptr;
  // Set once the thread unwinds to exit.
  std::atomic<bool> exiting_{false};
  // Waits and sleeps the thread is in, see BlockingCallScope.
  std::atomic<uint32_t> blocking_depth_{0};
  // Whether the thread is stopped in ParkSuspended, and can exit there.
  std::atomic<bool> parked_{false};
  std::atomic<bool> parked_can_exit_{false};
  // Wraps a thread not started by ThreadStartRoutine.
  bool adopted_ = false;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::function<void()> user_callback_;
#if XE_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

// Out-of-line destructor to ensure proper RTTI/vtable emission
PosixWaitHandle::~PosixWaitHandle() = default;

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  [[nodiscard]] void* native_handle() const override {
    return handle_.native_handle();
  }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner)
    : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset)
    : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset,
                                                  bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread)
    : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  auto result = posix_wait_handle->condition().Wait(timeout);
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal,
                         WaitHandle* wait_handle_to_wait_on, bool is_alertable,
                         std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal =
      dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on =
      dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr ||
      posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  if (posix_wait_handle_to_signal->condition().Signal()) {
    result = posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[],
                                           size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  auto result = PosixConditionBase::WaitMultiple(std::move(conditions),
                                                 wait_all, timeout);
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

class PosixEvent final : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  EventInfo Query() override { return handle_.Query(); }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore final : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count),
                           out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count,
                                             int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count ||
      maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant final : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner)
      : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer final : public PosixConditionHandle<Timer> {
  using WClock_ = WClock_;
  using GClock_ = GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(xe::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(date::clock_cast<GClock_>(due_time),
                     std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(
      xe::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(date::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread final : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params,
                  std::function<void()> start_routine) {
    auto start_data =
        new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override {
    handle_.set_affinity_mask(mask);
  }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override {
    handle_.set_priority(new_priority);
  }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  bool IsAtTerminationSafePoint() override {
    return handle_.IsAtTerminationSafePoint();
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
  threading::set_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  thread->handle_.tid_ = static_cast<pid_t>(syscall(SYS_gettid));

  // Signals the handle once the thread exits, whether the start routine
  // returns or the thread unwinds from Terminate or Exit.
  struct ExitNotifier {
    PosixThread* thread;
    ~ExitNotifier() {
      thread->handle_.OnExit();
      current_thread_ = nullptr;
    }
  } exit_notifier{thread};

  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_ =
        create_suspended ? State::kSuspended : State::kRunning;
    // Set the initial suspend count before Resume can pass WaitStarted.
    if (create_suspended) {
      thread->handle_.suspend_count_ = 1;
    }
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    // Wait on the semaphore Resume posts when the suspend count reaches zero.
    // Nothing has run yet, so it can exit from here.
    thread->handle_.ParkSuspended(true);
  }

  // Terminated before it was resumed.
  ExitIfTerminationRequested();

  start_routine();

  return nullptr;
}

static bool IsTerminationRequested() {
  return current_thread_ &&
         current_thread_->condition().termination_requested();
}

static void ExitIfTerminationRequested() {
  if (IsTerminationRequested()) {
    current_thread_->condition().ExitCurrentThread();
  }
}

static void RegisterTerminableWait(PosixConditionBase* condition) {
  if (current_thread_) {
    current_thread_->condition().RegisterWait(condition);
  }
}

static void UnregisterTerminableWait() {
  if (current_thread_) {
    current_thread_->condition().UnregisterWait();
  }
}

static void EnterBlockingCall() {
  if (current_thread_) {
    current_thread_->condition().EnterBlockingCall();
  }
}

static void LeaveBlockingCall() {
  if (current_thread_) {
    current_thread_->condition().LeaveBlockingCall();
  }
}

static uintptr_t GetInterruptedPc(void* signal_context) {
#if XE_PLATFORM_MAC
  // Not implemented, threads only exit at blocking waits and sleeps.
  return 0;
#elif XE_ARCH_AMD64
  return static_cast<uintptr_t>(
      static_cast<ucontext_t*>(signal_context)->uc_mcontext.gregs[REG_RIP]);
#elif XE_ARCH_ARM64
  return static_cast<uintptr_t>(
      static_cast<ucontext_t*>(signal_context)->uc_mcontext.pc);
#else
  return 0;
#endif
}

// Exits the calling thread if its termination was requested and the signal
// interrupted it where it can be unwound from (JIT code). Otherwise it keeps
// running until a safe point, or until Terminate interrupts it again.
static void ExitIfTerminationSafe(void* signal_context) {
  if (!IsTerminationRequested()) {
    return;
  }
  TerminationSafeCodeCheck check = termination_safe_code_check;
  if (check && check(GetInterruptedPc(signal_context))) {
    current_thread_->condition().ExitCurrentThread();
  }
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
  install_signal_handler(SignalType::kThreadTerminate);
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine))) {
    return nullptr;
  }
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_name(const std::string_view name) {
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#if XE_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
}

static void signal_handler(int signal, siginfo_t* info, void* context) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      if (!current_thread_) {
        // current_thread_ is NULL - this can happen if the signal arrives
        // before the thread has initialized or after it has exited
        return;
      }
      auto& condition = current_thread_->condition();
      // Terminate wakes the thread only if it can exit where it is suspended.
      condition.ParkSuspended(
          condition.IsSafeToExitAt(GetInterruptedPc(context)));
      // In JIT code it exits here, in a wait or a sleep once the handler
      // returns.
      ExitIfTerminationSafe(context);
    } break;
    case SignalType::kThreadUserCallback: {
      assert_not_null(info->si_value.sival_ptr);
      auto p_thread =
          static_cast<PosixCondition<Thread>*>(info->si_value.sival_ptr);
      if (alertable_state_) {
        p_thread->CallUserCallback();
      }
    } break;
    case SignalType::kThreadTerminate: {
      ExitIfTerminationSafe(context);
    } break;
    default:
      assert_always();
  }
}

}  // namespace threading
}  // namespace xe
