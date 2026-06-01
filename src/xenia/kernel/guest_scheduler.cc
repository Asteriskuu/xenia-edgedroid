/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler.h"

#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

GuestScheduler::GuestScheduler(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

GuestScheduler::~GuestScheduler() { Shutdown(); }

bool GuestScheduler::enabled() { return cvars::guest_scheduler; }

void GuestScheduler::EnsureStarted() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  ready_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  xe::threading::Thread::CreationParameters params;
  host_thread_ = xe::threading::Thread::Create(params, [this]() { RunLoop(); });
  host_thread_->set_name("Guest Scheduler");
}

void GuestScheduler::Shutdown() {
  if (!started_.load()) {
    return;
  }
  shutting_down_.store(true);
  if (ready_event_) {
    ready_event_->Set();
  }
  if (host_thread_) {
    // Join before reset(): reset() only closes the handle.
    xe::threading::Wait(host_thread_.get(), false);
    host_thread_.reset();
  }
}

void GuestScheduler::MarkReady(XThread* thread) {
  assert_not_null(thread);
  // Refuse to enqueue a terminated thread: a stray Resume/Terminate race could
  // otherwise put a zombie back on the ready queue after it has exited.
  if (thread->guest_object<X_KTHREAD>()->thread_state ==
      KTHREAD_STATE_TERMINATED) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    if (links.queued) {
      return;  // already ready; don't double-enqueue
    }
    links.queued = true;
    links.ready_next = nullptr;
    if (ready_tail_) {
      ready_tail_->scheduler_links().ready_next = thread;
    } else {
      ready_head_ = thread;
    }
    ready_tail_ = thread;
  }
  if (ready_event_) {
    ready_event_->Set();
  }
}

XThread* GuestScheduler::DequeueReady() {
  std::lock_guard<std::mutex> lock(lock_);
  XThread* thread = ready_head_;
  if (!thread) {
    return nullptr;
  }
  auto& links = thread->scheduler_links();
  ready_head_ = links.ready_next;
  if (!ready_head_) {
    ready_tail_ = nullptr;
  }
  links.ready_next = nullptr;
  links.queued = false;
  return thread;
}

void GuestScheduler::ForgetThread(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  if (!links.queued) {
    return;
  }
  XThread** link = &ready_head_;
  XThread* prev = nullptr;
  while (*link) {
    if (*link == thread) {
      *link = links.ready_next;
      if (ready_tail_ == thread) {
        ready_tail_ = prev;
      }
      break;
    }
    prev = *link;
    link = &(*link)->scheduler_links().ready_next;
  }
  links.ready_next = nullptr;
  links.queued = false;
}

void GuestScheduler::SwitchTo(XThread* next) {
  assert_not_null(next);
  assert_not_null(next->fiber());
  current_thread_ = next;
  XThread::SetCurrentThread(next);
  next->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_RUNNING;
  next->fiber()->SwitchTo();
  // Back on the idle fiber: the guest yielded or exited.
  current_thread_ = nullptr;
  XThread::SetCurrentThread(nullptr);
}

void GuestScheduler::YieldToScheduler() {
  assert_not_null(idle_fiber_);
  idle_fiber_->SwitchTo();
}

void GuestScheduler::YieldCurrentThread() {
  XThread* self = XThread::GetCurrentThread();
  MarkReady(self);
  YieldToScheduler();
}

void GuestScheduler::NotifyThreadExited(XThread* thread) {
  // Hand the finished thread to the dispatch loop to reclaim; we can't drop the
  // final handle here because we're still running on its fiber.
  exited_thread_ = thread;
}

void GuestScheduler::RunLoop() {
  // Adopt this host thread's stack as the dispatcher's idle fiber.
  idle_fiber_ = xe::threading::Fiber::CreateFromThread();
  XELOGI("GuestScheduler: dispatch loop started");

  while (!shutting_down_.load()) {
    XThread* next = DequeueReady();
    if (!next) {
      xe::threading::Wait(ready_event_.get(), false);
      continue;
    }
    exited_thread_ = nullptr;
    SwitchTo(next);
    if (exited_thread_) {
      // Safe to drop the final reference (which may destroy the XThread, and
      // with it its Fiber): control is back on the idle fiber, the exited
      // thread's fiber is suspended on its terminal YieldToScheduler, and the
      // dispatcher holds no other reference to it. The fiber being destroyed is
      // the one we just switched away from, never the one currently executing.
      XThread* dead = exited_thread_;
      exited_thread_ = nullptr;
      dead->ReleaseHandle();
    }
  }
  XELOGI("GuestScheduler: dispatch loop exited (shutting_down={})",
         shutting_down_.load());
}

}  // namespace kernel
}  // namespace xe
