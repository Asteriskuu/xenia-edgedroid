/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <android/log.h>
#include <stdio.h>
#include <time.h>
#include <algorithm>

#include "xenia/cpu/backend/a64/a64_function.h"

#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

#define ALOGI(...) \
  __android_log_print(ANDROID_LOG_INFO, "XeniaAndroid", __VA_ARGS__)
#define ALOGW(...) \
  __android_log_print(ANDROID_LOG_WARN, "XeniaAndroid", __VA_ARGS__)
#define ALOGE(...) \
  __android_log_print(ANDROID_LOG_ERROR, "XeniaAndroid", __VA_ARGS__)

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

A64Function::A64Function(Module* module, uint32_t address)
    : GuestFunction(module, address) {}

A64Function::~A64Function() {
  // machine_code_ is freed by code cache.
}

void A64Function::Setup(uint8_t* machine_code, size_t machine_code_length) {
  machine_code_length_.store(machine_code_length, std::memory_order_relaxed);
  machine_code_.store(machine_code, std::memory_order_release);
}

bool A64Function::CallImpl(ThreadState* thread_state, uint32_t return_address) {
  ALOGI("A64Function::CallImpl: entered address=%08X return_address=%08X",
        address(), return_address);

  auto backend =
      reinterpret_cast<A64Backend*>(thread_state->processor()->backend());
  ALOGI("A64Function::CallImpl: backend=%p", backend);

  auto thunk = backend ? backend->host_to_guest_thunk() : nullptr;
  ALOGI("A64Function::CallImpl: thunk=%p", thunk);

  auto* code = machine_code_.load(std::memory_order_acquire);
  ALOGI("A64Function::CallImpl: machine_code=%p", code);

  if (code) {
    ALOGI("A64Function: code address=%p", code);

    uint32_t* instructions = reinterpret_cast<uint32_t*>(code);
    for (int i = 0; i < 10; ++i) {
      ALOGI("A64Function: instruction[%d]=0x%08X", i, instructions[i]);
    }
  }

  {
    char buf[1024];
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    int n = snprintf(buf, sizeof(buf),
                     "A64Function::CallImpl dump: %04d-%02d-%02d %02d:%02d:%02d "
                     "addr=%08X return_address=%08X backend=%p thunk=%p machine_code=%p\n",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, address(), return_address, backend,
                     thunk, code);
    if (code) {
      uint32_t* instructions = reinterpret_cast<uint32_t*>(code);
      n += snprintf(buf + n, sizeof(buf) - n, "instructions:");
      for (int i = 0; i < 10; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, " 0x%08X", instructions[i]);
      }
      n += snprintf(buf + n, sizeof(buf) - n, "\n");
    }
    const char* paths[] = {"./xenia_a64_dump.txt",
                           "/data/local/tmp/xenia_a64_dump.txt",
                           "/sdcard/xenia_a64_dump.txt",
                           nullptr};
    for (const char** p = paths; *p; ++p) {
      FILE* f = fopen(*p, "a");
      if (f) {
        fwrite(buf, 1, (size_t)std::max(0, n), f);
        fflush(f);
        fclose(f);
        break;
      }
    }
  }

  if (code && thunk && code == reinterpret_cast<uint8_t*>(thunk)) {
    ALOGE("A64Function::CallImpl: machine_code equals thunk — refusing to call");
    return false;
  }

  if (!thunk || !code) {
    ALOGE("A64Function::CallImpl: missing thunk or machine_code");
    return false;
  }

  ALOGI("A64Function::CallImpl: jumping to guest code");
  thunk(code, thread_state->context(),
        reinterpret_cast<void*>(uintptr_t(return_address)));
  ALOGI("A64Function::CallImpl: guest code returned");

  return true;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
