/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/hlsl_shader_translator.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"
#if XE_PLATFORM_WIN32
// DXC (HLSL->DXIL) is part of the Windows-only D3D12 backend.
#include "xenia/gpu/d3d12/dxc_compiler.h"
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace gpu {

void HlslShaderTranslator::EmitMemExportHelpers() {
  // Xbox 360 extended-range float32 -> float16 (exponent 31 is a valid large
  // value, not Inf/NaN). Mirrors DxbcShaderTranslator's f32_to_f16 path.
  Emit(
      R"(uint XeMemExportF16(float v) {
  uint h = f32tof16(v);
  if ((h & 0x7C00u) == 0x7C00u) {
    h = f32tof16(clamp(v, -131008.0, 131008.0) * 0.5) + 0x0400u;
  }
  return h & 0xFFFFu;
}

uint XeMemExportPack(float4 v, uint4 widths, bool num_signed, bool num_integer) {
  v = select(isnan(v), float4(0.0, 0.0, 0.0, 0.0), v);
  uint4 pu;
  if (num_signed) {
    float4 maxv = float4((((int4)1) << max(int4(widths) - 1, 0)) - 1);
    if (num_integer) {
      v = clamp(v, -1.0 - maxv, maxv);
    } else {
      v = clamp(v, -1.0, 1.0);
      v = select(widths > 2u, v * maxv, v);
    }
    v += select(v >= 0.0, float4(0.5, 0.5, 0.5, 0.5),
                float4(-0.5, -0.5, -0.5, -0.5));
    pu = uint4(int4(v));
  } else {
    float4 maxv = float4((((uint4)1) << widths) - 1u);
    if (num_integer) {
      v = clamp(v, 0.0, maxv);
    } else {
      v = saturate(v);
      v = select(widths > 1u, v * maxv, v);
    }
    v += 0.5;
    pu = uint4(v);
  }
  uint4 offsets = uint4(0u, widths.x, widths.x + widths.y,
                        widths.x + widths.y + widths.z);
  uint result = 0u;
  [unroll] for (uint c = 0u; c < 4u; ++c) {
    if (widths[c] != 0u) {
      uint m = (1u << widths[c]) - 1u;
      result |= (pu[c] & m) << offsets[c];
    }
  }
  return result;
}

)");

  // Format conversion switch, with case labels from the ColorFormat enum.
  using CF = xenos::ColorFormat;
  std::string convert =
      "bool XeMemExportConvert(float4 v, uint format, bool num_signed,\n"
      "                        bool num_integer, out uint4 packed,\n"
      "                        out uint size_log2) {\n"
      "  packed = uint4(0u, 0u, 0u, 0u);\n"
      "  size_log2 = 0u;\n"
      "  switch (format) {\n";
  auto gen = [&](std::initializer_list<CF> formats, const char* widths,
                 uint32_t size_log2) {
    for (CF format : formats) {
      convert += "    case " + std::to_string(uint32_t(format)) + "u:\n";
    }
    convert += std::string("      packed.x = XeMemExportPack(v, ") + widths +
               ", num_signed, num_integer);\n      size_log2 = " +
               std::to_string(size_log2) + "u; return true;\n";
  };
  gen({CF::k_8, CF::k_8_A, CF::k_8_B}, "uint4(8u, 0u, 0u, 0u)", 0);
  gen({CF::k_1_5_5_5}, "uint4(5u, 5u, 5u, 1u)", 1);
  gen({CF::k_5_6_5}, "uint4(5u, 6u, 5u, 0u)", 1);
  gen({CF::k_6_5_5}, "uint4(5u, 5u, 6u, 0u)", 1);
  gen({CF::k_8_8_8_8, CF::k_8_8_8_8_A, CF::k_8_8_8_8_AS_16_16_16_16},
      "uint4(8u, 8u, 8u, 8u)", 2);
  gen({CF::k_2_10_10_10, CF::k_2_10_10_10_AS_16_16_16_16},
      "uint4(10u, 10u, 10u, 2u)", 2);
  gen({CF::k_8_8}, "uint4(8u, 8u, 0u, 0u)", 1);
  gen({CF::k_4_4_4_4}, "uint4(4u, 4u, 4u, 4u)", 1);
  gen({CF::k_10_11_11, CF::k_10_11_11_AS_16_16_16_16},
      "uint4(11u, 11u, 10u, 0u)", 2);
  gen({CF::k_11_11_10, CF::k_11_11_10_AS_16_16_16_16},
      "uint4(10u, 11u, 11u, 0u)", 2);
  gen({CF::k_16}, "uint4(16u, 0u, 0u, 0u)", 1);
  gen({CF::k_16_16}, "uint4(16u, 16u, 0u, 0u)", 2);
  // k_16_16_16_16 reuses the 16-bit packer per pair into two dwords.
  convert += "    case " + std::to_string(uint32_t(CF::k_16_16_16_16)) +
             "u:\n"
             "      packed.x = XeMemExportPack(v, uint4(16u, 16u, 0u, 0u), "
             "num_signed, num_integer);\n"
             "      packed.y = XeMemExportPack(float4(v.zw, 0.0, 0.0), "
             "uint4(16u, 16u, 0u, 0u), num_signed, num_integer);\n"
             "      size_log2 = 3u; return true;\n";
  convert += "    case " + std::to_string(uint32_t(CF::k_16_FLOAT)) +
             "u:\n      packed.x = XeMemExportF16(v.x); size_log2 = 1u; return "
             "true;\n";
  convert +=
      "    case " + std::to_string(uint32_t(CF::k_16_16_FLOAT)) +
      "u:\n      packed.x = XeMemExportF16(v.x) | (XeMemExportF16(v.y) << "
      "16u); size_log2 = 2u; return true;\n";
  convert += "    case " + std::to_string(uint32_t(CF::k_16_16_16_16_FLOAT)) +
             "u:\n"
             "      packed.x = XeMemExportF16(v.x) | (XeMemExportF16(v.y) << "
             "16u);\n"
             "      packed.y = XeMemExportF16(v.z) | (XeMemExportF16(v.w) << "
             "16u);\n"
             "      size_log2 = 3u; return true;\n";
  convert += "    case " + std::to_string(uint32_t(CF::k_32_FLOAT)) +
             "u:\n      packed.x = asuint(v.x); size_log2 = 2u; return true;\n";
  convert += "    case " + std::to_string(uint32_t(CF::k_32_32_FLOAT)) +
             "u:\n      packed.xy = asuint(v.xy); size_log2 = 3u; return "
             "true;\n";
  convert += "    case " + std::to_string(uint32_t(CF::k_32_32_32_32_FLOAT)) +
             "u:\n      packed = asuint(v); size_log2 = 4u; return true;\n";
  convert +=
      "    default:\n      size_log2 = 0xFFFFFFFFu; return false;\n  }\n}\n\n";
  Emit(convert);

  // Store a converted element and the full per-invocation flush.
  Emit(
      R"(void XeMemExportStore(uint addr, uint size_log2, uint4 packed) {
  if (size_log2 == 0u) {
    uint shift = (addr & 3u) * 8u;
    uint daddr = addr & ~3u;
    uint original;
    xe_shared_memory_uav.InterlockedAnd(daddr, ~(0xFFu << shift), original);
    xe_shared_memory_uav.InterlockedOr(daddr, (packed.x & 0xFFu) << shift,
                                       original);
  } else if (size_log2 == 1u) {
    uint shift = (addr & 3u) * 8u;
    uint daddr = addr & ~3u;
    uint original;
    xe_shared_memory_uav.InterlockedAnd(daddr, ~(0xFFFFu << shift), original);
    xe_shared_memory_uav.InterlockedOr(daddr, (packed.x & 0xFFFFu) << shift,
                                       original);
  } else if (size_log2 == 2u) {
    xe_shared_memory_uav.Store(addr, packed.x);
  } else if (size_log2 == 3u) {
    xe_shared_memory_uav.Store2(addr, packed.xy);
  } else {
    xe_shared_memory_uav.Store4(addr, packed);
  }
}

void XeExportToMemory(uint4 ea, float4 em[5], uint em_written, bool enabled) {
  if (!enabled) { return; }
  uint4 chk = ea >> uint4(30u, 23u, 23u, 23u);
  if (!(chk.x == 1u && chk.y == 0x96u && chk.z == 0x96u && chk.w == 0x96u)) {
    return;
  }
  uint format = (ea.z >> 8u) & 0x3Fu;
  bool num_signed = ((ea.z >> 16u) & 1u) != 0u;
  bool num_integer = ((ea.z >> 17u) & 1u) != 0u;
  bool rb_swap = ((ea.z >> 19u) & 1u) != 0u;
  uint endian = ea.z & 0x7u;
  uint base_index = ea.y & 0x7FFFFFu;
  uint index_count = ea.w & 0x7FFFFFu;
  uint base_address = (ea.x & 0x3FFFFFFFu) << 2u;
  [unroll] for (uint i = 0u; i < 5u; ++i) {
    if ((em_written & (1u << i)) == 0u) { continue; }
    uint index = base_index + i;
    if (index >= index_count) { continue; }
    float4 v = em[i];
    if (rb_swap) { v.xz = v.zx; }
    uint4 packed;
    uint size_log2;
    if (!XeMemExportConvert(v, format, num_signed, num_integer, packed,
                            size_log2)) {
      continue;
    }
    uint e = endian;
    if (e == 4u) { packed = packed.yxwz; e = 2u; }
    else if (e == 5u) { packed = packed.wzyx; e = 2u; }
    packed.x = XeEndianSwap(packed.x, e);
    packed.y = XeEndianSwap(packed.y, e);
    packed.z = XeEndianSwap(packed.z, e);
    packed.w = XeEndianSwap(packed.w, e);
    XeMemExportStore(base_address + (index << size_log2), size_log2, packed);
  }
}

)");
}

void HlslShaderTranslator::EmitMemExportFlush() {
  EmitLine(
      "XeExportToMemory(asuint(xe_eA), xe_eM, xe_eM_written, "
      "xe_memexport_enabled);");
}

void HlslShaderTranslator::EmitMemExportWrittenMark(
    const InstructionResult& result) {
  if (result.storage_target == InstructionStorageTarget::kExportData &&
      result.GetUsedWriteMask() != 0) {
    EmitLine("xe_eM_written |= " +
             std::to_string(uint32_t(1) << result.storage_index) + "u;");
  }
}
}  // namespace gpu
}  // namespace xe
