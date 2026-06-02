/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_SCHEDULER_H_
#define XENIA_KERNEL_GUEST_SCHEDULER_H_

#include <atomic>
#include <memory>
#include <mutex>

#include "xenia/base/threading.h"

namespace xe {
namespace kernel {

class KernelState;
class XThread;

// Cooperative, in-kernel scheduler for guest threads.
//
// First stage: all guest threads run as host fibers multiplexed onto a single
// dispatch host thread, scheduled round-robin. Scheduling is purely
// cooperative. A fiber yields control only at explicit points (thread exit,
// waits, delays, NtYieldExecution), with no forced preemption. Running on one
// host thread makes scheduling deterministic, with no cross-thread races to
// reason about while the cooperative model is brought up. Parallelism across
// the guest's logical CPUs comes later.
class GuestScheduler {
 public:
  explicit GuestScheduler(KernelState* kernel_state);
  ~GuestScheduler();

  // True if the cooperative scheduler is active (gated by the cvar).
  static bool enabled();

  // Starts the dispatch host thread the first time it is called (no-op after).
  void EnsureStarted();
  void Shutdown();

  // Adds |thread| to the ready queue and wakes the dispatcher. Idempotent and
  // safe to call from any host thread.
  void MarkReady(XThread* thread);

  // Yields the running guest fiber back to the dispatcher's idle fiber. Returns
  // (on the calling fiber) once the dispatcher switches back into it.
  void YieldToScheduler();

  // Cooperative yield (NtYieldExecution): re-queue the current thread, then let
  // the dispatcher pick the next ready thread.
  void YieldCurrentThread();

  // Parks the running guest fiber in the blocked (waiting) list and yields.
  // Returns once the dispatcher re-readies it, after a short poll backoff or
  // sooner if another thread becomes runnable, so a cooperative wait can
  // re-poll its host primitive. The deadline is owned by the caller's poll
  // loop.
  void BlockCurrentThread();

  // Marks the running guest fiber finished so the dispatcher can reclaim it,
  // dropping the final handle once control is back on the idle fiber. Call
  // before the final YieldToScheduler().
  void NotifyThreadExited(XThread* thread);

  // Detaches |thread| from the ready queue. Used by external Terminate so the
  // dispatcher can't reach a soon-to-be-freed XThread via a dangling pointer.
  void ForgetThread(XThread* thread);

  KernelState* kernel_state() const { return kernel_state_; }

 private:
  // Single-thread busy-poll backoff: when no thread is ready but some are
  // blocked, the dispatcher sleeps at most this long before re-polling them, so
  // a signal from another host thread (GPU, I/O, ...) is observed promptly
  // without spinning a core. This is the wake-latency floor for the cooperative
  // waits until a proper wake mechanism replaces the busy-poll.
  static constexpr uint64_t kPollBackoffMs = 1;

  XThread* DequeueReady();
  void SwitchTo(XThread* next);
  void RunLoop();
  // Moves every blocked thread back onto the ready queue so it re-polls.
  void RereadyBlocked();
  // Unlinks |thread| from a singly-linked list (ready_next), fixing up tail.
  static void UnlinkLocked(XThread*& head, XThread*& tail, XThread* thread);

  KernelState* kernel_state_;

  // Guards the ready and blocked lists. Never held across a fiber switch.
  std::mutex lock_;
  // Intrusive ready FIFO, linked through XThread::scheduler_links().ready_next.
  XThread* ready_head_ = nullptr;
  XThread* ready_tail_ = nullptr;
  // Intrusive list of fibers blocked in a cooperative wait, linked through the
  // same ready_next (a thread is in at most one list).
  XThread* blocked_head_ = nullptr;
  XThread* blocked_tail_ = nullptr;
  // A thread that exited on its fiber, awaiting reclaim by the dispatch loop
  // (its final handle is dropped once control is back on the idle fiber).
  XThread* exited_thread_ = nullptr;

  std::unique_ptr<xe::threading::Thread> host_thread_;
  std::unique_ptr<xe::threading::Fiber> idle_fiber_;
  std::unique_ptr<xe::threading::Event> ready_event_;

  std::atomic<bool> started_{false};
  std::atomic<bool> shutting_down_{false};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_H_
