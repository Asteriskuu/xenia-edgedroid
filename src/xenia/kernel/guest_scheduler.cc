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
#include "xenia/base/clock.h"
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
  // Don't re-enqueue a terminated thread, or a stray Resume could revive a
  // zombie.
  if (thread->guest_object<X_KTHREAD>()->thread_state ==
      KTHREAD_STATE_TERMINATED) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    // Blocked threads move via RereadyBlocked, not here, since ready and
    // blocked share ready_next and a thread must be in only one list.
    assert_false(links.blocked);
    if (links.queued) {
      return;
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

void GuestScheduler::UnlinkLocked(XThread*& head, XThread*& tail,
                                  XThread* thread) {
  XThread** link = &head;
  XThread* prev = nullptr;
  while (*link) {
    if (*link == thread) {
      *link = thread->scheduler_links().ready_next;
      if (tail == thread) {
        tail = prev;
      }
      return;
    }
    prev = *link;
    link = &(*link)->scheduler_links().ready_next;
  }
}

void GuestScheduler::ForgetThread(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  if (links.queued) {
    UnlinkLocked(ready_head_, ready_tail_, thread);
    links.queued = false;
  } else if (links.blocked) {
    UnlinkLocked(blocked_head_, blocked_tail_, thread);
    links.blocked = false;
  }
  links.ready_next = nullptr;
}

void GuestScheduler::SwitchTo(XThread* next) {
  assert_not_null(next);
  assert_not_null(next->fiber());
  auto& links = next->scheduler_links();
  if (!links.has_run) {
    links.has_run = true;
    XELOGI("GuestScheduler: first run tid={:08X} '{}'", next->thread_id(),
           next->thread_name());
  }
  XThread::SetCurrentThread(next);
  next->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_RUNNING;
  next->fiber()->SwitchTo();
  // Back on the idle fiber.
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
  XELOGI("GuestScheduler: exited tid={:08X} '{}'", thread->thread_id(),
         thread->thread_name());
  // The dispatch loop reclaims it, since we can't drop the last handle while
  // running on its fiber.
  exited_thread_ = thread;
}

void GuestScheduler::BlockCurrentThread() {
  XThread* self = XThread::GetCurrentThread();
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = self->scheduler_links();
    // Park self (running, in no list) on the blocked list.
    links.blocked = true;
    links.ready_next = nullptr;
    if (blocked_tail_) {
      blocked_tail_->scheduler_links().ready_next = self;
    } else {
      blocked_head_ = self;
    }
    blocked_tail_ = self;
  }
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_WAITING;
  YieldToScheduler();
}

void GuestScheduler::RereadyBlocked() {
  std::lock_guard<std::mutex> lock(lock_);
  XThread* t = blocked_head_;
  while (t) {
    auto& links = t->scheduler_links();
    XThread* next = links.ready_next;
    links.blocked = false;
    links.queued = true;
    links.ready_next = nullptr;
    if (ready_tail_) {
      ready_tail_->scheduler_links().ready_next = t;
    } else {
      ready_head_ = t;
    }
    ready_tail_ = t;
    t = next;
  }
  blocked_head_ = nullptr;
  blocked_tail_ = nullptr;
}

void GuestScheduler::RunLoop() {
  // Adopt this host thread's stack as the dispatcher's idle fiber.
  idle_fiber_ = xe::threading::Fiber::CreateFromThread();
  XELOGI("GuestScheduler: dispatch loop started");

  uint64_t next_repoll_ms = 0;
  while (!shutting_down_.load()) {
    // Re-poll blocked waiters on a timer even while other fibers run, or a busy
    // fiber that rarely waits would starve them.
    uint64_t now = Clock::QueryHostUptimeMillis();
    if (now >= next_repoll_ms) {
      RereadyBlocked();
      next_repoll_ms = now + kPollBackoffMs;
    }

    XThread* next = DequeueReady();
    if (next) {
      exited_thread_ = nullptr;
      SwitchTo(next);
      if (exited_thread_) {
        // Safe to drop the last handle now, which may free the XThread and its
        // fiber. We're on the idle fiber and the exited fiber is parked on its
        // final yield, so we never free the running fiber.
        XThread* dead = exited_thread_;
        exited_thread_ = nullptr;
        dead->ReleaseHandle();
      }
      continue;
    }

    // Nothing ready, so sleep until the next re-poll if waiters are blocked (a
    // MarkReady wakes us sooner), otherwise idle until something is runnable.
    bool have_blocked;
    {
      std::lock_guard<std::mutex> lock(lock_);
      have_blocked = blocked_head_ != nullptr;
    }
    if (!have_blocked) {
      xe::threading::Wait(ready_event_.get(), false);
      continue;
    }
    now = Clock::QueryHostUptimeMillis();
    uint64_t sleep_ms = next_repoll_ms > now ? next_repoll_ms - now : 0;
    xe::threading::Wait(ready_event_.get(), false,
                        std::chrono::milliseconds(sleep_ms));
  }
  XELOGI("GuestScheduler: dispatch loop exited (shutting_down={})",
         shutting_down_.load());
}

}  // namespace kernel
}  // namespace xe
