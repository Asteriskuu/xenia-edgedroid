/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xmutant.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {

namespace {
// Append |mutant| to the tail of |owner_thread|'s KTHREAD.mutants_list and
// point KMUTANT.owner at the thread.
void InsertMutantOwned(Memory* memory, XThread* owner_thread, XMutant* mutant) {
  uint32_t kthread_addr = owner_thread->guest_object();
  uint32_t kmutant_addr = mutant->guest_object();
  auto* kthread = memory->TranslateVirtual<X_KTHREAD*>(kthread_addr);
  auto* kmutant = memory->TranslateVirtual<X_KMUTANT*>(kmutant_addr);
  uint32_t head_addr = kthread_addr + offsetof(X_KTHREAD, mutants_list);
  uint32_t link_addr = kmutant_addr + offsetof(X_KMUTANT, unk_list);
  uint32_t old_tail = kthread->mutants_list.blink_ptr;
  kmutant->unk_list.flink_ptr = head_addr;
  kmutant->unk_list.blink_ptr = old_tail;
  memory->TranslateVirtual<X_LIST_ENTRY*>(old_tail)->flink_ptr = link_addr;
  kthread->mutants_list.blink_ptr = link_addr;
  kmutant->owner = kthread_addr;
}

// Unlink |mutant| from its owner thread's mutants_list and clear owner.
void RemoveMutantOwned(Memory* memory, XMutant* mutant) {
  uint32_t kmutant_addr = mutant->guest_object();
  auto* kmutant = memory->TranslateVirtual<X_KMUTANT*>(kmutant_addr);
  if (!kmutant->owner) {
    return;
  }
  uint32_t prev = kmutant->unk_list.blink_ptr;
  uint32_t next = kmutant->unk_list.flink_ptr;
  if (prev) {
    memory->TranslateVirtual<X_LIST_ENTRY*>(prev)->flink_ptr = next;
  }
  if (next) {
    memory->TranslateVirtual<X_LIST_ENTRY*>(next)->blink_ptr = prev;
  }
  kmutant->unk_list.flink_ptr = 0;
  kmutant->unk_list.blink_ptr = 0;
  kmutant->owner = 0u;
}
}  // namespace

XMutant::XMutant(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XMutant::XMutant() : XObject(kObjectType) {}

XMutant::~XMutant() = default;

void XMutant::Initialize(bool initial_owner) {
  assert_false(mutant_);

  // Skip on the Restore path -- RestoreObject already populated guest_object_.
  if (guest_object() == 0) {
    CreateNative<X_KMUTANT>();
    auto* kmutant = memory()->TranslateVirtual<X_KMUTANT*>(guest_object());
    // Don't touch header.wait_list: SetNativePointer stashes the handle there.
    kmutant->header.type = 2;  // DISPATCHER_MUTANT
    kmutant->header.signal_state = initial_owner ? 0 : 1;
  }

  mutant_ = xe::threading::Mutant::Create(initial_owner);
  assert_not_null(mutant_);

  if (initial_owner) {
    // Initial-owner acquire doesn't go through Wait, so link the chain here.
    XThread* self = XThread::GetCurrentThread();
    if (self) {
      owning_thread_ = self;
      recursion_count_ = 1;
      InsertMutantOwned(memory(), self, this);
    }
  }
}

void XMutant::InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header) {
  assert_false(mutant_);

  auto* kmutant = reinterpret_cast<X_KMUTANT*>(native_ptr);
  // signal_state: 1 free, 0 held once, negative for recursive holds.
  int32_t signal_state = kmutant->header.signal_state;
  bool initial_owner = signal_state <= 0;
  mutant_ = xe::threading::Mutant::Create(initial_owner);
  assert_not_null(mutant_);
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);

  if (initial_owner) {
    // Recover the owner XThread from the stashed handle in its KTHREAD
    // dispatch header (same trick as GetNativeObject); current thread is the
    // fallback if the stash isn't where we expect.
    // Caveat: if signal_state < 0 we set recursion to match, but the host
    // primitive's count is only 1. Game releases past the first would hit
    // host NOT_OWNED. Would need Wait()-up-recursion to fix.
    recursion_count_ = static_cast<uint32_t>(1 - signal_state);
    XThread* owner = nullptr;
    if (!!kmutant->owner) {
      auto* owner_kthread =
          memory()->TranslateVirtual<X_KTHREAD*>(kmutant->owner.m_ptr);
      if (owner_kthread->header.wait_list.flink_ptr == kXObjSignature) {
        owner = kernel_state()
                    ->object_table()
                    ->LookupObject<XThread>(
                        owner_kthread->header.wait_list.blink_ptr)
                    .get();
      }
    }
    if (!owner) {
      owner = XThread::GetCurrentThread();
    }
    if (owner) {
      owning_thread_ = owner;
      InsertMutantOwned(memory(), owner, this);
    }
  }
}

X_STATUS XMutant::ReleaseMutant(uint32_t priority_increment, bool abandon,
                                bool wait) {
  set_priority_increment(priority_increment);
  // TODO(benvanik): abandoning.
  assert_false(abandon);

  // Update ownership before mutant_->Release(): a waiter that acquires on
  // another thread between the host release and our clear would get stranded
  // with a null owning_thread_.
  if (owning_thread_ == XThread::GetCurrentThread()) {
    --recursion_count_;
    if (recursion_count_ == 0) {
      owning_thread_ = nullptr;
      RemoveMutantOwned(memory(), this);
    }
    auto& signal_state = memory()
                             ->TranslateVirtual<X_KMUTANT*>(guest_object())
                             ->header.signal_state;
    signal_state = signal_state + 1;
  }

  if (mutant_->Release()) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_MUTANT_NOT_OWNED;
  }
}

bool XMutant::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  XThread* owner = owning_thread_.load();
  uint32_t owning_thread_handle = owner ? owner->handle() : 0;
  stream->Write<uint32_t>(owning_thread_handle);
  XELOGD("XMutant {:08X} (owner: {:08X})", handle(), owning_thread_handle);

  return true;
}

object_ref<XMutant> XMutant::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  auto mutant = new XMutant();
  mutant->kernel_state_ = kernel_state;

  if (!mutant->RestoreObject(stream)) {
    delete mutant;
    return nullptr;
  }

  mutant->Initialize(false);

  auto owning_thread_handle = stream->Read<uint32_t>();
  if (owning_thread_handle) {
    // Do NOT pre-set owning_thread_: the queued Wait at thread start needs to
    // hit the first-acquire branch in WaitCallback. Pre-setting it would land
    // in the recursive branch and skip the chain insert.
    auto owner_thread = kernel_state->object_table()->LookupObject<XThread>(
        owning_thread_handle);
    owner_thread->AcquireMutantOnStartup(retain_object(mutant));
  }

  return object_ref<XMutant>(mutant);
}

void XMutant::AbandonAllOwnedByThread(KernelState* ks, XThread* thread) {
  // The host mutex is left to the OS: when |thread|'s host thread exits, it
  // is released with WAIT_ABANDONED which our wait path maps to
  // X_STATUS_ABANDONED_WAIT_0.
  auto* memory = ks->memory();
  uint32_t kthread_addr = thread->guest_object();
  if (!kthread_addr) {
    return;
  }
  auto* kthread = memory->TranslateVirtual<X_KTHREAD*>(kthread_addr);
  uint32_t head_addr = kthread_addr + offsetof(X_KTHREAD, mutants_list);
  uint32_t entry = kthread->mutants_list.flink_ptr;
  while (entry != 0 && entry != head_addr) {
    uint32_t kmutant_addr = entry - offsetof(X_KMUTANT, unk_list);
    auto* kmutant = memory->TranslateVirtual<X_KMUTANT*>(kmutant_addr);
    uint32_t next = kmutant->unk_list.flink_ptr;
    kmutant->abandoned = 1;
    kmutant->owner = 0u;
    kmutant->unk_list.flink_ptr = 0;
    kmutant->unk_list.blink_ptr = 0;
    if (kmutant->header.wait_list.flink_ptr == kXObjSignature) {
      auto obj = ks->object_table()->LookupObject<XMutant>(
          kmutant->header.wait_list.blink_ptr);
      if (obj) {
        obj->owning_thread_ = nullptr;
        obj->recursion_count_ = 0;
      }
    }
    entry = next;
  }
  kthread->mutants_list.flink_ptr = head_addr;
  kthread->mutants_list.blink_ptr = head_addr;
}

void XMutant::WaitCallback() {
  XThread* self = XThread::GetCurrentThread();
  if (!self) {
    return;
  }
  XThread* prev = owning_thread_.exchange(self);
  if (prev != self) {
    recursion_count_ = 1;
    InsertMutantOwned(memory(), self, this);
  } else {
    ++recursion_count_;
  }
  auto& signal_state = memory()
                           ->TranslateVirtual<X_KMUTANT*>(guest_object())
                           ->header.signal_state;
  signal_state = signal_state - 1;
}

}  // namespace kernel
}  // namespace xe
