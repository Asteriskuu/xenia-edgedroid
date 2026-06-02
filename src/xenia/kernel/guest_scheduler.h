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
// The 360's 6 logical CPUs are spread across a configurable number of dispatch
// host threads (guest_scheduler_cpus: 1, 3, or 6). Each guest thread runs as a
// host fiber pinned by its guest current_cpu to one dispatch thread. Fibers on
// different dispatch threads run truly in parallel, while fibers sharing one
// are cooperatively scheduled, switching only at yield points (waits, delays,
// NtYieldExecution, exit). There is no forced preemption. A single lock guards
// every per-CPU queue and is never held across a fiber switch.
class GuestScheduler {
 public:
  explicit GuestScheduler(KernelState* kernel_state);
  ~GuestScheduler();

  // True if the cooperative scheduler is active (gated by the cvar).
  static bool enabled();

  // Starts the per-CPU dispatch threads the first time it is called (no-op
  // after).
  void EnsureStarted();
  void Shutdown();

  // Adds |thread| to its CPU's ready queue (by guest affinity) and wakes that
  // CPU. Idempotent and safe to call from any host thread. The thread must not
  // be currently running on a dispatch thread.
  void MarkReady(XThread* thread);

  // Yields the running guest fiber back to its CPU's idle fiber. Returns (on
  // the calling fiber) once the dispatcher switches back into it.
  void YieldToScheduler();

  // Cooperative yield (NtYieldExecution): re-queue the current thread on its
  // current CPU, then let that CPU pick the next ready thread.
  void YieldCurrentThread();

  // Parks the running guest fiber on its CPU's blocked list and yields. Returns
  // once the dispatcher re-readies it, after a short poll backoff or sooner if
  // another thread becomes runnable, so a cooperative wait can re-poll its host
  // primitive. The deadline is owned by the caller's poll loop.
  void BlockCurrentThread();

  // Marks the running guest fiber finished so its CPU can reclaim it, dropping
  // the final handle once control is back on the idle fiber. Call before the
  // final YieldToScheduler().
  void NotifyThreadExited(XThread* thread);

  // Detaches |thread| from every ready and blocked queue. Used by external
  // Terminate so a dispatcher can't reach a soon-to-be-freed XThread.
  void ForgetThread(XThread* thread);

  KernelState* kernel_state() const { return kernel_state_; }

 private:
  // Xbox 360 logical CPU count, also the maximum number of dispatch threads.
  static constexpr int kMaxCpus = 6;
  // When no thread is ready but some are blocked, a CPU sleeps at most this
  // long before re-polling them, so a signal from another host thread (GPU,
  // I/O, or a fiber on another CPU) is observed promptly without spinning a
  // core.
  static constexpr uint64_t kPollBackoffMs = 1;

  // Per-CPU dispatch state, each driven by its own host thread. Ready and
  // blocked fibers are intrusive FIFOs linked through
  // XThread::scheduler_links().ready_next (a thread is in at most one list).
  struct Cpu {
    XThread* ready_head = nullptr;
    XThread* ready_tail = nullptr;
    XThread* blocked_head = nullptr;
    XThread* blocked_tail = nullptr;
    XThread* exited_thread = nullptr;
    std::unique_ptr<xe::threading::Thread> host_thread;
    std::unique_ptr<xe::threading::Fiber> idle_fiber;
    std::unique_ptr<xe::threading::Event> ready_event;
  };

  // The dispatch thread a thread is pinned to: its guest current_cpu mapped
  // onto the active host_cpu_count_ dispatch threads.
  int CpuOf(XThread* thread) const;

  void EnqueueReady(XThread* thread, int cpu_index);
  XThread* DequeueReady(int cpu_index);
  void SwitchTo(XThread* next);
  void RereadyBlocked(int cpu_index);
  void RunLoop(int cpu_index);
  // Unlinks |thread| from a singly-linked list (ready_next), fixing up tail.
  static void UnlinkLocked(XThread*& head, XThread*& tail, XThread* thread);

  KernelState* kernel_state_;

  // Number of active dispatch threads (guest_scheduler_cpus, 1..kMaxCpus).
  int host_cpu_count_ = kMaxCpus;

  // Guards every CPU's ready and blocked lists. Never held across a fiber
  // switch.
  std::mutex lock_;
  Cpu cpus_[kMaxCpus];

  std::atomic<bool> started_{false};
  std::atomic<bool> shutting_down_{false};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_H_
