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

bool HlslShaderTranslator::PixelShaderNeedsFloat24DepthOutput() const {
  if (!is_pixel_shader()) {
    return false;
  }
  using DepthStencilMode = Modification::DepthStencilMode;
  DepthStencilMode mode = GetHlslShaderModification().pixel.depth_stencil_mode;
  return mode == DepthStencilMode::kFloat24Truncating ||
         mode == DepthStencilMode::kFloat24Rounding ||
         mode == DepthStencilMode::kFloat24TruncatingPolygonOffset ||
         mode == DepthStencilMode::kFloat24RoundingPolygonOffset;
}

bool HlslShaderTranslator::PixelShaderAppliesPolygonOffset() const {
  if (!is_pixel_shader() || edram_rov_used_) {
    return false;
  }
  using DepthStencilMode = Modification::DepthStencilMode;
  DepthStencilMode mode = GetHlslShaderModification().pixel.depth_stencil_mode;
  return mode == DepthStencilMode::kPolygonOffset ||
         mode == DepthStencilMode::kFloat24TruncatingPolygonOffset ||
         mode == DepthStencilMode::kFloat24RoundingPolygonOffset;
}

bool HlslShaderTranslator::PixelShaderWritesDepthOutput() const {
  return is_pixel_shader() && (current_shader().writes_depth() ||
                               PixelShaderNeedsFloat24DepthOutput() ||
                               PixelShaderAppliesPolygonOffset());
}

bool HlslShaderTranslator::IsSampleRate() const {
  // Float24 depth conversion under MSAA must run per sample so each sample's
  // depth comes from its own interpolated position Z (SV_Position is declared
  // sample-frequency). With a shader-written oDepth there's a single depth for
  // the whole pixel, so sample-rate isn't needed. Mirrors DxbcShaderTranslator.
  return is_pixel_shader() && !edram_rov_used_ &&
         PixelShaderNeedsFloat24DepthOutput() &&
         !current_shader().writes_depth();
}

bool HlslShaderTranslator::IsForceEarlyDepthStencilEnabled() const {
  return is_pixel_shader() &&
         GetHlslShaderModification().pixel.depth_stencil_mode ==
             Modification::DepthStencilMode::kEarlyHint &&
         !edram_rov_used_ && current_shader().implicit_early_z_write_allowed();
}

bool HlslShaderTranslator::PixelShaderNeedsCoverageOutput() const {
  return is_pixel_shader() && current_shader().writes_color_target(0) &&
         !IsForceEarlyDepthStencilEnabled();
}

void HlslShaderTranslator::EmitPixelShaderParamGen() {
  if (!is_pixel_shader()) {
    return;
  }
  Modification modification = GetHlslShaderModification();
  if (!modification.pixel.param_gen_enable ||
      modification.pixel.param_gen_interpolator >= register_count()) {
    return;
  }

  std::string dest =
      RegisterToHlsl(modification.pixel.param_gen_interpolator,
                     InstructionStorageAddressingMode::kAbsolute);
  EmitLine("// Generate PsParamGen pseudo-interpolator");
  EmitLine("{");
  Indent();
  EmitLine("float2 xe_param_xy = floor(input.xe_position.xy);");
  if (draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1) {
    EmitLine("xe_param_xy *= float2(" +
             HlslFloatLiteral(1.0f / float(draw_resolution_scale_x_)) + ", " +
             HlslFloatLiteral(1.0f / float(draw_resolution_scale_y_)) + ");");
  }
  EmitLine("float4 xe_param_gen = float4(abs(xe_param_xy), 0.0f, 0.0f);");
  if (modification.pixel.param_gen_point) {
    EmitLine("xe_param_gen.y = XeSetFloatSignBit(abs(xe_param_gen.y));");
    EmitLine("xe_param_gen.zw = saturate(input.xe_point_parameters.xy);");
  } else {
    EmitLine("if ((xe_flags & 16u) != 0u && !input.xe_is_front_face) {");
    Indent();
    EmitLine("xe_param_gen.x = XeSetFloatSignBit(xe_param_gen.x);");
    Outdent();
    EmitLine("}");
    EmitLine("if ((xe_flags & 32u) != 0u) {");
    Indent();
    EmitLine("xe_param_gen.z = asfloat(0x80000000u);");
    Outdent();
    EmitLine("}");
  }
  EmitLine(dest + " = xe_param_gen;");
  Outdent();
  EmitLine("}");
  EmitLine("");
}

void HlslShaderTranslator::EmitPixelShaderAlphaTest() {
  if (!is_pixel_shader() || !current_shader().writes_color_target(0) ||
      IsForceEarlyDepthStencilEnabled()) {
    return;
  }

  // Comparison expressions, with the optional fuzzy epsilon tolerance matching
  // DxbcShaderTranslator. NotEqual stays true for NaN.
  const char* cmp_ne;
  const char* cmp_lt;
  const char* cmp_eq;
  const char* cmp_gt;
  if (cvars::use_fuzzy_alpha_epsilon) {
    cmp_ne = "!(abs(xe_alpha_test_alpha - xe_alpha_test_reference) < 1e-3f)";
    cmp_lt = "((xe_alpha_test_alpha - 1e-3f) < xe_alpha_test_reference)";
    cmp_eq = "(abs(xe_alpha_test_alpha - xe_alpha_test_reference) < 1e-3f)";
    cmp_gt = "(xe_alpha_test_reference < (xe_alpha_test_alpha + 1e-3f))";
  } else {
    cmp_ne = "(xe_alpha_test_alpha != xe_alpha_test_reference)";
    cmp_lt = "(xe_alpha_test_alpha < xe_alpha_test_reference)";
    cmp_eq = "(xe_alpha_test_alpha == xe_alpha_test_reference)";
    cmp_gt = "(xe_alpha_test_reference < xe_alpha_test_alpha)";
  }

  EmitLine("// Alpha test");
  // Under ROV, only test when render target 0 was written on this execution
  // path (output.xe_color_0 is otherwise zero), matching the DXBC rov_params
  // 1 << 8 guard.
  EmitLine(edram_rov_used_ ? "if ((xe_color_written & 1u) != 0u) {" : "{");
  Indent();
  EmitLine("uint xe_alpha_test_function = (xe_flags >> 7u) & 7u;");
  EmitLine("if (xe_alpha_test_function != 7u) {");
  Indent();
  EmitLine("float xe_alpha_test_alpha = output.xe_color_0.a;");
  EmitLine("bool xe_alpha_test_pass = false;");
  EmitLine("if (xe_alpha_test_function == 5u) {");
  Indent();
  EmitLine(std::string("xe_alpha_test_pass = ") + cmp_ne + ";");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(std::string("xe_alpha_test_pass = xe_alpha_test_pass || "
                       "(((xe_alpha_test_function & 1u) != 0u) && ") +
           cmp_lt + ");");
  EmitLine(std::string("xe_alpha_test_pass = xe_alpha_test_pass || "
                       "(((xe_alpha_test_function & 2u) != 0u) && ") +
           cmp_eq + ");");
  EmitLine(std::string("xe_alpha_test_pass = xe_alpha_test_pass || "
                       "(((xe_alpha_test_function & 4u) != 0u) && ") +
           cmp_gt + ");");
  Outdent();
  EmitLine("}");
  EmitLine("if (!xe_alpha_test_pass) { discard; }");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");
}

void HlslShaderTranslator::EmitPixelShaderAlphaToCoverage() {
  if (!PixelShaderNeedsCoverageOutput()) {
    return;
  }

  if (edram_rov_used_) {
    // Under ROV there is no SV_Coverage output - alpha to coverage instead
    // narrows the per-sample ROV coverage mask, applied by the output merger.
    EmitROVAlphaToCoverage();
    return;
  }

  EmitLine("// Alpha to coverage");
  EmitLine("if (xe_alpha_to_mask != 0u) {");
  Indent();
  EmitLine("uint2 xe_atoc_pixel = uint2(input.xe_position.xy);");
  EmitLine(
      "uint xe_atoc_offset_index = ((xe_atoc_pixel.x & 1u) << 1u) | "
      "(xe_atoc_pixel.y & 1u);");
  EmitLine(
      "float xe_atoc_offset = float((xe_alpha_to_mask >> "
      "(xe_atoc_offset_index << 1u)) & 3u);");
  EmitLine("float xe_atoc_alpha = output.xe_color_0.a;");
  EmitLine("uint xe_atoc_coverage = 0u;");
  EmitLine("if (xe_sample_count_log2.y != 0u) {");
  Indent();
  EmitLine("if (xe_sample_count_log2.x != 0u) {");
  Indent();
  EmitLine(
      "xe_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.75f - xe_atoc_offset * (1.0f / 16.0f))) ? 1u : 0u;");
  EmitLine(
      "xe_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.25f - xe_atoc_offset * (1.0f / 16.0f))) ? 2u : 0u;");
  EmitLine(
      "xe_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.5f - xe_atoc_offset * (1.0f / 16.0f))) ? 4u : 0u;");
  EmitLine(
      "xe_atoc_coverage |= (xe_atoc_alpha >= "
      "(1.0f - xe_atoc_offset * (1.0f / 16.0f))) ? 8u : 0u;");
  Outdent();
  EmitLine("} else {");
  Indent();
  if (msaa_2x_supported_) {
    // Native 2x: top sample is D3D index 1, bottom is index 0.
    EmitLine(
        "xe_atoc_coverage |= (xe_atoc_alpha >= "
        "(0.5f - xe_atoc_offset * (1.0f / 8.0f))) ? 2u : 0u;");
    EmitLine(
        "xe_atoc_coverage |= (xe_atoc_alpha >= "
        "(1.0f - xe_atoc_offset * (1.0f / 8.0f))) ? 1u : 0u;");
  } else {
    // 2x emulated as 4x: top sample is D3D index 0, bottom is index 3.
    EmitLine(
        "xe_atoc_coverage |= (xe_atoc_alpha >= "
        "(0.5f - xe_atoc_offset * (1.0f / 8.0f))) ? 1u : 0u;");
    EmitLine(
        "xe_atoc_coverage |= (xe_atoc_alpha >= "
        "(1.0f - xe_atoc_offset * (1.0f / 8.0f))) ? 8u : 0u;");
  }
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(
      "xe_atoc_coverage = (xe_atoc_alpha >= "
      "(1.0f - xe_atoc_offset * (1.0f / 4.0f))) ? 1u : 0u;");
  Outdent();
  EmitLine("}");
  EmitLine("output.xe_coverage = xe_atoc_coverage;");
  EmitLine("if (xe_atoc_coverage == 0u) { discard; }");
  Outdent();
  EmitLine("}");
  EmitLine("");
}

void HlslShaderTranslator::EmitROVAlphaToCoverage() {
  // Guest sample order, mirroring xe_rov_coverage and
  // DxbcShaderTranslator::CompletePixelShader_AlphaToMask. Alpha is read here
  // before the exponent bias, matching the DXBC. Default all samples passing so
  // the merger AND is a no-op when alpha to coverage is disabled.
  EmitLine("// Alpha to coverage (ROV)");
  EmitLine("uint xe_rov_atoc_coverage = 0xFu;");
  // Only narrow when render target 0 was written on this execution path,
  // matching the DXBC rov_params 1 << 8 guard. Otherwise the mask stays full.
  EmitLine("if (xe_alpha_to_mask != 0u && (xe_color_written & 1u) != 0u) {");
  Indent();
  EmitLine("uint2 xe_atoc_pixel = uint2(input.xe_position.xy);");
  EmitLine(
      "uint xe_atoc_offset_index = ((xe_atoc_pixel.x & 1u) << 1u) | "
      "(xe_atoc_pixel.y & 1u);");
  EmitLine(
      "float xe_atoc_offset = float((xe_alpha_to_mask >> "
      "(xe_atoc_offset_index << 1u)) & 3u);");
  EmitLine("float xe_atoc_alpha = output.xe_color_0.a;");
  EmitLine("xe_rov_atoc_coverage = 0u;");
  EmitLine("if (xe_sample_count_log2.y != 0u) {");
  Indent();
  EmitLine("if (xe_sample_count_log2.x != 0u) {");
  Indent();
  // 4x, guest samples 0..3.
  EmitLine(
      "xe_rov_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.75f - xe_atoc_offset * (1.0f / 16.0f))) ? 1u : 0u;");
  EmitLine(
      "xe_rov_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.25f - xe_atoc_offset * (1.0f / 16.0f))) ? 2u : 0u;");
  EmitLine(
      "xe_rov_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.5f - xe_atoc_offset * (1.0f / 16.0f))) ? 4u : 0u;");
  EmitLine(
      "xe_rov_atoc_coverage |= (xe_atoc_alpha >= "
      "(1.0f - xe_atoc_offset * (1.0f / 16.0f))) ? 8u : 0u;");
  Outdent();
  EmitLine("} else {");
  Indent();
  // 2x, guest samples 0 and 1.
  EmitLine(
      "xe_rov_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.5f - xe_atoc_offset * (1.0f / 8.0f))) ? 1u : 0u;");
  EmitLine(
      "xe_rov_atoc_coverage |= (xe_atoc_alpha >= "
      "(1.0f - xe_atoc_offset * (1.0f / 8.0f))) ? 2u : 0u;");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("} else {");
  Indent();
  // 1x, guest sample 0.
  EmitLine(
      "xe_rov_atoc_coverage = (xe_atoc_alpha >= "
      "(1.0f - xe_atoc_offset * (1.0f / 4.0f))) ? 1u : 0u;");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");
}
}  // namespace gpu
}  // namespace xe
