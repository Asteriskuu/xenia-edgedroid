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
// dispatch host thread, scheduled round-robin. A fiber yields control only at
// explicit points -- thread exit and NtYieldExecution today; cooperative waits
// and JIT timeslice preemption are layered on in later stages. Running on one
// host thread makes scheduling deterministic: there are no cross-thread races
// to reason about while the cooperative model is brought up. Parallelism across
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

  // Marks the running guest fiber finished; the dispatcher reclaims it (drops
  // the final handle once control is back on the idle fiber). Call immediately
  // before the final YieldToScheduler().
  void NotifyThreadExited(XThread* thread);

  // Detaches |thread| from the ready queue. Used by external Terminate so the
  // dispatcher can't reach a soon-to-be-freed XThread via a dangling pointer.
  void ForgetThread(XThread* thread);

  KernelState* kernel_state() const { return kernel_state_; }

 private:
  XThread* DequeueReady();
  void SwitchTo(XThread* next);
  void RunLoop();

  KernelState* kernel_state_;

  // Guards the ready queue. Never held across a fiber switch.
  std::mutex lock_;
  // Intrusive ready FIFO, linked through XThread::scheduler_links().ready_next.
  XThread* ready_head_ = nullptr;
  XThread* ready_tail_ = nullptr;
  // The fiber the dispatcher has switched into (null while on the idle fiber).
  XThread* current_thread_ = nullptr;
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
