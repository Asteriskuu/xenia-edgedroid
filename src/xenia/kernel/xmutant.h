/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XMUTANT_H_
#define XENIA_KERNEL_XMUTANT_H_

#include <atomic>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class XThread;

class XMutant : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Mutant;

  explicit XMutant(KernelState* kernel_state);
  ~XMutant() override;

  void Initialize(bool initial_owner);
  void InitializeNative(void* native_ptr, X_DISPATCH_HEADER* header);

  X_STATUS ReleaseMutant(uint32_t priority_increment, bool abandon, bool wait);

  bool Save(ByteStream* stream) override;
  static object_ref<XMutant> Restore(KernelState* kernel_state,
                                     ByteStream* stream);

  // Mark every mutant in |thread|'s mutants_list abandoned and unlink it.
  // Called from XThread::Exit/Terminate.
  static void AbandonAllOwnedByThread(KernelState* kernel_state,
                                      XThread* thread);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return mutant_.get(); }
  void WaitCallback() override;

 private:
  XMutant();

  std::unique_ptr<xe::threading::Mutant> mutant_;
  std::atomic<XThread*> owning_thread_{nullptr};
  // Mutant::Release() doesn't report count-to-zero, so we track recursion
  // ourselves. Only the current owner mutates this -- no synchronization.
  uint32_t recursion_count_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XMUTANT_H_
