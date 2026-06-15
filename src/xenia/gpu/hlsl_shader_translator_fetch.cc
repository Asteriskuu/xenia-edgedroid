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

DECLARE_bool(draw_resolution_scaled_texture_offsets);

namespace xe {
namespace gpu {

uint32_t HlslShaderTranslator::FindOrAddTextureBinding(
    uint32_t fetch_constant, xenos::FetchOpDimension dimension,
    bool is_signed) {
  // Search for existing binding.
  for (uint32_t i = 0; i < uint32_t(texture_bindings_.size()); ++i) {
    const TextureBinding& binding = texture_bindings_[i];
    if (binding.fetch_constant == fetch_constant &&
        binding.dimension == dimension && binding.is_signed == is_signed) {
      return i;
    }
  }
  // Create new binding.
  // NOTE: Calculate bindless_descriptor_index BEFORE emplace_back so indices
  // start at 0, not 1. This ensures the descriptor_indices buffer (which is
  // sized based on binding count) has enough space for all indices.
  uint32_t index = uint32_t(texture_bindings_.size());
  uint32_t bindless_index =
      bindless_resources_used_ ? GetBindlessResourceCount() : 0;
  TextureBinding& new_binding = texture_bindings_.emplace_back();
  new_binding.bindful_srv_index = index;
  new_binding.bindless_descriptor_index = bindless_index;
  new_binding.fetch_constant = fetch_constant;
  new_binding.dimension = dimension;
  new_binding.is_signed = is_signed;
  return index;
}

uint32_t HlslShaderTranslator::FindOrAddSamplerBinding(
    uint32_t fetch_constant, xenos::TextureFilter mag_filter,
    xenos::TextureFilter min_filter, xenos::TextureFilter mip_filter,
    xenos::AnisoFilter aniso_filter) {
  // In D3D12, anisotropic filtering implies linear filtering.
  if (aniso_filter != xenos::AnisoFilter::kDisabled &&
      aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    mag_filter = xenos::TextureFilter::kLinear;
    min_filter = xenos::TextureFilter::kLinear;
    mip_filter = xenos::TextureFilter::kLinear;
    aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  }
  // Search for existing binding.
  for (uint32_t i = 0; i < uint32_t(sampler_bindings_.size()); ++i) {
    const SamplerBinding& binding = sampler_bindings_[i];
    if (binding.fetch_constant == fetch_constant &&
        binding.mag_filter == mag_filter && binding.min_filter == min_filter &&
        binding.mip_filter == mip_filter &&
        binding.aniso_filter == aniso_filter) {
      return i;
    }
  }
  // Create new binding.
  // NOTE: Calculate bindless_descriptor_index BEFORE emplace_back so indices
  // start at 0, not 1. This ensures the descriptor_indices buffer (which is
  // sized based on binding count) has enough space for all indices.
  uint32_t bindless_index =
      bindless_resources_used_ ? GetBindlessResourceCount() : 0;
  SamplerBinding& new_binding = sampler_bindings_.emplace_back();
  new_binding.bindless_descriptor_index = bindless_index;
  new_binding.fetch_constant = fetch_constant;
  new_binding.mag_filter = mag_filter;
  new_binding.min_filter = min_filter;
  new_binding.mip_filter = mip_filter;
  new_binding.aniso_filter = aniso_filter;
  return uint32_t(sampler_bindings_.size()) - 1;
}

void HlslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  if (!used_result_components && instr.is_mini_fetch) {
    // Nothing to load, but a constant-only dest swizzle still emits its 0/1.
    EmitVectorResultAssignment(instr.result, "float4(0.0, 0.0, 0.0, 0.0)");
    StoreConstantComponents(instr.result);
    return;
  }

  // Get fetch constant index from operand 1.
  uint32_t fetch_constant_index = instr.operands[1].storage_index;

  // Load the fetch constant (vf# - vertex fetch constant).
  // Fetch constants are packed 2 per uint4 in xe_fetch_constants_data:
  //   vf0 -> [0].xy, vf1 -> [0].zw, vf2 -> [1].xy, vf3 -> [1].zw, etc.
  // Each fetch constant is 2 dwords: word0 (base address), word1
  // (endian/flags).
  uint32_t fetch_uint4_index = fetch_constant_index / 2;
  bool fetch_use_zw = (fetch_constant_index % 2) != 0;
  std::string fetch_comp0 = fetch_use_zw ? "z" : "x";
  std::string fetch_comp1 = fetch_use_zw ? "w" : "y";

  // Gate a predicated fetch, like the ALU path. A (p0) vfetch in a
  // non-predicated exec block must guard its own body.
  if (instr.is_predicated) {
    EmitLine("if (xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
  } else {
    EmitLine("{");
  }
  Indent();

  // Load fetch constant words.
  EmitLine("uint xe_vf_word0 = xe_fetch_constants_data[" +
           std::to_string(fetch_uint4_index) + "]." + fetch_comp0 + ";");
  EmitLine("uint xe_vf_word1 = xe_fetch_constants_data[" +
           std::to_string(fetch_uint4_index) + "]." + fetch_comp1 + ";");

  // Extract base address (bits [2:31] shifted right by 2 = byte address).
  EmitLine("uint xe_vf_base_addr = (xe_vf_word0 & 0xFFFFFFFCu);");

  // Extract endianness from word1 bits [0:1].
  EmitLine("uint xe_vf_endian = xe_vf_word1 & 0x3u;");

  // Get vertex index from operand 0.
  if (!instr.is_mini_fetch) {
    std::string index_operand = OperandToHlsl(instr.operands[0], 1);
    if (instr.attributes.is_index_rounded) {
      EmitLine("int xe_vf_index = int(floor(" + index_operand + " + 0.5));");
    } else if (cvars::ac6_ground_fix) {
      // UGLY HACK matching DxbcShaderTranslator. Remove ASAP. A proper fix
      // needs an accurate RCP implementation.
      EmitLine("int xe_vf_index = int(floor(" + index_operand +
               " + 0.00025));");
    } else {
      EmitLine("int xe_vf_index = int(floor(" + index_operand + "));");
    }
    // Stride is in DWORDs (4-byte units), convert to bytes by multiplying by 4.
    // Store the vertex's base address for subsequent mini-fetches.
    EmitLine("xe_vfetch_address = xe_vf_base_addr + uint(xe_vf_index) * " +
             std::to_string(instr.attributes.stride * sizeof(uint32_t)) + "u;");
    EmitLine("uint xe_vf_byte_addr = xe_vfetch_address + " +
             std::to_string(instr.attributes.offset * sizeof(uint32_t)) + "u;");
  } else {
    // Mini fetch uses address from previous full fetch + this fetch's offset.
    // Offset is in DWORDs (4-byte units), convert to bytes.
    EmitLine("uint xe_vf_byte_addr = xe_vfetch_address + " +
             std::to_string(instr.attributes.offset * sizeof(uint32_t)) + "u;");
  }

  // Load data from shared memory based on format.
  xenos::VertexFormat format = instr.attributes.data_format;
  uint32_t needed_words = 0;
  bool known_format = true;
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      needed_words =
          xenos::GetVertexFormatNeededWords(format, used_result_components);
      break;
    default:
      known_format = false;
      break;
  }
  if (known_format && !needed_words) {
    EmitVectorResultAssignment(instr.result, "float4(0.0, 0.0, 0.0, 0.0)");
    StoreConstantComponents(instr.result);
    Outdent();
    EmitLine("}");
    return;
  }

  auto make_signed_int = [](const std::string& value_expr,
                            uint32_t bits) -> std::string {
    if (bits == 32) {
      return "asint(" + value_expr + ")";
    }
    return "XeSignExtend(" + value_expr + ", " + std::to_string(bits) + "u)";
  };
  auto make_integer_component = [&](const std::string& value_expr,
                                    uint32_t bits) -> std::string {
    if (instr.attributes.is_signed) {
      return "float(" + make_signed_int(value_expr, bits) + ")";
    }
    return "float(" + value_expr + ")";
  };
  auto make_normalized_component = [&](const std::string& value_expr,
                                       uint32_t bits) -> std::string {
    if (instr.attributes.is_signed) {
      const std::string signed_value = make_signed_int(value_expr, bits);
      if (instr.attributes.signed_rf_mode ==
          xenos::SignedRepeatingFractionMode::kNoZero) {
        return "XeNormalizeSignedNoZero(" + signed_value + ", " +
               std::to_string(bits) + "u)";
      }
      return "XeNormalizeSignedZeroClampMinusOne(" + signed_value + ", " +
             std::to_string(bits) + "u)";
    }
    return "XeNormalizeUnsigned(" + value_expr + ", " + std::to_string(bits) +
           "u)";
  };
  auto make_fixed_component = [&](const std::string& value_expr,
                                  uint32_t bits) -> std::string {
    return instr.attributes.is_integer
               ? make_integer_component(value_expr, bits)
               : make_normalized_component(value_expr, bits);
  };
  auto make_packed_component = [&](const std::string& packed_expr,
                                   uint32_t shift, uint32_t mask,
                                   uint32_t bits) -> std::string {
    std::string value_expr;
    if (shift) {
      value_expr = "((" + packed_expr + " >> " + std::to_string(shift) +
                   "u) & 0x" + fmt::format("{:X}", mask) + "u)";
    } else {
      value_expr =
          "(" + packed_expr + " & 0x" + fmt::format("{:X}", mask) + "u)";
    }
    return make_fixed_component(value_expr, bits);
  };

  std::string result_value;

  switch (format) {
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      EmitLine("uint4 xe_vf_raw = XeSharedMemoryLoad4(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      EmitLine("xe_vf_raw.z = XeEndianSwap(xe_vf_raw.z, xe_vf_endian);");
      EmitLine("xe_vf_raw.w = XeEndianSwap(xe_vf_raw.w, xe_vf_endian);");
      result_value = "asfloat(xe_vf_raw)";
      break;

    case xenos::VertexFormat::k_32_32_32_FLOAT:
      EmitLine("uint3 xe_vf_raw = XeSharedMemoryLoad3(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      EmitLine("xe_vf_raw.z = XeEndianSwap(xe_vf_raw.z, xe_vf_endian);");
      result_value = "float4(asfloat(xe_vf_raw), 0.0)";
      break;

    case xenos::VertexFormat::k_32_32_FLOAT:
      EmitLine("uint2 xe_vf_raw = XeSharedMemoryLoad2(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      result_value = "float4(asfloat(xe_vf_raw), 0.0, 0.0)";
      break;

    case xenos::VertexFormat::k_32_FLOAT:
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      result_value = "float4(asfloat(xe_vf_raw), 0.0, 0.0, 0.0)";
      break;

    case xenos::VertexFormat::k_8_8_8_8:
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      result_value = "float4(" +
                     make_packed_component("xe_vf_raw", 0, 0xFF, 8) + ", " +
                     make_packed_component("xe_vf_raw", 8, 0xFF, 8) + ", " +
                     make_packed_component("xe_vf_raw", 16, 0xFF, 8) + ", " +
                     make_packed_component("xe_vf_raw", 24, 0xFF, 8) + ")";
      break;

    case xenos::VertexFormat::k_2_10_10_10:
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      result_value = "float4(" +
                     make_packed_component("xe_vf_raw", 0, 0x3FF, 10) + ", " +
                     make_packed_component("xe_vf_raw", 10, 0x3FF, 10) + ", " +
                     make_packed_component("xe_vf_raw", 20, 0x3FF, 10) + ", " +
                     make_packed_component("xe_vf_raw", 30, 0x3, 2) + ")";
      break;

    case xenos::VertexFormat::k_10_11_11: {
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      result_value =
          "float4(" + make_packed_component("xe_vf_raw", 0, 0x7FF, 11) + ", " +
          make_packed_component("xe_vf_raw", 11, 0x7FF, 11) + ", " +
          make_packed_component("xe_vf_raw", 22, 0x3FF, 10) + ", 0.0)";
      break;
    }

    case xenos::VertexFormat::k_11_11_10: {
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      result_value =
          "float4(" + make_packed_component("xe_vf_raw", 0, 0x3FF, 10) + ", " +
          make_packed_component("xe_vf_raw", 10, 0x7FF, 11) + ", " +
          make_packed_component("xe_vf_raw", 21, 0x7FF, 11) + ", 0.0)";
      break;
    }

    case xenos::VertexFormat::k_16_16: {
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      result_value =
          "float4(" + make_packed_component("xe_vf_raw", 0, 0xFFFF, 16) + ", " +
          make_packed_component("xe_vf_raw", 16, 0xFFFF, 16) + ", 0.0, 0.0)";
      break;
    }

    case xenos::VertexFormat::k_16_16_16_16: {
      EmitLine("uint2 xe_vf_raw = XeSharedMemoryLoad2(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      result_value =
          "float4(" + make_packed_component("xe_vf_raw.x", 0, 0xFFFF, 16) +
          ", " + make_packed_component("xe_vf_raw.x", 16, 0xFFFF, 16) + ", " +
          make_packed_component("xe_vf_raw.y", 0, 0xFFFF, 16) + ", " +
          make_packed_component("xe_vf_raw.y", 16, 0xFFFF, 16) + ")";
    } break;

    case xenos::VertexFormat::k_16_16_FLOAT: {
      EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
      EmitLine("float2 xe_vf_xy = XeUnpackFloat16x2(xe_vf_raw);");
      result_value = "float4(xe_vf_xy.x, xe_vf_xy.y, 0.0, 0.0)";
    } break;

    case xenos::VertexFormat::k_16_16_16_16_FLOAT: {
      EmitLine("uint2 xe_vf_raw = XeSharedMemoryLoad2(xe_vf_byte_addr);");
      EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
      EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
      EmitLine("float2 xe_vf_xy = XeUnpackFloat16x2(xe_vf_raw.x);");
      EmitLine("float2 xe_vf_zw = XeUnpackFloat16x2(xe_vf_raw.y);");
      result_value = "float4(xe_vf_xy.x, xe_vf_xy.y, xe_vf_zw.x, xe_vf_zw.y)";
    } break;

    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32: {
      // Integer formats.
      uint32_t component_count = 1;
      if (format == xenos::VertexFormat::k_32_32) {
        component_count = 2;
      } else if (format == xenos::VertexFormat::k_32_32_32_32) {
        component_count = 4;
      }

      if (component_count == 1) {
        EmitLine("uint xe_vf_raw = XeSharedMemoryLoad(xe_vf_byte_addr);");
        EmitLine("xe_vf_raw = XeEndianSwap(xe_vf_raw, xe_vf_endian);");
        result_value = "float4(" + make_fixed_component("xe_vf_raw", 32) +
                       ", 0.0, 0.0, 0.0)";
      } else if (component_count == 2) {
        EmitLine("uint2 xe_vf_raw = XeSharedMemoryLoad2(xe_vf_byte_addr);");
        EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
        EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
        result_value = "float4(" + make_fixed_component("xe_vf_raw.x", 32) +
                       ", " + make_fixed_component("xe_vf_raw.y", 32) +
                       ", 0.0, 0.0)";
      } else {
        EmitLine("uint4 xe_vf_raw = XeSharedMemoryLoad4(xe_vf_byte_addr);");
        EmitLine("xe_vf_raw.x = XeEndianSwap(xe_vf_raw.x, xe_vf_endian);");
        EmitLine("xe_vf_raw.y = XeEndianSwap(xe_vf_raw.y, xe_vf_endian);");
        EmitLine("xe_vf_raw.z = XeEndianSwap(xe_vf_raw.z, xe_vf_endian);");
        EmitLine("xe_vf_raw.w = XeEndianSwap(xe_vf_raw.w, xe_vf_endian);");
        result_value = "float4(" + make_fixed_component("xe_vf_raw.x", 32) +
                       ", " + make_fixed_component("xe_vf_raw.y", 32) + ", " +
                       make_fixed_component("xe_vf_raw.z", 32) + ", " +
                       make_fixed_component("xe_vf_raw.w", 32) + ")";
      }
    } break;

    default:
      // Unsupported format - return zeros.
      XELOGW("HLSL: Unsupported vertex format: {}",
             static_cast<uint32_t>(format));
      result_value = "float4(0.0, 0.0, 0.0, 0.0)";
      break;
  }

  // Apply exponent bias if needed.
  if (instr.attributes.exp_adjust != 0) {
    float exp_adjust_multiplier = std::ldexp(1.0f, instr.attributes.exp_adjust);
    EmitLine("float4 xe_vf_result = " + result_value + " * " +
             HlslFloatLiteral(exp_adjust_multiplier) + ";");
    result_value = "xe_vf_result";
  }

  // Store result with proper swizzle matching.
  EmitVectorResultAssignment(instr.result, result_value);

  // Store any constant components (k0 or k1) in the write mask.
  // For example, r1.xy1_ means Z should be constant 1.0, not from the fetch.
  StoreConstantComponents(instr.result);

  Outdent();
  EmitLine("}");
}

void HlslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  using FetchOpcode = ucode::FetchOpcode;

  switch (instr.opcode) {
    case FetchOpcode::kTextureFetch:
    case FetchOpcode::kGetTextureBorderColorFrac:
    case FetchOpcode::kGetTextureComputedLod:
    case FetchOpcode::kGetTextureGradients:
    case FetchOpcode::kGetTextureWeights:
      break;
    case FetchOpcode::kSetTextureLod:
      EmitLine("xe_texture_lod = " + OperandToHlsl(instr.operands[0], 1) + ";");
      return;
    case FetchOpcode::kSetTextureGradientsHorz:
      EmitLine("xe_texture_grad_h = " + OperandToHlsl(instr.operands[0], 3) +
               ";");
      return;
    case FetchOpcode::kSetTextureGradientsVert:
      EmitLine("xe_texture_grad_v = " + OperandToHlsl(instr.operands[0], 3) +
               ";");
      return;
    default:
      XELOGW("HLSL: Unhandled texture fetch opcode: {}", instr.opcode_name);
      return;
  }

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  if (!used_result_components) {
    // A constant-only dest swizzle (e.g. .xyz1) still has to emit its 0/1
    // components even though no texel is sampled.
    StoreConstantComponents(instr.result);
    return;
  }

  // Get texture and sampler indices from fetch constant.
  uint32_t fetch_constant_index = instr.operands[1].storage_index;

  // Get coordinates from source operand.
  std::string coords = "(" + OperandToHlsl(instr.operands[0], 4) + ")";

  // Gate a predicated fetch, like the ALU path. A (p0) tfetch in a
  // non-predicated exec block must guard its own body.
  if (instr.is_predicated) {
    EmitLine("if (xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
  } else {
    EmitLine("{");
  }
  Indent();

  const std::string fetch_constant_literal =
      std::to_string(fetch_constant_index) + "u";
  const std::string instruction_lod_bias =
      HlslFloatLiteral(instr.attributes.lod_bias);
  const bool get_texture_weights =
      instr.opcode == FetchOpcode::kGetTextureWeights;
  const bool get_border_color_frac =
      instr.opcode == FetchOpcode::kGetTextureBorderColorFrac;
  // getWeights and getBCF both need the bilinear footprint centered on the
  // texel grid (the -0.5 texel shift), not the raw sampling coordinate.
  const bool needs_bilinear_footprint =
      get_texture_weights || get_border_color_frac;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float offset_z = 0.0f;
  if (instr.opcode != FetchOpcode::kGetTextureComputedLod) {
    constexpr float rounding_offset = 1.5f / 1024.0f;
    offset_x = instr.attributes.offset_x + rounding_offset;
    offset_y = instr.attributes.offset_y + rounding_offset;
    offset_z = instr.attributes.offset_z + rounding_offset;
    if (instr.dimension == xenos::FetchOpDimension::kCube) {
      // Cube SC/TC get the same texel-center ambiguity fixup as 2D, but the
      // face index is a discrete value, not a texture coordinate.
      offset_z = needs_bilinear_footprint ? 0.0f : instr.attributes.offset_z;
    }
    if (needs_bilinear_footprint) {
      offset_x -= 0.5f;
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          offset_y -= 0.5f;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          offset_y -= 0.5f;
          offset_z -= 0.5f;
          break;
        default:
          break;
      }
    }
  }
  const std::string offset_x_literal = HlslFloatLiteral(offset_x);
  const std::string offset_y_literal = HlslFloatLiteral(offset_y);
  const std::string offset_z_literal = HlslFloatLiteral(offset_z);
  std::string result_value = "xe_tf_result";
  EmitLine("float4 xe_tf_result = float4(0.0, 0.0, 0.0, 0.0);");
  EmitLine("float3 xe_tf_weight_coord = float3(0.0, 0.0, 0.0);");
  if (instr.opcode == FetchOpcode::kGetTextureComputedLod &&
      (!TextureFetchUsesComputedLod(instr) ||
       instr.attributes.use_register_gradients)) {
    XELOGW(
        "HLSL: getCompTexLOD used with explicit LOD/gradients or outside "
        "pixel shader; returning zero");
    EmitVectorResultAssignment(instr.result, result_value);
    StoreConstantComponents(instr.result);
    Outdent();
    EmitLine("}");
    return;
  }

  // Resolution-scaled render-to-texture offset correction: when this texture is
  // itself a resolution-scaled render target, a guest-texel sampling offset is
  // divided by the resolution scale per scaled axis so it lands on the host's
  // finer texel grid (matches DxbcShaderTranslator and the
  // draw_resolution_scaled_texture_offsets cvar). Applied to the normalized
  // sampling coordinate only, never to the guest-space weight coordinate.
  std::string off_scale_xy;  // "" or " * xe_tf_off_scale"
  std::string off_scale_x;   // "" or " * xe_tf_off_scale.x"
  if (cvars::draw_resolution_scaled_texture_offsets &&
      !instr.attributes.unnormalized_coordinates &&
      (draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1)) {
    EmitLine("float2 xe_tf_off_scale = ((xe_textures_resolution_scaled & " +
             std::to_string(uint32_t(1) << fetch_constant_index) +
             "u) != 0u) ? float2(" +
             HlslFloatLiteral(draw_resolution_scale_x_ > 1
                                  ? 1.0f / float(draw_resolution_scale_x_)
                                  : 1.0f) +
             ", " +
             HlslFloatLiteral(draw_resolution_scale_y_ > 1
                                  ? 1.0f / float(draw_resolution_scale_y_)
                                  : 1.0f) +
             ") : float2(1.0, 1.0);");
    off_scale_xy = " * xe_tf_off_scale";
    off_scale_x = " * xe_tf_off_scale.x";
  }

  switch (instr.dimension) {
    case xenos::FetchOpDimension::k1D: {
      EmitLine("float xe_tf_width = XeGetTextureFetchSize1D(" +
               fetch_constant_literal + ");");
      if (instr.attributes.unnormalized_coordinates) {
        EmitLine("xe_tf_weight_coord.x = " + coords + ".x + " +
                 offset_x_literal + ";");
        EmitLine(
            "float2 xe_tf_uv = float2(xe_tf_weight_coord.x / "
            "xe_tf_width, 0.0);");
      } else {
        EmitLine("xe_tf_weight_coord.x = " + coords + ".x * xe_tf_width + " +
                 offset_x_literal + ";");
        EmitLine("float2 xe_tf_uv = float2(" + coords + ".x + (" +
                 offset_x_literal + off_scale_x + " / xe_tf_width), 0.0);");
      }
      EmitLine("if (XeGetTextureFetchDimension(" + fetch_constant_literal +
               ") == 0u && uint(xe_tf_width - 1.0f) >= 8192u) {");
      Indent();
      EmitLine("float xe_tf_row_width = 8192.0f;");
      EmitLine(
          "float xe_tf_row = floor(xe_tf_weight_coord.x / "
          "xe_tf_row_width);");
      EmitLine("float xe_tf_rows = ceil(xe_tf_width / xe_tf_row_width);");
      EmitLine("xe_tf_uv.x = frac(xe_tf_weight_coord.x / xe_tf_row_width);");
      EmitLine("xe_tf_uv.y = xe_tf_row / xe_tf_rows;");
      Outdent();
      EmitLine("}");
    } break;

    case xenos::FetchOpDimension::k2D: {
      EmitLine("float2 xe_tf_size = XeGetTextureFetchSize2D(" +
               fetch_constant_literal + ");");
      if (instr.attributes.unnormalized_coordinates) {
        EmitLine("xe_tf_weight_coord.xy = " + coords + ".xy + float2(" +
                 offset_x_literal + ", " + offset_y_literal + ");");
        EmitLine("float2 xe_tf_uv = xe_tf_weight_coord.xy / xe_tf_size;");
      } else {
        EmitLine("xe_tf_weight_coord.xy = " + coords +
                 ".xy * xe_tf_size + float2(" + offset_x_literal + ", " +
                 offset_y_literal + ");");
        EmitLine("float2 xe_tf_uv = " + coords + ".xy + (float2(" +
                 offset_x_literal + ", " + offset_y_literal + ")" +
                 off_scale_xy + ") / xe_tf_size;");
      }
    } break;

    case xenos::FetchOpDimension::k3DOrStacked: {
      EmitLine("float3 xe_tf_size = XeGetTextureFetchSize3DOrStacked(" +
               fetch_constant_literal + ");");
      EmitLine("bool xe_tf_is_3d = XeTextureFetchConstantIs3D(" +
               fetch_constant_literal + ");");
      if (instr.attributes.unnormalized_coordinates) {
        EmitLine("xe_tf_weight_coord = " + coords + ".xyz + float3(" +
                 offset_x_literal + ", " + offset_y_literal + ", " +
                 offset_z_literal + ");");
        EmitLine("float3 xe_tf_uvw = xe_tf_weight_coord / xe_tf_size;");
        EmitLine("if (!xe_tf_is_3d) {");
        Indent();
        EmitLine("xe_tf_uvw.z = xe_tf_weight_coord.z;");
        Outdent();
        EmitLine("}");
      } else {
        EmitLine("xe_tf_weight_coord = " + coords +
                 ".xyz * xe_tf_size + float3(" + offset_x_literal + ", " +
                 offset_y_literal + ", " + offset_z_literal + ");");
        EmitLine("float3 xe_tf_uvw;");
        EmitLine("xe_tf_uvw.xy = " + coords + ".xy + (float2(" +
                 offset_x_literal + ", " + offset_y_literal + ")" +
                 off_scale_xy + ") / xe_tf_size.xy;");
        EmitLine("xe_tf_uvw.z = xe_tf_is_3d ? (" + coords + ".z + (" +
                 offset_z_literal + " / xe_tf_size.z)) : (" + coords +
                 ".z * xe_tf_size.z + " + offset_z_literal + ");");
      }
    } break;

    case xenos::FetchOpDimension::kCube: {
      EmitLine("float2 xe_tf_size = XeGetTextureFetchSize2D(" +
               fetch_constant_literal + ");");
      if (instr.attributes.unnormalized_coordinates) {
        EmitLine("xe_tf_weight_coord.xy = " + coords + ".xy + float2(" +
                 offset_x_literal + ", " + offset_y_literal + ");");
        EmitLine(
            "float3 xe_tf_cube_coord = float3("
            "xe_tf_weight_coord.xy / xe_tf_size, " +
            coords + ".z + " + offset_z_literal + ");");
      } else {
        EmitLine("xe_tf_weight_coord.xy = " + coords +
                 ".xy * xe_tf_size + float2(" + offset_x_literal + ", " +
                 offset_y_literal + ");");
        EmitLine("float3 xe_tf_cube_coord = float3(" + coords +
                 ".xy + (float2(" + offset_x_literal + ", " + offset_y_literal +
                 ")" + off_scale_xy + ") / xe_tf_size, " + coords + ".z + " +
                 offset_z_literal + ");");
      }
      EmitLine("xe_tf_weight_coord.z = " + coords + ".z + " + offset_z_literal +
               ";");
      EmitLine("float3 xe_tf_dir = XeTextureCubeDirection(xe_tf_cube_coord);");
    } break;

    default:
      XELOGW("HLSL: Unknown texture dimension: {}",
             static_cast<uint32_t>(instr.dimension));
      EmitLine("xe_tf_result = float4(1.0, 0.0, 1.0, 1.0);");
      break;
  }

  if (instr.opcode == FetchOpcode::kGetTextureGradients) {
    if (is_pixel_shader()) {
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
        case xenos::FetchOpDimension::k2D:
          EmitLine("xe_tf_result = float4(ddx_coarse(" + coords +
                   ".xy), ddy_coarse(" + coords + ".xy));");
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          EmitLine("xe_tf_result = float4(ddx_coarse(" + coords +
                   ".xy), ddy_coarse(" + coords + ".xy));");
          break;
        case xenos::FetchOpDimension::kCube:
          EmitLine("xe_tf_result = float4(ddx_coarse(" + coords +
                   ".xy), ddy_coarse(" + coords + ".xy));");
          break;
        default:
          break;
      }
    }
  } else if (instr.opcode == FetchOpcode::kGetTextureWeights) {
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        EmitLine(
            "xe_tf_result = float4(frac(xe_tf_weight_coord.xy), 0.0, "
            "0.0);");
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        EmitLine("xe_tf_result = float4(frac(xe_tf_weight_coord), 0.0);");
        break;
      case xenos::FetchOpDimension::kCube:
        EmitLine(
            "xe_tf_result = float4(frac(xe_tf_weight_coord.xy), 0.0, "
            "0.0);");
        break;
      default:
        break;
    }
  } else if (get_border_color_frac) {
    // Border color fraction (in .x): the bilinear-weighted share of the sample
    // footprint that lands outside the texture on axes using a border clamp
    // mode (6/7). Mips are ignored (LOD 0), matching the getWeights
    // simplification. xe_tf_weight_coord is the footprint center in texels.
    EmitLine("uint xe_tf_clamp = XeGetTextureFetchConstantWord(" +
             fetch_constant_literal + ", 0u);");
    EmitLine("bool xe_tf_bx = ((xe_tf_clamp >> 10u) & 6u) == 6u;");
    EmitLine("bool xe_tf_by = ((xe_tf_clamp >> 13u) & 6u) == 6u;");
    EmitLine("bool xe_tf_bz = ((xe_tf_clamp >> 16u) & 6u) == 6u;");
    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D: {
        EmitLine("float xe_tf_w = XeGetTextureFetchSize1D(" +
                 fetch_constant_literal + ");");
        EmitLine("float xe_tf_f = frac(xe_tf_weight_coord.x);");
        EmitLine("float xe_tf_i0 = floor(xe_tf_weight_coord.x);");
        EmitLine(
            "bool xe_tf_x0 = xe_tf_bx && (xe_tf_i0 < 0.0 || xe_tf_i0 >= "
            "xe_tf_w);");
        EmitLine(
            "bool xe_tf_x1 = xe_tf_bx && (xe_tf_i0 + 1.0 < 0.0 || xe_tf_i0 + "
            "1.0 >= xe_tf_w);");
        EmitLine(
            "xe_tf_result.x = (xe_tf_x0 ? (1.0 - xe_tf_f) : 0.0) + (xe_tf_x1 ? "
            "xe_tf_f : 0.0);");
      } break;
      case xenos::FetchOpDimension::k2D:
      case xenos::FetchOpDimension::kCube: {
        // Cube reuses the 2D face-UV footprint; cube faces rarely use border
        // clamp, so this is normally zero.
        EmitLine("float2 xe_tf_s = XeGetTextureFetchSize2D(" +
                 fetch_constant_literal + ");");
        EmitLine("float2 xe_tf_f = frac(xe_tf_weight_coord.xy);");
        EmitLine("float2 xe_tf_i0 = floor(xe_tf_weight_coord.xy);");
        EmitLine("float2 xe_tf_i1 = xe_tf_i0 + 1.0;");
        EmitLine(
            "bool xe_tf_x0 = xe_tf_bx && (xe_tf_i0.x < 0.0 || xe_tf_i0.x >= "
            "xe_tf_s.x);");
        EmitLine(
            "bool xe_tf_x1 = xe_tf_bx && (xe_tf_i1.x < 0.0 || xe_tf_i1.x >= "
            "xe_tf_s.x);");
        EmitLine(
            "bool xe_tf_y0 = xe_tf_by && (xe_tf_i0.y < 0.0 || xe_tf_i0.y >= "
            "xe_tf_s.y);");
        EmitLine(
            "bool xe_tf_y1 = xe_tf_by && (xe_tf_i1.y < 0.0 || xe_tf_i1.y >= "
            "xe_tf_s.y);");
        EmitLine("float xe_tf_bcf = 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x0 || xe_tf_y0) ? (1.0 - xe_tf_f.x) * (1.0 - "
            "xe_tf_f.y) : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x1 || xe_tf_y0) ? xe_tf_f.x * (1.0 - "
            "xe_tf_f.y) : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x0 || xe_tf_y1) ? (1.0 - xe_tf_f.x) * "
            "xe_tf_f.y : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x1 || xe_tf_y1) ? xe_tf_f.x * xe_tf_f.y : "
            "0.0;");
        EmitLine("xe_tf_result.x = xe_tf_bcf;");
      } break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        EmitLine("float3 xe_tf_s = XeGetTextureFetchSize3DOrStacked(" +
                 fetch_constant_literal + ");");
        EmitLine("float3 xe_tf_f = frac(xe_tf_weight_coord);");
        EmitLine("float3 xe_tf_i0 = floor(xe_tf_weight_coord);");
        EmitLine("float3 xe_tf_i1 = xe_tf_i0 + 1.0;");
        EmitLine(
            "bool xe_tf_x0 = xe_tf_bx && (xe_tf_i0.x < 0.0 || xe_tf_i0.x >= "
            "xe_tf_s.x);");
        EmitLine(
            "bool xe_tf_x1 = xe_tf_bx && (xe_tf_i1.x < 0.0 || xe_tf_i1.x >= "
            "xe_tf_s.x);");
        EmitLine(
            "bool xe_tf_y0 = xe_tf_by && (xe_tf_i0.y < 0.0 || xe_tf_i0.y >= "
            "xe_tf_s.y);");
        EmitLine(
            "bool xe_tf_y1 = xe_tf_by && (xe_tf_i1.y < 0.0 || xe_tf_i1.y >= "
            "xe_tf_s.y);");
        EmitLine(
            "bool xe_tf_z0 = xe_tf_bz && (xe_tf_i0.z < 0.0 || xe_tf_i0.z >= "
            "xe_tf_s.z);");
        EmitLine(
            "bool xe_tf_z1 = xe_tf_bz && (xe_tf_i1.z < 0.0 || xe_tf_i1.z >= "
            "xe_tf_s.z);");
        EmitLine("float3 xe_tf_g = 1.0 - xe_tf_f;");
        EmitLine("float xe_tf_bcf = 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x0||xe_tf_y0||xe_tf_z0) ? xe_tf_g.x*xe_tf_g.y*"
            "xe_tf_g.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x1||xe_tf_y0||xe_tf_z0) ? xe_tf_f.x*xe_tf_g.y*"
            "xe_tf_g.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x0||xe_tf_y1||xe_tf_z0) ? xe_tf_g.x*xe_tf_f.y*"
            "xe_tf_g.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x1||xe_tf_y1||xe_tf_z0) ? xe_tf_f.x*xe_tf_f.y*"
            "xe_tf_g.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x0||xe_tf_y0||xe_tf_z1) ? xe_tf_g.x*xe_tf_g.y*"
            "xe_tf_f.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x1||xe_tf_y0||xe_tf_z1) ? xe_tf_f.x*xe_tf_g.y*"
            "xe_tf_f.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x0||xe_tf_y1||xe_tf_z1) ? xe_tf_g.x*xe_tf_f.y*"
            "xe_tf_f.z : 0.0;");
        EmitLine(
            "xe_tf_bcf += (xe_tf_x1||xe_tf_y1||xe_tf_z1) ? xe_tf_f.x*xe_tf_f.y*"
            "xe_tf_f.z : 0.0;");
        EmitLine("xe_tf_result.x = xe_tf_bcf;");
      } break;
      default:
        break;
    }
  } else {
    const bool texture_fetch = instr.opcode == FetchOpcode::kTextureFetch;
    const bool use_computed_lod = TextureFetchUsesComputedLod(instr);
    const bool use_sample_grad =
        use_computed_lod && instr.attributes.use_register_gradients &&
        instr.dimension != xenos::FetchOpDimension::k1D;
    const bool use_sample_level = !use_computed_lod;
    const xenos::TextureFilter sampler_mip_filter =
        instr.opcode == FetchOpcode::kGetTextureComputedLod
            ? xenos::TextureFilter::kLinear
            : instr.attributes.mip_filter;
    const xenos::AnisoFilter sampler_aniso_filter =
        use_computed_lod ? instr.attributes.aniso_filter
                         : xenos::AnisoFilter::kDisabled;

    uint32_t texture_binding_unsigned = UINT32_MAX;
    uint32_t texture_binding_signed = UINT32_MAX;
    uint32_t texture_binding_3d_unsigned = UINT32_MAX;
    uint32_t texture_binding_3d_signed = UINT32_MAX;
    uint32_t texture_binding_2d_unsigned = UINT32_MAX;
    uint32_t texture_binding_2d_signed = UINT32_MAX;

    if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
      texture_binding_3d_unsigned = FindOrAddTextureBinding(
          fetch_constant_index, xenos::FetchOpDimension::k3DOrStacked, false);
      texture_binding_2d_unsigned = FindOrAddTextureBinding(
          fetch_constant_index, xenos::FetchOpDimension::k2D, false);
      if (texture_fetch) {
        texture_binding_3d_signed = FindOrAddTextureBinding(
            fetch_constant_index, xenos::FetchOpDimension::k3DOrStacked, true);
        texture_binding_2d_signed = FindOrAddTextureBinding(
            fetch_constant_index, xenos::FetchOpDimension::k2D, true);
      }
    } else {
      texture_binding_unsigned =
          FindOrAddTextureBinding(fetch_constant_index, instr.dimension, false);
      if (texture_fetch) {
        texture_binding_signed = FindOrAddTextureBinding(fetch_constant_index,
                                                         instr.dimension, true);
      }
    }
    uint32_t sampler_binding_index = FindOrAddSamplerBinding(
        fetch_constant_index, instr.attributes.mag_filter,
        instr.attributes.min_filter, sampler_mip_filter, sampler_aniso_filter);

    EmitLine("float xe_tf_lod = XeGetTextureFetchLodBias(" +
             fetch_constant_literal + ") + " + instruction_lod_bias + ";");
    if (instr.attributes.use_register_lod) {
      EmitLine("xe_tf_lod += xe_texture_lod;");
    }
    EmitLine("float3 xe_tf_grad_h = xe_texture_grad_h;");
    EmitLine("float3 xe_tf_grad_v = xe_texture_grad_v;");
    if (instr.attributes.use_register_gradients &&
        instr.attributes.unnormalized_coordinates &&
        instr.dimension != xenos::FetchOpDimension::k1D &&
        instr.dimension != xenos::FetchOpDimension::kCube) {
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
        case xenos::FetchOpDimension::k2D:
          EmitLine("xe_tf_grad_h.xy /= xe_tf_size.xy;");
          EmitLine("xe_tf_grad_v.xy /= xe_tf_size.xy;");
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          EmitLine("xe_tf_grad_h.xy /= xe_tf_size.xy;");
          EmitLine("xe_tf_grad_v.xy /= xe_tf_size.xy;");
          EmitLine("if (xe_tf_is_3d) {");
          Indent();
          EmitLine("xe_tf_grad_h.z /= xe_tf_size.z;");
          EmitLine("xe_tf_grad_v.z /= xe_tf_size.z;");
          Outdent();
          EmitLine("}");
          break;
        default:
          break;
      }
    }
    if (use_sample_grad) {
      // Fold the LOD bias into the gradients (exp2 scale) so SampleGrad honors
      // the fetch-constant and instruction LOD bias, matching DXBC's lod_src.
      EmitLine("float xe_tf_grad_scale = exp2(xe_tf_lod);");
      EmitLine("xe_tf_grad_h *= xe_tf_grad_scale;");
      EmitLine("xe_tf_grad_v *= xe_tf_grad_scale;");
    }
    EmitLine("float4 xe_tf_result_unsigned = float4(0.0, 0.0, 0.0, 0.0);");
    EmitLine("float4 xe_tf_result_signed = float4(0.0, 0.0, 0.0, 0.0);");
    if (texture_fetch) {
      EmitLine("uint xe_tf_signs_vec = xe_texture_swizzled_signs[" +
               std::to_string(fetch_constant_index >> 4) + "][" +
               std::to_string((fetch_constant_index >> 2) & 3) + "];");
      EmitLine("uint xe_tf_signs = (xe_tf_signs_vec >> " +
               std::to_string((fetch_constant_index & 3) * 8) + "u) & 0xFFu;");
      EmitLine("uint xe_tf_sign_x = xe_tf_signs & 0x3u;");
      EmitLine("uint xe_tf_sign_y = (xe_tf_signs >> 2u) & 0x3u;");
      EmitLine("uint xe_tf_sign_z = (xe_tf_signs >> 4u) & 0x3u;");
      EmitLine("uint xe_tf_sign_w = (xe_tf_signs >> 6u) & 0x3u;");
      EmitLine(
          "bool xe_tf_needs_signed = xe_tf_sign_x == 1u || xe_tf_sign_y == "
          "1u || xe_tf_sign_z == 1u || xe_tf_sign_w == 1u;");
      EmitLine(
          "bool xe_tf_needs_unsigned = xe_tf_sign_x != 1u || xe_tf_sign_y != "
          "1u || xe_tf_sign_z != 1u || xe_tf_sign_w != 1u;");
    }

    // Resolve a texture binding to the HLSL variable used in a sample call. In
    // bindless mode this declares a local bound from ResourceDescriptorHeap; in
    // bindful mode it names the global SRV declared at t[1 + binding index].
    auto bind_texture = [&](const std::string& local_name, const char* type,
                            uint32_t binding_index) -> std::string {
      if (!bindless_resources_used_) {
        return "xe_texture" + std::to_string(binding_index);
      }
      uint32_t descriptor_index =
          texture_bindings_[binding_index].bindless_descriptor_index;
      EmitLine("uint " + local_name + "_idx = XeGetDescriptorIndex(" +
               std::to_string(descriptor_index) +
               "u) + kXeResourceDescriptorHeapStart;");
      EmitLine(std::string(type) + "<float4> " + local_name +
               " = ResourceDescriptorHeap[NonUniformResourceIndex(" +
               local_name + "_idx)];");
      return local_name;
    };
    auto emit_sample =
        [&](const std::string& result_name, const std::string& texture_name,
            const std::string& sampler_name, const std::string& sample_coord,
            const std::string& lod_coord, const std::string& grad_h,
            const std::string& grad_v) {
          if (instr.opcode == FetchOpcode::kGetTextureComputedLod) {
            if (is_pixel_shader() && !instr.attributes.use_register_gradients) {
              EmitLine("float xe_tf_computed_lod = " + texture_name +
                       ".CalculateLevelOfDetailUnclamped(" + sampler_name +
                       ", " + lod_coord + ");");
              EmitLine(result_name +
                       " = float4(xe_tf_computed_lod, xe_tf_computed_lod, "
                       "xe_tf_computed_lod, xe_tf_computed_lod);");
            }
          } else if (use_sample_grad) {
            EmitLine(result_name + " = " + texture_name + ".SampleGrad(" +
                     sampler_name + ", " + sample_coord + ", " + grad_h + ", " +
                     grad_v + ");");
          } else if (use_sample_level) {
            EmitLine(result_name + " = " + texture_name + ".SampleLevel(" +
                     sampler_name + ", " + sample_coord + ", xe_tf_lod);");
          } else {
            EmitLine(result_name + " = " + texture_name + ".SampleBias(" +
                     sampler_name + ", " + sample_coord + ", xe_tf_lod);");
          }
        };
    auto emit_texture_fetch_sample =
        [&](const std::string& condition, const std::string& result_name,
            const std::string& texture_name, const std::string& sampler_name,
            const std::string& sample_coord, const std::string& lod_coord,
            const std::string& grad_h, const std::string& grad_v) {
          if (!texture_fetch) {
            emit_sample(result_name, texture_name, sampler_name, sample_coord,
                        lod_coord, grad_h, grad_v);
            return;
          }
          EmitLine("if (" + condition + ") {");
          Indent();
          emit_sample(result_name, texture_name, sampler_name, sample_coord,
                      lod_coord, grad_h, grad_v);
          Outdent();
          EmitLine("}");
        };

    std::string sampler_var;
    if (bindless_resources_used_) {
      uint32_t sampler_descriptor_index =
          sampler_bindings_[sampler_binding_index].bindless_descriptor_index;
      EmitLine("uint xe_tf_smp_idx = XeGetDescriptorIndex(" +
               std::to_string(sampler_descriptor_index) + "u);");
      EmitLine(
          "SamplerState xe_tf_smp = "
          "SamplerDescriptorHeap[NonUniformResourceIndex(xe_tf_smp_idx)];");
      sampler_var = "xe_tf_smp";
    } else {
      sampler_var = "xe_sampler" + std::to_string(sampler_binding_index);
    }

    switch (instr.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D: {
        std::string tex_unsigned = bind_texture(
            "xe_tf_tex_unsigned", "Texture2DArray", texture_binding_unsigned);
        std::string tex_signed;
        if (texture_fetch) {
          tex_signed = bind_texture("xe_tf_tex_signed", "Texture2DArray",
                                    texture_binding_signed);
        }
        EmitLine("float3 xe_tf_uvl = float3(xe_tf_uv, 0.0);");
        emit_texture_fetch_sample("xe_tf_needs_unsigned",
                                  "xe_tf_result_unsigned", tex_unsigned,
                                  sampler_var, "xe_tf_uvl", "xe_tf_uv",
                                  "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
        if (texture_fetch) {
          emit_texture_fetch_sample("xe_tf_needs_signed", "xe_tf_result_signed",
                                    tex_signed, sampler_var, "xe_tf_uvl",
                                    "xe_tf_uv", "xe_tf_grad_h.xy",
                                    "xe_tf_grad_v.xy");
        }
      } break;
      case xenos::FetchOpDimension::k3DOrStacked: {
        EmitLine("if (XeTextureFetchConstantIs3D(" + fetch_constant_literal +
                 ")) {");
        Indent();
        std::string tex3d_unsigned = bind_texture(
            "xe_tf_tex_3d_unsigned", "Texture3D", texture_binding_3d_unsigned);
        std::string tex3d_signed;
        if (texture_fetch) {
          tex3d_signed = bind_texture("xe_tf_tex_3d_signed", "Texture3D",
                                      texture_binding_3d_signed);
        }
        emit_texture_fetch_sample("xe_tf_needs_unsigned",
                                  "xe_tf_result_unsigned", tex3d_unsigned,
                                  sampler_var, "xe_tf_uvw", "xe_tf_uvw",
                                  "xe_tf_grad_h", "xe_tf_grad_v");
        if (texture_fetch) {
          emit_texture_fetch_sample("xe_tf_needs_signed", "xe_tf_result_signed",
                                    tex3d_signed, sampler_var, "xe_tf_uvw",
                                    "xe_tf_uvw", "xe_tf_grad_h",
                                    "xe_tf_grad_v");
        }
        Outdent();
        EmitLine("} else {");
        Indent();
        std::string tex2d_unsigned =
            bind_texture("xe_tf_tex_2d_unsigned", "Texture2DArray",
                         texture_binding_2d_unsigned);
        std::string tex2d_signed;
        if (texture_fetch) {
          tex2d_signed = bind_texture("xe_tf_tex_2d_signed", "Texture2DArray",
                                      texture_binding_2d_signed);
        }
        // Stacked textures are host 2D arrays, which don't filter across
        // layers. Match DxbcShaderTranslator by lerping the two adjacent
        // layers when the volume magnification filter is linear (point keeps
        // the single-layer sample). Layer minification filtering is left
        // unhandled, as it is in DXBC.
        {
          std::string layer_linear;
          switch (instr.attributes.vol_mag_filter) {
            case xenos::TextureFilter::kPoint:
              layer_linear = "false";
              break;
            case xenos::TextureFilter::kLinear:
              layer_linear = "true";
              break;
            default:
              layer_linear = "((XeGetTextureFetchConstantWord(" +
                             fetch_constant_literal + ", 4u) & 1u) != 0u)";
              break;
          }
          EmitLine("if (" + layer_linear + ") {");
          Indent();
          EmitLine("float xe_tf_layer0 = floor(xe_tf_uvw.z - 0.5f);");
          EmitLine(
              "float xe_tf_layer_frac = (xe_tf_uvw.z - 0.5f) - "
              "xe_tf_layer0;");
          EmitLine("float3 xe_tf_uvw_l0 = float3(xe_tf_uvw.xy, xe_tf_layer0);");
          EmitLine(
              "float3 xe_tf_uvw_l1 = float3(xe_tf_uvw.xy, "
              "xe_tf_layer0 + 1.0f);");
          EmitLine(
              "float4 xe_tf_layer1_unsigned = float4(0.0, 0.0, 0.0, 0.0);");
          emit_texture_fetch_sample("xe_tf_needs_unsigned",
                                    "xe_tf_result_unsigned", tex2d_unsigned,
                                    sampler_var, "xe_tf_uvw_l0", "xe_tf_uvw.xy",
                                    "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          emit_texture_fetch_sample("xe_tf_needs_unsigned",
                                    "xe_tf_layer1_unsigned", tex2d_unsigned,
                                    sampler_var, "xe_tf_uvw_l1", "xe_tf_uvw.xy",
                                    "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          EmitLine(
              "xe_tf_result_unsigned = lerp(xe_tf_result_unsigned, "
              "xe_tf_layer1_unsigned, xe_tf_layer_frac);");
          if (texture_fetch) {
            EmitLine(
                "float4 xe_tf_layer1_signed = float4(0.0, 0.0, 0.0, 0.0);");
            emit_texture_fetch_sample(
                "xe_tf_needs_signed", "xe_tf_result_signed", tex2d_signed,
                sampler_var, "xe_tf_uvw_l0", "xe_tf_uvw.xy", "xe_tf_grad_h.xy",
                "xe_tf_grad_v.xy");
            emit_texture_fetch_sample(
                "xe_tf_needs_signed", "xe_tf_layer1_signed", tex2d_signed,
                sampler_var, "xe_tf_uvw_l1", "xe_tf_uvw.xy", "xe_tf_grad_h.xy",
                "xe_tf_grad_v.xy");
            EmitLine(
                "xe_tf_result_signed = lerp(xe_tf_result_signed, "
                "xe_tf_layer1_signed, xe_tf_layer_frac);");
          }
          Outdent();
          EmitLine("} else {");
          Indent();
          // Floor the array layer (D3D rounds to nearest, but the 360
          // addresses stacked layers like 3D), matching DXBC for point.
          EmitLine("xe_tf_uvw.z = floor(xe_tf_uvw.z);");
          emit_texture_fetch_sample("xe_tf_needs_unsigned",
                                    "xe_tf_result_unsigned", tex2d_unsigned,
                                    sampler_var, "xe_tf_uvw", "xe_tf_uvw.xy",
                                    "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          if (texture_fetch) {
            emit_texture_fetch_sample("xe_tf_needs_signed",
                                      "xe_tf_result_signed", tex2d_signed,
                                      sampler_var, "xe_tf_uvw", "xe_tf_uvw.xy",
                                      "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          }
          Outdent();
          EmitLine("}");
        }
        Outdent();
        EmitLine("}");
      } break;
      case xenos::FetchOpDimension::kCube: {
        std::string tex_unsigned = bind_texture(
            "xe_tf_tex_unsigned", "TextureCube", texture_binding_unsigned);
        std::string tex_signed;
        if (texture_fetch) {
          tex_signed = bind_texture("xe_tf_tex_signed", "TextureCube",
                                    texture_binding_signed);
        }
        emit_texture_fetch_sample("xe_tf_needs_unsigned",
                                  "xe_tf_result_unsigned", tex_unsigned,
                                  sampler_var, "xe_tf_dir", "xe_tf_dir",
                                  "xe_tf_grad_h", "xe_tf_grad_v");
        if (texture_fetch) {
          emit_texture_fetch_sample("xe_tf_needs_signed", "xe_tf_result_signed",
                                    tex_signed, sampler_var, "xe_tf_dir",
                                    "xe_tf_dir", "xe_tf_grad_h",
                                    "xe_tf_grad_v");
        }
      } break;
      default:
        break;
    }

    if (texture_fetch) {
      EmitLine("xe_tf_result = float4(");
      Indent();
      EmitLine(
          "XeApplyTextureSign(xe_tf_result_unsigned.x, xe_tf_result_signed.x, "
          "xe_tf_sign_x),");
      EmitLine(
          "XeApplyTextureSign(xe_tf_result_unsigned.y, xe_tf_result_signed.y, "
          "xe_tf_sign_y),");
      EmitLine(
          "XeApplyTextureSign(xe_tf_result_unsigned.z, xe_tf_result_signed.z, "
          "xe_tf_sign_z),");
      EmitLine(
          "XeApplyTextureSign(xe_tf_result_unsigned.w, xe_tf_result_signed.w, "
          "xe_tf_sign_w));");
      Outdent();
      EmitLine("xe_tf_result *= XeGetTextureFetchExpAdjust(" +
               fetch_constant_literal + ");");
    } else {
      EmitLine("xe_tf_result = xe_tf_result_unsigned;");
    }
  }

  // Store result with proper swizzle matching.
  // Uses EmitVectorResultAssignment which handles result.components[] to
  // determine which source component goes to each destination, matching
  // DXBC's StoreResult behavior.
  if (!result_value.empty()) {
    EmitVectorResultAssignment(instr.result, result_value);
    // Store any constant components (k0 or k1) in the write mask.
    StoreConstantComponents(instr.result);
  }

  Outdent();
  EmitLine("}");
}
}  // namespace gpu
}  // namespace xe
