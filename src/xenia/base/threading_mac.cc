/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/threading_timer_queue.h"

namespace xe {
namespace threading {

class MacThread;

uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
  uint64_t tid;
  pthread_threadid_np(pthread_self(), &tid);
  return static_cast<uint32_t>(tid);
}

void set_name(const std::string_view name) {
  pthread_setname_np(std::string(name).c_str());
}

void MaybeYield() { pthread_yield_np(); }
void EnableAffinityConfiguration() {}

void Sleep(std::chrono::microseconds duration) {
  uint64_t micros = duration.count();
  timespec rqtp = {static_cast<time_t>(micros / 1000000),
                   static_cast<long>((micros % 1000000) * 1000)};
  nanosleep(&rqtp, nullptr);
}

void NanoSleep(int64_t ns) {
  timespec req = {static_cast<time_t>(ns / 1000000000),
                  static_cast<long>(ns % 1000000000)};
  nanosleep(&req, nullptr);
}

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  Sleep(duration);
  return SleepResult::kSuccess;
}

class MacConditionBase {
 public:
  virtual ~MacConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->signaled(); };
    auto lock = std::unique_lock(mutex_);
    if (predicate()) {
      executed = true;
    } else {
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    }
    return WaitResult::kTimeout;
  }

  static std::pair<WaitResult, size_t> WaitMultiple(
      std::vector<MacConditionBase*>&& handles, bool wait_all,
      std::chrono::milliseconds timeout) {
    if (handles.empty()) {
      return std::make_pair(WaitResult::kFailed, size_t(0));
    }

    std::function<bool()> predicate;
    {
      using iter_t = std::vector<MacConditionBase*>::const_iterator;
      const auto predicate_inner = [](auto h) { return h->signaled(); };
      const auto operation =
          wait_all ? std::all_of<iter_t, decltype(predicate_inner)>
                   : std::any_of<iter_t, decltype(predicate_inner)>;
      predicate = [&handles, operation, predicate_inner] {
        return operation(handles.cbegin(), handles.cend(), predicate_inner);
      };
    }

    std::unique_lock lock(mutex_);

    bool wait_success = true;
    if (timeout == std::chrono::milliseconds::max()) {
      cond_.wait(lock, predicate);
    } else {
      wait_success = cond_.wait_for(lock, timeout, predicate);
    }
    if (wait_success) {
      auto first_signaled = std::numeric_limits<size_t>::max();
      for (auto i = 0u; i < handles.size(); ++i) {
        if (handles[i]->signaled()) {
          if (first_signaled > i) {
            first_signaled = i;
          }
          handles[i]->post_execution();
          if (!wait_all) break;
        }
      }
      assert_true(std::numeric_limits<size_t>::max() != first_signaled);
      return std::make_pair(WaitResult::kSuccess, first_signaled);
    }
    return std::make_pair(WaitResult::kTimeout, size_t(0));
  }

 protected:
  [[nodiscard]] virtual bool signaled() const = 0;
  virtual void post_execution() = 0;
  static std::condition_variable cond_;
  static std::mutex mutex_;
};

std::condition_variable MacConditionBase::cond_;
std::mutex MacConditionBase::mutex_;

template <typename T>
class MacCondition {};

template <>
class MacCondition<Semaphore> final : public MacConditionBase {
 public:
  MacCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    if (maximum_count_ - count_ >= release_count) {
      auto lock = std::unique_lock(mutex_);
      if (out_previous_count) *out_previous_count = count_;
      count_ += release_count;
      cond_.notify_all();
      return true;
    }
    return false;
  }

  [[nodiscard]] void* native_handle() const {
    return const_cast<std::mutex*>(&mutex_);
  }

 private:
  [[nodiscard]] bool signaled() const override { return count_ > 0; }
  void post_execution() override {
    count_--;
    cond_.notify_all();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class MacCondition<Mutant> final : public MacConditionBase {
 public:
  explicit MacCondition(bool initial_owner) : count_(0) {
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
      if (count_ == 0) {
        cond_.notify_all();
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] void* native_handle() const {
    return const_cast<std::mutex*>(&mutex_);
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
class MacCondition<Timer> final : public MacConditionBase {
 public:
  explicit MacCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  ~MacCondition() override { Cancel(); }

  bool Signal() override {
    std::lock_guard lock(mutex_);
    signal_ = true;
    cond_.notify_all();
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

  [[nodiscard]] void* native_handle() const {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = static_cast<MacCondition*>(userdata);
    timer->Signal();
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
  bool signal_;
  const bool manual_reset_;
};

template <>
class MacCondition<Event> : public MacConditionBase {
 public:
  MacCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  ~MacCondition() override = default;

  bool Signal() override {
    auto lock = std::unique_lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock(mutex_);
    signal_ = false;
  }

  [[nodiscard]] bool is_signaled() const { return signal_; }
  [[nodiscard]] void* native_handle() const {
    return const_cast<std::mutex*>(&mutex_);
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

class MacWaitHandle {
 public:
  virtual ~MacWaitHandle() = default;
  virtual MacConditionBase& condition() = 0;
  virtual void* native_handle() const = 0;
};

template <typename T>
class MacConditionHandle : public T, public MacWaitHandle {
 public:
  MacConditionHandle() = default;
  ~MacConditionHandle() override = default;

  explicit MacConditionHandle(bool initial_owner) : handle_(initial_owner) {}
  explicit MacConditionHandle(pthread_t thread) : handle_(thread) {}
  MacConditionHandle(bool manual_reset, bool initial_state)
      : handle_(manual_reset, initial_state) {}
  MacConditionHandle(uint32_t initial_count, uint32_t maximum_count)
      : handle_(initial_count, maximum_count) {}

  MacCondition<T>& condition() override { return handle_; }
  [[nodiscard]] void* native_handle() const override {
    return handle_.native_handle();
  }

 protected:
  MacCondition<T> handle_;
};

class MacSemaphore final : public MacConditionHandle<Semaphore> {
 public:
  MacSemaphore(int initial_count, int maximum_count)
      : MacConditionHandle(static_cast<uint32_t>(initial_count),
                           static_cast<uint32_t>(maximum_count)) {}
  ~MacSemaphore() override = default;

  bool Release(int release_count, int* out_previous_count) override {
    return handle_.Release(static_cast<uint32_t>(release_count),
                           out_previous_count);
  }
};

class MacMutant final : public MacConditionHandle<Mutant> {
 public:
  explicit MacMutant(bool initial_owner) : MacConditionHandle(initial_owner) {}
  ~MacMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

class MacTimer final : public MacConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit MacTimer(bool manual_reset) : MacConditionHandle(manual_reset) {}
  ~MacTimer() override = default;

  bool SetOnceAfter(xe::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(date::clock_cast<GClock_>(due_time),
                     std::move(opt_callback));
  }
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

class MacEvent final : public MacConditionHandle<Event> {
 public:
  MacEvent(bool manual_reset, bool initial_state)
      : MacConditionHandle(manual_reset, initial_state) {}
  ~MacEvent() override = default;

  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override {
    handle_.Signal();
    MaybeYield();
    xe::threading::Sleep(std::chrono::microseconds(10));
    handle_.Reset();
  }
  EventInfo Query() override { return {0, handle_.is_signaled() ? 1u : 0u}; }
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  // MacThread* thread; - Codacy flips out when this is in despite it probably
  // being a good thing to bind to imo
  MacCondition<Thread>* condition;
};

template <>
class MacCondition<Thread> final : public MacConditionBase {
 public:
  MacCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {}
  explicit MacCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {}

  bool Signal() override { return true; }

  bool Initialize(Thread::CreationParameters params,
                  ThreadStartData* start_data) {
    start_data_ = start_data;
    params_ = params;
    start_data->condition = this;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      return false;
    }

    if (params.stack_size > 0) {
      pthread_attr_setstacksize(&attr, params.stack_size);
    }

    int result =
        pthread_create(&thread_, &attr, ThreadStartRoutine, start_data);
    pthread_attr_destroy(&attr);

    if (result != 0) {
      return false;
    }

    state_ = State::kRunning;
    if (!params.create_suspended) {
      started_ = true;
      cond_.notify_all();
    }

    return true;
  }

  uint32_t system_id() const {
    uint64_t thread_id;
    pthread_threadid_np(thread_, &thread_id);
    return static_cast<uint32_t>(thread_id);
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::lock_guard lock(mutex_);
      if (state_ == State::kFinished) {
        return;
      }
      state_ = State::kFinished;
      exit_code_ = exit_code;
      signaled_ = true;
      cond_.notify_all();
    }
    if (is_current_thread) {
      pthread_exit(reinterpret_cast<void*>(exit_code));
    }
    pthread_cancel(thread_);
  }

  void WaitStarted() const {
    std::unique_lock lock(mutex_);
    cond_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  void WaitSuspended() {
    std::unique_lock lock(mutex_);
    cond_.wait(lock, [this] { return suspend_count_ == 0; });
    state_ = State::kRunning;
  }

  void CallUserCallback() {
    std::lock_guard lock(mutex_);
    if (user_callback_) {
      user_callback_();
    }
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::lock_guard lock(mutex_);
    if (state_ != State::kSuspended) return false;
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    suspend_count_--;
    if (suspend_count_ == 0) {
      state_ = State::kRunning;
      cond_.notify_all();
    }
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    {
      std::lock_guard lock(mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      suspend_count_++;
    }
    return true;
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    std::lock_guard lock(mutex_);
    user_callback_ = std::move(callback);
  }

  int priority() const {
    WaitStarted();
    int policy;
    sched_param param{};
    if (pthread_getschedparam(thread_, &policy, &param) != 0) {
      return -1;
    }
    return param.sched_priority;
  }

  void set_priority(int new_priority) const {
    WaitStarted();
    sched_param param{};
    param.sched_priority = new_priority;
    pthread_setschedparam(thread_, SCHED_FIFO, &param);
  }

  uint64_t affinity_mask() const {
    WaitStarted();
    return ~0ULL;
  }

  void set_affinity_mask(uint64_t mask) const { WaitStarted(); }

  void set_name(const std::string& name) const {
    WaitStarted();
    std::lock_guard lock(mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
      if (pthread_equal(pthread_self(), thread_)) {
        pthread_setname_np(name.c_str());
      }
    }
  }

  [[nodiscard]] void* native_handle() const {
    return reinterpret_cast<void*>(thread_);
  }

 private:
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

  static void* ThreadStartRoutine(void* parameter) {
    auto start_data = static_cast<ThreadStartData*>(parameter);
    start_data->condition->WaitStarted();
    start_data->start_routine();
    return nullptr;
  }

  [[nodiscard]] bool signaled() const override { return signaled_; }
  void post_execution() override {
    if (thread_) {
      pthread_join(thread_, nullptr);
    }
  }

  pthread_t thread_;
  // mutable std::mutex mutex_;
  bool started_ = false;
  bool signaled_;
  int exit_code_;
  std::atomic<State> state_;
  std::atomic<uint32_t> suspend_count_;
  std::function<void()> user_callback_;
  ThreadStartData* start_data_ = nullptr;
  Thread::CreationParameters params_;
};

class MacThread final : public MacConditionHandle<Thread> {
 public:
  MacThread() = default;
  explicit MacThread(pthread_t thread) : MacConditionHandle(thread) {}
  ~MacThread() override = default;

  void WaitStarted() const { handle_.WaitStarted(); }

  bool Initialize(Thread::CreationParameters params,
                  std::function<void()> start_routine) {
    auto start_data =
        new ThreadStartData({std::move(start_routine), false, this, nullptr});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name.resize(15);
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

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }
};

thread_local MacThread* current_thread_ = nullptr;

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count,
                                             int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count ||
      maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<MacSemaphore>(initial_count, maximum_count);
}

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<MacMutant>(initial_owner);
}

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<MacTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<MacTimer>(false);
}

std::unique_ptr<Thread> Thread::Create(Thread::CreationParameters params,
                                       std::function<void()> start_routine) {
  auto thread = std::make_unique<MacThread>();
  if (!thread->Initialize(params, std::move(start_routine))) return nullptr;
  assert_not_null(thread);
  return thread;
}

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<MacEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<MacEvent>(false, initial_state);
}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                std::chrono::milliseconds timeout) {
  auto mac_handle = dynamic_cast<MacWaitHandle*>(wait_handle);
  if (!mac_handle) return WaitResult::kFailed;
  return mac_handle->condition().Wait(timeout);
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal,
                         WaitHandle* wait_handle_to_wait_on, bool is_alertable,
                         std::chrono::milliseconds timeout) {
  auto mac_handle_to_signal =
      dynamic_cast<MacWaitHandle*>(wait_handle_to_signal);
  auto mac_handle_to_wait_on =
      dynamic_cast<MacWaitHandle*>(wait_handle_to_wait_on);
  if (!mac_handle_to_signal || !mac_handle_to_wait_on)
    return WaitResult::kFailed;
  mac_handle_to_signal->condition().Signal();
  return mac_handle_to_wait_on->condition().Wait(timeout);
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[],
                                           size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<MacConditionBase*> conditions;
  for (size_t i = 0; i < wait_handle_count; ++i) {
    auto mac_handle = dynamic_cast<MacWaitHandle*>(wait_handles[i]);
    if (!mac_handle) return std::make_pair(WaitResult::kFailed, size_t(0));
    conditions.push_back(&mac_handle->condition());
  }
  return MacConditionBase::WaitMultiple(std::move(conditions), wait_all,
                                        timeout);
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }
  pthread_t handle = pthread_self();
  current_thread_ = new MacThread(handle);
  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  assert_always();
}

}  // namespace threading
}  // namespace xe
