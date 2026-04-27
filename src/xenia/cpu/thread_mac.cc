/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/thread_mac.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

thread_local Thread* Thread::current_thread_ = nullptr;

Thread::Thread() {}
Thread::~Thread() {}

bool Thread::IsInThread() { return current_thread_ != nullptr; }

Thread* Thread::GetCurrentThread() { return current_thread_; }

void Thread::SetCurrentThread(Thread* thread) { current_thread_ = thread; }

uint32_t Thread::GetCurrentThreadId() {
#ifdef __APPLE__
  Thread* current_thread = Thread::GetCurrentThread();
  if (current_thread && current_thread->thread_state()) {
    uint32_t xenon_thread_id = current_thread->thread_state()->thread_id();
    uint64_t macos_thread_id = 0;
    pthread_threadid_np(pthread_self(), &macos_thread_id);
    const unsigned long long macos_thread_id_ull =
        static_cast<unsigned long long>(macos_thread_id);

    fprintf(stderr, "[DEBUG] Thread::GetCurrentThreadId() called\n");
    fprintf(stderr, "  Xenon thread ID: 0x%08X\n", xenon_thread_id);
    fprintf(stderr, "  macOS thread ID: %llu (0x%llX)\n", macos_thread_id_ull,
            macos_thread_id_ull);

    return xenon_thread_id;
  } else {
    fprintf(stderr, "[DEBUG] WARNING: No current thread or thread state\n");
  }
#endif

  return Thread::GetCurrentThread()->thread_state()->thread_id();
}

}  // namespace cpu
}  // namespace xe
