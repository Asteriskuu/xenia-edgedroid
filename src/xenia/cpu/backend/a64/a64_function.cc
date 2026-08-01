/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <android/log.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
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

  auto addr_in_maps_and_dump = [](uintptr_t addr, char* out_first_bytes,
                                  size_t out_first_bytes_len,
                                  char* out_maps_path) -> bool {
    FILE* mapsf = fopen("/proc/self/maps", "r");
    if (!mapsf) {
      return false;
    }
    FILE* dumpf = fopen("/sdcard/xenia_maps.txt", "a");
    if (dumpf) {
      char mapsbuf[1024];
      while (fgets(mapsbuf, sizeof(mapsbuf), mapsf)) {
        fputs(mapsbuf, dumpf);
      }
      fputc('\n', dumpf);
      fclose(dumpf);
      fclose(mapsf);
      mapsf = fopen("/proc/self/maps", "r");
      if (!mapsf) {
        return false;
      }
    }

    bool found = false;
    char line[512];
    while (fgets(line, sizeof(line), mapsf)) {
      unsigned long long start = 0, end = 0;
      if (sscanf(line, "%llx-%llx", &start, &end) == 2) {
        if (addr >= start && addr < end) {
          found = true;
          FILE* notef = fopen("/sdcard/xenia_maps.txt", "a");
          if (notef) {
            fprintf(notef, "Address %p found in maps range %llx-%llx: %s\n",
                    (void*)addr, start, end, line);
            fclose(notef);
          }
          break;
        }
      }
    }
    fclose(mapsf);

    if (found) {
      uint8_t bytes[16] = {0};
      memcpy(bytes, reinterpret_cast<void*>(addr), sizeof(bytes));
      size_t n = 0;
      for (size_t i = 0; i < sizeof(bytes) && n + 3 < out_first_bytes_len;
           ++i) {
        n += snprintf(out_first_bytes + n, out_first_bytes_len - n, "%02X ",
                      bytes[i]);
      }
      if (out_maps_path) {
        strncpy(out_maps_path, "/sdcard/xenia_maps.txt", 256);
      }
    }
    return found;
  };

  uintptr_t thunk_addr = reinterpret_cast<uintptr_t>(thunk);
  uintptr_t code_addr = reinterpret_cast<uintptr_t>(code);
  char thunk_first_bytes[128] = {0};
  char code_first_bytes[128] = {0};
  char maps_path[256] = {0};
  bool thunk_mapped = false;
  bool code_mapped = false;
  if (thunk_addr) {
    thunk_mapped = addr_in_maps_and_dump(thunk_addr, thunk_first_bytes,
                                         sizeof(thunk_first_bytes), maps_path);
    ALOGI("A64Function: thunk %p mapped=%d first_bytes=%s maps_dump=%s", thunk,
          thunk_mapped, thunk_first_bytes, maps_path[0] ? maps_path : "(none)");
  }
  if (code_addr) {
    code_mapped = addr_in_maps_and_dump(code_addr, code_first_bytes,
                                        sizeof(code_first_bytes), maps_path);
    ALOGI("A64Function: machine_code %p mapped=%d first_bytes=%s maps_dump=%s",
          code, code_mapped, code_first_bytes,
          maps_path[0] ? maps_path : "(none)");
  }

  uintptr_t sp_val = 0;
  asm volatile("mov %0, sp" : "=r"(sp_val));
  ALOGI("A64Function: current SP=%p, aligned16=%d", (void*)sp_val,
        int((sp_val & 0xF) == 0));

  {
    char buf[2048];
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    int n = snprintf(
        buf, sizeof(buf),
        "A64Function::CallImpl dump: %04d-%02d-%02d %02d:%02d:%02d "
        "addr=%08X return_address=%08X backend=%p thunk=%p machine_code=%p\n",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
        tm.tm_sec, address(), return_address, backend, thunk, code);
    if (code) {
      uint32_t* instructions = reinterpret_cast<uint32_t*>(code);
      n += snprintf(buf + n, sizeof(buf) - n, "instructions:");
      for (int i = 0; i < 10; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, " 0x%08X", instructions[i]);
      }
      n += snprintf(buf + n, sizeof(buf) - n, "\n");
    }
    const char* dump_path = "/data/local/tmp/xenia_a64_dump.txt";
    FILE* f = fopen(dump_path, "a");
    if (f) {
      fwrite(buf, 1, (size_t)std::max(0, n), f);
      fflush(f);
      fsync(fileno(f));
      fclose(f);
      ALOGI("A64Function: wrote diagnostic dump to %s", dump_path);
    } else {
      ALOGE("A64Function: failed to open %s for write", dump_path);
    }
  }

  {
    const char* maps_out = "/data/local/tmp/xenia_maps.txt";
    FILE* mapsf = fopen("/proc/self/maps", "r");
    FILE* out = fopen(maps_out, "a");
    if (!out) {
      ALOGE("A64Function: failed to open %s for maps output", maps_out);
    }
    if (mapsf) {
      char line[512];
      bool thunk_found = false;
      bool code_found = false;
      uintptr_t thunk_addr = reinterpret_cast<uintptr_t>(thunk);
      uintptr_t code_addr = reinterpret_cast<uintptr_t>(code);
      if (out) {
        fprintf(out, "=== /proc/self/maps dump at time %ld ===\n",
                time(nullptr));
      }
      while (fgets(line, sizeof(line), mapsf)) {
        if (out) {
          fputs(line, out);
        }
        unsigned long long start = 0, end = 0;
        if (sscanf(line, "%llx-%llx", &start, &end) == 2) {
          if (thunk_addr && thunk_addr >= start && thunk_addr < end) {
            thunk_found = true;
          }
          if (code_addr && code_addr >= start && code_addr < end) {
            code_found = true;
          }
        }
      }
      if (out) {
        fprintf(out, "thunk=%p mapped=%d code=%p mapped=%d\n", thunk,
                thunk_found ? 1 : 0, code, code_found ? 1 : 0);
        fputc('\n', out);
        fflush(out);
        fsync(fileno(out));
      }
      fclose(mapsf);
      if (out) {
        fclose(out);
      }
      ALOGI(
          "A64Function: wrote /proc/self/maps to %s (thunk_mapped=%d "
          "code_mapped=%d)",
          maps_out, thunk_found ? 1 : 0, code_found ? 1 : 0);
    } else {
      ALOGE("A64Function: failed to open /proc/self/maps for reading");
      if (out) {
        fprintf(out, "failed to open /proc/self/maps\n");
        fclose(out);
      }
    }
  }

  if (code && thunk && code == reinterpret_cast<uint8_t*>(thunk)) {
    ALOGE(
        "A64Function::CallImpl: machine_code equals thunk — refusing to call");
    return false;
  }

  if (!thunk || !code) {
    ALOGE("A64Function::CallImpl: missing thunk or machine_code");
    return false;
  }

  if (!thunk_mapped || !code_mapped) {
    ALOGE(
        "A64Function::CallImpl: thunk_mapped=%d code_mapped=%d - refusing to "
        "call",
        thunk_mapped ? 1 : 0, code_mapped ? 1 : 0);
    return false;
  }

  static constexpr uint32_t kForceReturnAddress = 0x9FFF0000u;
  if (return_address == 0xBCBCBCBCu || return_address == 0xCDCDCDCDu ||
      return_address == 0xFFFFFFFFu || return_address == 0u) {
    ALOGW(
        "A64Function::CallImpl: sanitizing invalid return_address=0x%08X -> "
        "0x%08X",
        return_address, kForceReturnAddress);
    return_address = kForceReturnAddress;
  }

  ALOGI("A64Function::CallImpl: jumping to guest code (ret=0x%08X)",
        return_address);
  thunk(code, thread_state->context(),
        reinterpret_cast<void*>(uintptr_t(return_address)));
  ALOGI("A64Function::CallImpl: guest code returned");

  return true;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
