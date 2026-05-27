/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_mac.h"

#include "xenia/base/assert.h"
#include "xenia/base/threading_mac.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <cstring>
#include <sstream>
#include <thread>

namespace xe {
namespace ui {

namespace {
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int LongWaitWarningThresholdMsForTag(const char* tag) {
  // RunTitle can legitimately take longer while opening the package and
  // creating the initial emulator state, so avoid early false-positive stalls.
  if (tag && std::strcmp(tag, "RunTitle") == 0) {
    return 5000;
  }
  if (tag &&
      std::strcmp(tag, "SetupGraphicsSystemPresenterPainting") == 0) {
    return 8000;
  }
  return 2000;
}
}  // namespace

WindowedAppContext::~WindowedAppContext() {
  // The UI thread is responsible for managing the lifetime of the context.
  assert_true(IsInUIThread());

  // It's okay to destroy the context from a platform's internal UI loop
  // callback, primarily on platforms where the loop is run by the OS itself,
  // and the context can't be created and destroyed in a RAII way, rather, it's
  // created in an initialization handler and destroyed in a shutdown handler
  // called by the OS. However, destruction must not be done from within the
  // queued functions - as in this case, the pending function container, the
  // mutex, will be accessed after having been destroyed already.

  // Make sure CallInUIThreadDeferred doesn't call
  // NotifyUILoopOfPendingFunctions, which is virtual.
  is_in_destructor_ = true;
  // Make sure the final ExecutePendingFunctionsFromUIThread doesn't call
  // PlatformQuitFromUIThread, which is virtual.
  has_quit_ = true;

  // Platform-specific quit is expected to be performed by the subclass (the
  // order of it vs. the final ExecutePendingFunctionsFromUIThread shouldn't
  // matter anymore, the implementation may assume that no pending functions
  // will be requested for execution specifically via the platform-specific
  // loop, as there should be no more references to the context in other
  // threads), can't call the virtual PlatformQuitFromUIThread anymore.
  ExecutePendingFunctionsFromUIThread(true);
}

bool WindowedAppContext::CallInUIThreadDeferred(
    std::function<void()> function) {
  return CallInUIThreadDeferredTagged("CallInUIThreadDeferred",
                                      std::move(function));
}

bool WindowedAppContext::CallInUIThreadDeferredTagged(
    const char* tag, std::function<void()> function) {
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  fprintf(stderr,
          "[WindowedAppContext] DEBUG: CallInUIThreadDeferred called from thread %s (tag=%s)\n",
          oss.str().c_str(), tag ? tag : "unknown");

  {
    std::unique_lock<std::mutex> pending_functions_lock(
        pending_functions_mutex_);
    if (!pending_functions_accepted_) {
      fprintf(stderr, "[WindowedAppContext] DEBUG: pending_functions_accepted_ is false, rejecting\n");
      // Will not be called as the loop will not be executed anymore.
      return false;
    }
    size_t queue_size_before = pending_functions_.size();
    PendingFunction pending;
    pending.fn = std::move(function);
    pending.tag = tag ? tag : "unknown";
    pending.id = next_pending_id_.fetch_add(1, std::memory_order_relaxed);
    fprintf(stderr,
            "[WindowedAppContext] DEBUG: Enqueued pending function id=%llu tag=%s\n",
            static_cast<unsigned long long>(pending.id),
            pending.tag);
    pending_functions_.emplace_back(std::move(pending));
    size_t queue_size_after = pending_functions_.size();
    fprintf(stderr, "[WindowedAppContext] DEBUG: Function added to queue (size: %zu -> %zu)\n",
            queue_size_before, queue_size_after);
  }
  // Notify unconditionally, even if currently running pending functions. It's
  // possible for pending functions themselves to run inner platform message
  // loops, such as when displaying dialogs - in this case, the notification is
  // needed to run the new function from such an inner loop. A modal loop can be
  // started even in leftovers happening during the quit, where there's still
  // opportunity for enqueueing and executing new pending functions - so only
  // checking if called in the destructor (it's safe to check this without
  // locking a mutex as it's assumed that if the object is already being
  // destroyed, no other threads can have references to it - any access would
  // result in a race condition anyway) as the subclass has already been
  // destroyed. Having pending_functions_mutex_ unlocked also means that
  // NotifyUILoopOfPendingFunctions may be done while the UI thread is calling
  // or has already called PlatformQuitFromUIThread - but it's better than
  // keeping pending_functions_mutex_ locked as NotifyUILoopOfPendingFunctions
  // may be implemented as pushing to a fixed-size pipe, in which case it will
  // have to wait until free space is available, but if the UI thread tries to
  // lock the mutex afterwards to execute pending functions (and encouters
  // contention), nothing will be able to receive from the pipe anymore and thus
  // free the space, causing a deadlock.
  if (!is_in_destructor_) {
    fprintf(stderr, "[WindowedAppContext] DEBUG: Calling NotifyUILoopOfPendingFunctions\n");
    NotifyUILoopOfPendingFunctions();
    fprintf(stderr, "[WindowedAppContext] DEBUG: NotifyUILoopOfPendingFunctions returned\n");
  } else {
    fprintf(stderr, "[WindowedAppContext] DEBUG: In destructor, skipping NotifyUILoopOfPendingFunctions\n");
  }
  return true;
}

bool WindowedAppContext::CallInUIThread(std::function<void()> function) {
  return CallInUIThreadTagged("CallInUIThread", std::move(function));
}

bool WindowedAppContext::CallInUIThreadTagged(
    const char* tag, std::function<void()> function) {
  if (IsInUIThread()) {
    // The intention is just to make sure the code is executed in the UI thread,
    // don't defer execution if no need to.
    function();
    return true;
  }
  return CallInUIThreadDeferredTagged(tag, std::move(function));
}

bool WindowedAppContext::CallInUIThreadSynchronous(
    std::function<void()> function) {
  return CallInUIThreadSynchronousTagged("CallInUIThreadSynchronous",
                                         std::move(function));
}

bool WindowedAppContext::CallInUIThreadSynchronousTagged(
    const char* tag, std::function<void()> function) {
  const char* wait_tag = tag ? tag : "unknown";
  int wait_warn_after_ms = LongWaitWarningThresholdMsForTag(wait_tag);

  std::ostringstream oss1;
  oss1 << std::this_thread::get_id();
  fprintf(stderr,
          "[WindowedAppContext] CallInUIThreadSynchronous called from thread %s (tag=%s)\n",
          oss1.str().c_str(), wait_tag);
  std::ostringstream oss2;
  oss2 << ui_thread_id_;
  fprintf(stderr, "[WindowedAppContext] UI thread ID is: %s\n", oss2.str().c_str());
  
  if (IsInUIThread()) {
    // Prevent deadlock if called from the UI thread.
    fprintf(stderr, "[WindowedAppContext] Already in UI thread, executing immediately\n");
    function();
    return true;
  }
  
  fprintf(stderr, "[WindowedAppContext] NOT in UI thread, using fence...\n");
  xe::threading::Fence fence;
  bool fence_signaled = false;
  
  if (!CallInUIThreadDeferredTagged(wait_tag, [&function, &fence, &fence_signaled]() {
        fprintf(stderr, "[WindowedAppContext] FENCE LAMBDA EXECUTING!\n");
        function();
        fprintf(stderr, "[WindowedAppContext] Function executed, signaling fence...\n");
        fence.Signal();
        fence_signaled = true;
        fprintf(stderr, "[WindowedAppContext] Fence signaled!\n");
      })) {
    fprintf(stderr, "[WindowedAppContext] CallInUIThreadDeferred FAILED!\n");
    return false;
  }
  
  fprintf(stderr, "[WindowedAppContext] Waiting on fence...\n");
  
  // CRITICAL FIX: Enhanced debugging for SDL2 fence issues
  // SDL2 event loop needs to be actively processing to complete the fence
  fprintf(stderr, "[WindowedAppContext] About to call fence.Wait() - this will block until SDL2 processes pending functions\n");

  auto wait_done = std::make_shared<std::atomic<bool>>(false);
    std::thread([wait_done, wait_tag, wait_warn_after_ms, this]() {
      int elapsed_ms = 0;
      int wakeup_attempts = 0;
      while (!wait_done->load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        elapsed_ms += 500;
        if (wait_done->load(std::memory_order_relaxed)) {
          return;
        }
        if (elapsed_ms < wait_warn_after_ms) {
          continue;
        }
        fprintf(stderr,
                "[WindowedAppContext] DEBUG: fence.Wait still blocked after %d ms (tag=%s)\n",
                elapsed_ms, wait_tag);
        size_t pending_size = 0;
        {
          std::lock_guard<std::mutex> lock(pending_functions_mutex_);
          pending_size = pending_functions_.size();
        }
        fprintf(stderr,
                "[WindowedAppContext] DEBUG: pending_functions_ size: %zu\n",
                pending_size);
        {
          std::lock_guard<std::mutex> lock(pending_functions_mutex_);
          size_t idx = 0;
          for (const auto& pending : pending_functions_) {
            fprintf(stderr,
                    "[WindowedAppContext] DEBUG: queued pending[%zu]: id=%llu tag=%s\n",
                    idx,
                    static_cast<unsigned long long>(pending.id),
                    pending.tag ? pending.tag : "unknown");
            if (++idx >= 8) {
              size_t remaining = pending_functions_.size() - idx;
              if (remaining > 0) {
                fprintf(stderr,
                        "[WindowedAppContext] DEBUG: ... %zu more pending not shown\n",
                        remaining);
              }
              break;
            }
          }
        }
        uint64_t active_id = active_pending_id_.load(std::memory_order_relaxed);
        const char* active_tag =
            active_pending_tag_.load(std::memory_order_relaxed);
        int64_t active_start =
            active_pending_start_ms_.load(std::memory_order_relaxed);
        if (active_id != 0 && active_start != 0) {
          int64_t active_elapsed = NowMs() - active_start;
          fprintf(stderr,
                  "[WindowedAppContext] DEBUG: active pending id=%llu tag=%s running for %lld ms\n",
                  static_cast<unsigned long long>(active_id),
                  active_tag ? active_tag : "unknown",
                  static_cast<long long>(active_elapsed));
        }
        // Fallback wakeup: if the fence wait is taking too long, poke the UI loop again.
        if ((elapsed_ms % 2000) == 0 && wakeup_attempts < 5) {
          if (!is_in_destructor_) {
            fprintf(stderr,
                    "[WindowedAppContext] DEBUG: fallback UI wakeup attempt #%d at %d ms\n",
                    wakeup_attempts + 1, elapsed_ms);
            NotifyUILoopOfPendingFunctions();
          }
          ++wakeup_attempts;
        }
        fflush(stderr);
      }
    }).detach();
  
  // The basic Wait() should work if SDL2 event loop is processing
  // If it hangs, it means SDL2 is not calling ExecutePendingFunctionsFromUIThread()
  fence.Wait();
  wait_done->store(true, std::memory_order_relaxed);
  
  fprintf(stderr, "[WindowedAppContext] Fence wait completed!\n");
  return true;
}

size_t WindowedAppContext::GetPendingFunctionsCount() const {
  std::lock_guard<std::mutex> lock(pending_functions_mutex_);
  return pending_functions_.size();
}

void WindowedAppContext::QuitFromUIThread() {
  assert_true(IsInUIThread());
  bool has_quit_previously = has_quit_;
  // Make sure PlatformQuitFromUIThread is called only once, not from nested
  // pending function execution during the quit - otherwise it will be called
  // when it's still possible to add new pending functions. This isn't as wrong
  // as calling PlatformQuitFromUIThread from the destructor, but still a part
  // of the contract for simplicity.
  has_quit_ = true;
  // Executing pending function unconditionally because it's the contract of
  // this method that functions are executed immediately.
  ExecutePendingFunctionsFromUIThread(true);
  if (has_quit_previously) {
    // Potentially calling QuitFromUIThread from inside a pending function (in
    // the worst and dangerous case, from a pending function executed in the
    // destructor - and PlatformQuitFromUIThread is virtual).
    return;
  }
  // Call the platform-specific shutdown while letting it assume that no new
  // functions will be queued anymore (but NotifyUILoopOfPendingFunctions may
  // still be called after PlatformQuitFromUIThread as the two are not
  // interlocked). This is different than the order in the destruction, but
  // there this assumption is ensured by the expectation that there should be no
  // more references to the context in other threads that would allow queueing
  // new functions with calling NotifyUILoopOfPendingFunctions.
  PlatformQuitFromUIThread();
}

void WindowedAppContext::ExecutePendingFunctionsFromUIThread(bool is_final) {
  assert_true(IsInUIThread());
  std::unique_lock<std::mutex> pending_functions_lock(pending_functions_mutex_);

  size_t queue_size = pending_functions_.size();
  
  // Smart logging: Only log when we have work to do or on final execution
  bool should_log_entry = (queue_size > 0) || is_final;
  
  if (should_log_entry) {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    fprintf(stderr, "[WindowedAppContext] DEBUG: ExecutePendingFunctionsFromUIThread ENTRY (thread %s, is_final=%d)\n",
            oss.str().c_str(), is_final);
    fprintf(stderr, "[WindowedAppContext] DEBUG: Pending functions queue size: %zu\n", queue_size);
  }

  int executed_count = 0;
  while (!pending_functions_.empty()) {
    // Removing the function from the queue before executing it, as the function
    // itself may call ExecutePendingFunctionsFromUIThread - if it's kept, the
    // inner loop will try to execute it again, resulting in potentially endless
    // recursion, and even if it's terminated, each level will be trying to
    // remove the same function from the queue - instead, actually removing
    // other functions, or even beyond the end of the queue.
    PendingFunction pending = std::move(pending_functions_.front());
    pending_functions_.pop_front();
    executed_count++;

    auto fn_start = std::chrono::steady_clock::now();
    auto fn_done = std::make_shared<std::atomic<bool>>(false);
    int pending_warn_after_ms = LongWaitWarningThresholdMsForTag(pending.tag);
    std::thread([fn_done, executed_count, pending_id = pending.id,
                 pending_tag = pending.tag,
                 pending_warn_after_ms]() {
      int elapsed_ms = 0;
      while (!fn_done->load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        elapsed_ms += 500;
        if (fn_done->load(std::memory_order_relaxed)) {
          return;
        }
        if (elapsed_ms < pending_warn_after_ms) {
          continue;
        }
        fprintf(stderr,
                "[WindowedAppContext] DEBUG: Pending function #%d (id=%llu tag=%s) running for %d ms\n",
                executed_count,
                static_cast<unsigned long long>(pending_id),
                pending_tag ? pending_tag : "unknown",
                elapsed_ms);
        fflush(stderr);
      }
    }).detach();

    fprintf(stderr,
            "[WindowedAppContext] DEBUG: Executing pending function #%d (id=%llu tag=%s, queue size now: %zu)\n",
            executed_count,
            static_cast<unsigned long long>(pending.id),
            pending.tag ? pending.tag : "unknown",
            pending_functions_.size());

    // Call the function with the lock released as it may take an indefinitely
    // long time to execute if it opens some dialog (possibly with its own
    // platform message loop), and in that case, without unlocking, no other
    // thread would be able to add new pending functions (which would result in
    // unintended waits for user input). This also allows using std::mutex
    // instead of std::recursive_mutex.
    int64_t fn_start_ms = NowMs();
    active_pending_id_.store(pending.id, std::memory_order_relaxed);
    active_pending_tag_.store(pending.tag, std::memory_order_relaxed);
    active_pending_start_ms_.store(fn_start_ms, std::memory_order_relaxed);
    pending_functions_lock.unlock();
    pending.fn();
    fn_done->store(true, std::memory_order_relaxed);
    active_pending_id_.store(0, std::memory_order_relaxed);
    active_pending_tag_.store(nullptr, std::memory_order_relaxed);
    active_pending_start_ms_.store(0, std::memory_order_relaxed);
    auto fn_end = std::chrono::steady_clock::now();
    auto fn_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        fn_end - fn_start).count();
    fprintf(stderr,
            "[WindowedAppContext] DEBUG: Pending function #%d (id=%llu tag=%s) completed in %lld ms\n",
            executed_count,
            static_cast<unsigned long long>(pending.id),
            pending.tag ? pending.tag : "unknown",
            static_cast<long long>(fn_ms));
    pending_functions_lock.lock();
  }

  if (should_log_entry || executed_count > 0) {
    fprintf(stderr, "[WindowedAppContext] DEBUG: ExecutePendingFunctionsFromUIThread EXIT - executed %d functions\n",
            executed_count);
  }

  if (is_final) {
    // Atomically with completion of the pending functions loop, disallow adding
    // new functions after executing the existing ones - it was possible to
    // enqueue new functions from the leftover ones as there still was
    // opportunity to call them, so it wasn't necessary to disallow adding
    // before executing, but now new functions will potentially never be
    // executed. This is done even if this is just an inner pending functions
    // execution and there's still potential possibility of adding and executing
    // new functions in the outer loops - for simplicity and consistency (so
    // QuitFromUIThread's behavior doesn't depend as much on the location of the
    // call - inside a pending function or from some system callback of the
    // window), assuming after a PlatformQuitFromUIThread call, it's not
    // possible to add new pending functions anymore.
    pending_functions_accepted_ = false;
    if (should_log_entry) {
      fprintf(stderr, "[WindowedAppContext] DEBUG: is_final=true, pending_functions_accepted_ set to false\n");
    }
  }
}

}  // namespace ui
}  // namespace xe
