/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <functional>
#include <memory>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"

#if XE_PLATFORM_WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// Boost.Context fcontext_t primitives. The implementation is the hand-written
// assembly in the boostorg/context submodule (third_party/boost_context),
// linked via the boost_context static lib. These three routines carry no Boost
// header dependencies, so we declare the two we use directly here.
extern "C" {
typedef void* fcontext_t;
struct fcontext_transfer_t {
  fcontext_t fctx;
  void* data;
};
fcontext_t make_fcontext(void* sp, size_t size,
                         void (*fn)(fcontext_transfer_t));
fcontext_transfer_t jump_fcontext(fcontext_t to, void* vp);
}  // extern "C"

namespace xe {
namespace threading {

namespace {

// The fiber currently executing on this host thread.
thread_local Fiber* current_fiber_ = nullptr;

size_t HostPageSize() {
#if XE_PLATFORM_WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<size_t>(si.dwPageSize);
#else
  return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

// A guard-paged fiber stack: the lowest page is no-access so an overflow
// faults instead of silently corrupting adjacent memory.
struct FiberStack {
  void* base = nullptr;  // low address of the whole allocation
  size_t total = 0;      // whole allocation, including the guard page
  void* high = nullptr;  // one past the top (what make_fcontext wants)
  size_t usable = 0;     // total minus the guard page
};

FiberStack AllocFiberStack(size_t size) {
  const size_t page = HostPageSize();
  size_t pages = (size + page - 1) / page;
  if (pages < 1) {
    pages = 1;
  }
  FiberStack s;
  s.total = (pages + 1) * page;  // +1 guard page at the low end

#if XE_PLATFORM_WIN32
  s.base =
      VirtualAlloc(nullptr, s.total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  assert_not_null(s.base);
  DWORD old_protect;
  VirtualProtect(s.base, page, PAGE_READWRITE | PAGE_GUARD, &old_protect);
#else
  s.base = mmap(nullptr, s.total, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert_true(s.base != MAP_FAILED);
  mprotect(s.base, page, PROT_NONE);
#endif

  s.high = static_cast<char*>(s.base) + s.total;
  s.usable = s.total - page;
  return s;
}

void FreeFiberStack(FiberStack& s) {
  if (!s.base) {
    return;
  }
#if XE_PLATFORM_WIN32
  VirtualFree(s.base, 0, MEM_RELEASE);
#else
  munmap(s.base, s.total);
#endif
  s = FiberStack{};
}

class FcontextFiber : public Fiber {
 public:
  // Created fiber: owns a guard-paged stack and a context entered the first
  // time it is switched to.
  FcontextFiber(size_t stack_size, std::function<void()> start_routine)
      : entry_(std::move(start_routine)) {
    stack_ = AllocFiberStack(stack_size);
    fctx_ = make_fcontext(stack_.high, stack_.usable,
                          &FcontextFiber::EntryTrampoline);
    owns_stack_ = true;
  }

  // Adopted fiber: wraps the calling thread's existing stack. Its context is
  // captured the first time something switches away from it.
  FcontextFiber() = default;

  ~FcontextFiber() override {
    if (owns_stack_) {
      FreeFiberStack(stack_);
    }
  }

  void SwitchTo() override {
    auto* prev = static_cast<FcontextFiber*>(current_fiber_);
    // SwitchTo requires the host thread to have a current fiber (established by
    // CreateFromThread or a prior switch).
    assert_not_null(prev);
    assert_true(prev != this);
    current_fiber_ = this;
    // Enter this fiber, handing it |prev| so it can record where prev resumes.
    fcontext_transfer_t t = jump_fcontext(fctx_, prev);
    // Resumed: |t.data| is the fiber that switched back into us, |t.fctx| its
    // continuation point. Save it so we can return to that fiber later.
    auto* resumer = static_cast<FcontextFiber*>(t.data);
    resumer->fctx_ = t.fctx;
  }

  void SetTerminated() override { terminated_ = true; }

 private:
  static void EntryTrampoline(fcontext_transfer_t t) {
    // First entry into a created fiber. |t.data| is the switcher (prev),
    // |t.fctx| its continuation - record it so prev can be resumed.
    auto* prev = static_cast<FcontextFiber*>(t.data);
    prev->fctx_ = t.fctx;

    auto* self = static_cast<FcontextFiber*>(current_fiber_);
    self->entry_();
    self->terminated_ = true;

    // The start routine returned. In normal use the guest scheduler switches
    // away before this point, so this is only a safety net: hand control back
    // to whoever last resumed us. Resuming a terminated fiber is undefined.
    jump_fcontext(prev->fctx_, prev);
  }

  std::function<void()> entry_;
  fcontext_t fctx_ = nullptr;
  FiberStack stack_;
  bool owns_stack_ = false;
  bool terminated_ = false;
};

}  // namespace

std::unique_ptr<Fiber> Fiber::Create(CreationParameters params,
                                     std::function<void()> start_routine) {
  return std::make_unique<FcontextFiber>(params.stack_size,
                                         std::move(start_routine));
}

std::unique_ptr<Fiber> Fiber::CreateFromThread() {
  auto fiber = std::make_unique<FcontextFiber>();
  current_fiber_ = fiber.get();
  return fiber;
}

Fiber* Fiber::GetCurrentFiber() { return current_fiber_; }

}  // namespace threading
}  // namespace xe
