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

namespace {
// Emitted by EmitResourceDeclarations and replaced in CompleteTranslation with
// the per-binding bindful texture and sampler declarations, which are only
// known once the body has been translated.
constexpr char kBindfulResourceDeclarationsMarker[] =
    "// XE_BINDFUL_RESOURCE_DECLARATIONS";
}  // namespace

std::string HlslShaderTranslator::HlslFloatLiteral(float value) {
  if (std::isnan(value)) {
    return "asfloat(0x7FC00000u)";
  }
  if (std::isinf(value)) {
    return value < 0.0f ? "-asfloat(0x7F800000u)" : "asfloat(0x7F800000u)";
  }
  std::string text = fmt::format("{:.9g}", value);
  if (text.find_first_of(".eE") == std::string::npos) {
    text += ".0";
  }
  text += "f";
  return text;
}

HlslShaderTranslator::HlslShaderTranslator(
    ui::GraphicsProvider::GpuVendorID vendor_id, bool bindless_resources_used,
    bool edram_rov_used, bool gamma_render_target_as_unorm8,
    bool msaa_2x_supported, bool use_shader_model_6_6,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y)
    : vendor_id_(vendor_id),
      bindless_resources_used_(bindless_resources_used),
      edram_rov_used_(edram_rov_used),
      gamma_render_target_as_unorm8_(gamma_render_target_as_unorm8),
      msaa_2x_supported_(msaa_2x_supported),
      use_shader_model_6_6_(use_shader_model_6_6),
      draw_resolution_scale_x_(draw_resolution_scale_x ? draw_resolution_scale_x
                                                       : UINT32_C(1)),
      draw_resolution_scale_y_(draw_resolution_scale_y ? draw_resolution_scale_y
                                                       : UINT32_C(1)) {}

HlslShaderTranslator::~HlslShaderTranslator() = default;

std::string HlslShaderTranslator::GetShaderTargetProfile() const {
  if (is_vertex_shader()) {
    return IsDomainShader() ? "ds_6_6" : "vs_6_6";
  } else {
    return "ps_6_6";
  }
}

bool HlslShaderTranslator::GetDomainShaderInfo(
    const char*& domain_out, uint32_t& control_point_count_out,
    uint32_t& domain_location_component_count_out) const {
  switch (GetHlslShaderModification().vertex.host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      domain_out = "tri";
      control_point_count_out = 3;
      domain_location_component_count_out = 3;
      return true;
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      domain_out = "tri";
      control_point_count_out = 1;
      domain_location_component_count_out = 3;
      return true;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      domain_out = "quad";
      control_point_count_out = 4;
      domain_location_component_count_out = 2;
      return true;
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      domain_out = "quad";
      control_point_count_out = 1;
      domain_location_component_count_out = 2;
      return true;
    default:
      return false;
  }
}

uint64_t HlslShaderTranslator::GetDefaultVertexShaderModification(
    uint32_t dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification modification;
  modification.vertex.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  modification.vertex.interpolator_mask =
      (UINT32_C(1) << xenos::kMaxInterpolators) - 1;
  return modification.value;
}

uint64_t HlslShaderTranslator::GetDefaultPixelShaderModification(
    uint32_t dynamic_addressable_register_count) const {
  Modification modification;
  modification.pixel.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  modification.pixel.interpolator_mask =
      (UINT32_C(1) << xenos::kMaxInterpolators) - 1;
  modification.pixel.depth_stencil_mode =
      Modification::DepthStencilMode::kNoModifiers;
  return modification.value;
}

uint32_t HlslShaderTranslator::GetModificationRegisterCount() const {
  Modification modification = GetHlslShaderModification();
  return is_vertex_shader()
             ? modification.vertex.dynamic_addressable_register_count
             : modification.pixel.dynamic_addressable_register_count;
}

void HlslShaderTranslator::Reset() {
  ShaderTranslator::Reset();

  hlsl_stream_.str("");
  hlsl_stream_.clear();
  hlsl_source_.clear();
  indent_level_ = 0;
  indent_string_.clear();

  cf_exec_predicated_ = false;
  cf_exec_predicate_condition_ = false;
  cf_exec_bool_constant_ = UINT32_MAX;
  cf_exec_bool_constant_condition_ = false;

  has_main_switch_ = false;
  cf_instruction_predicate_if_open_ = false;

  // Clear resource bindings for new translation.
  texture_bindings_.clear();
  sampler_bindings_.clear();
}

void HlslShaderTranslator::PostTranslation() {
  Shader::Translation& translation = current_translation();
  if (!translation.is_valid()) {
    return;
  }
  // Copy bindings to the DxbcShader object (D3D12 uses DxbcShader for all
  // shaders, even when using HLSL/DXIL).
  DxbcShader* dxbc_shader = dynamic_cast<DxbcShader*>(&translation.shader());
  if (dxbc_shader && !dxbc_shader->bindings_setup_entered_.test_and_set(
                         std::memory_order_relaxed)) {
    dxbc_shader->texture_bindings_.clear();
    dxbc_shader->texture_bindings_.reserve(texture_bindings_.size());
    dxbc_shader->used_texture_mask_ = 0;
    for (const TextureBinding& translator_binding : texture_bindings_) {
      DxbcShader::TextureBinding& shader_binding =
          dxbc_shader->texture_bindings_.emplace_back();
      // For a stable hash.
      std::memset(&shader_binding, 0, sizeof(shader_binding));
      shader_binding.bindless_descriptor_index =
          translator_binding.bindless_descriptor_index;
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.dimension = translator_binding.dimension;
      shader_binding.is_signed = translator_binding.is_signed;
      dxbc_shader->used_texture_mask_ |= 1u
                                         << translator_binding.fetch_constant;
    }
    dxbc_shader->sampler_bindings_.clear();
    dxbc_shader->sampler_bindings_.reserve(sampler_bindings_.size());
    for (const SamplerBinding& translator_binding : sampler_bindings_) {
      DxbcShader::SamplerBinding& shader_binding =
          dxbc_shader->sampler_bindings_.emplace_back();
      shader_binding.bindless_descriptor_index =
          translator_binding.bindless_descriptor_index;
      shader_binding.fetch_constant = translator_binding.fetch_constant;
      shader_binding.mag_filter = translator_binding.mag_filter;
      shader_binding.min_filter = translator_binding.min_filter;
      shader_binding.mip_filter = translator_binding.mip_filter;
      shader_binding.aniso_filter = translator_binding.aniso_filter;
    }
  }
}

void HlslShaderTranslator::EmitLine(const std::string& line) {
  hlsl_stream_ << indent_string_ << line << "\n";
}

void HlslShaderTranslator::Emit(const std::string& text) {
  hlsl_stream_ << text;
}

void HlslShaderTranslator::Indent() {
  ++indent_level_;
  indent_string_ = std::string(indent_level_ * 2, ' ');
}

void HlslShaderTranslator::Outdent() {
  if (indent_level_ > 0) {
    --indent_level_;
  }
  indent_string_ = std::string(indent_level_ * 2, ' ');
}

void HlslShaderTranslator::EmitSystemConstants() {
  // System constants - must match xenos_draw.hlsli and
  // DxbcShaderTranslator::SystemConstants
  EmitLine("cbuffer xe_system_cbuffer : register(b0) {");
  Indent();
  EmitLine("uint xe_flags;");
  EmitLine("float2 xe_tessellation_factor_range;");
  EmitLine("uint xe_line_loop_closing_index;");
  EmitLine("");
  EmitLine("uint xe_vertex_index_endian;");
  EmitLine("uint xe_vertex_index_offset;");
  EmitLine("uint2 xe_vertex_index_min_max;");
  EmitLine("");
  EmitLine("float4 xe_user_clip_planes[6];");
  EmitLine("");
  EmitLine("float3 xe_ndc_scale;");
  EmitLine("float xe_point_vertex_diameter_min;");
  EmitLine("");
  EmitLine("float3 xe_ndc_offset;");
  EmitLine("float xe_point_vertex_diameter_max;");
  EmitLine("");
  EmitLine("float2 xe_point_constant_diameter;");
  EmitLine("float2 xe_point_screen_diameter_to_ndc_radius;");
  EmitLine("");
  EmitLine("uint4 xe_texture_swizzled_signs[2];");
  EmitLine("");
  EmitLine("uint xe_textures_resolution_scaled;");
  EmitLine("uint2 xe_sample_count_log2;");
  EmitLine("float xe_alpha_test_reference;");
  EmitLine("");
  EmitLine("uint xe_alpha_to_mask;");
  EmitLine("uint xe_edram_32bpp_tile_pitch_dwords_scaled;");
  EmitLine("uint xe_edram_depth_base_dwords_scaled;");
  EmitLine("uint _xe_padding_system_0;");
  EmitLine("");
  EmitLine("float4 xe_color_exp_bias;");
  EmitLine("");
  EmitLine("float2 xe_edram_poly_offset_front;");
  EmitLine("float2 xe_edram_poly_offset_back;");
  EmitLine("");
  EmitLine("uint4 xe_edram_stencil[2];");
  EmitLine("");
  EmitLine("uint4 xe_edram_rt_base_dwords_scaled;");
  EmitLine("");
  EmitLine("uint4 xe_edram_rt_format_flags;");
  EmitLine("");
  EmitLine("float4 xe_edram_rt_clamp[4];");
  EmitLine("");
  EmitLine("uint4 xe_edram_rt_keep_mask[2];");
  EmitLine("");
  EmitLine("uint4 xe_edram_rt_blend_factors_ops;");
  EmitLine("");
  EmitLine("float4 xe_edram_blend_constant;");
  Outdent();
  EmitLine("};");
  EmitLine("");
}

void HlslShaderTranslator::EmitConstantBuffers() {
  // Float constants - use packed count from constant register map.
  // The command processor only fills the float constants that are actually
  // used, so we must match the packed buffer size and use remapped indices.
  const Shader::ConstantRegisterMap& constant_map =
      current_shader().constant_register_map();
  uint32_t float_count = constant_map.float_count;
  // If dynamic addressing is used, all 256 constants could be accessed.
  if (constant_map.float_dynamic_addressing) {
    float_count = 256;
  }
  EmitLine("cbuffer xe_float_constants : register(b1) {");
  Indent();
  EmitLine("float4 xe_float_constants_data[" +
           std::to_string(std::max(float_count, 1u)) + "];");
  Outdent();
  EmitLine("};");
  EmitLine("");

  // Bool and loop constants.
  EmitLine("cbuffer xe_bool_loop_constants : register(b2) {");
  Indent();
  EmitLine("uint4 xe_bool_loop_constants_data[8 + 32];");
  Outdent();
  EmitLine("};");
  EmitLine("");

  // Fetch constants (32 fetch slots, each 6 dwords = 192 dwords = 48 uint4).
  // Each slot can be 1 texture fetch (6 dwords) or 3 vertex fetches (2 dwords
  // each). Vertex fetch vf[i] is at uint4[(i/2)].xy (even) or .zw (odd).
  EmitLine("cbuffer xe_fetch_constants : register(b3) {");
  Indent();
  EmitLine("uint4 xe_fetch_constants_data[48];");
  Outdent();
  EmitLine("};");
  EmitLine("");
}

void HlslShaderTranslator::EmitResourceDeclarations() {
  // Shared memory as byte address buffer (SRV).
  // The runtime uses either SRV or UAV depending on xe_flags bit 0.
  EmitLine("ByteAddressBuffer xe_shared_memory_srv : register(t0);");
  EmitLine("RWByteAddressBuffer xe_shared_memory_uav : register(u0);");
  EmitLine("");

  if (bindless_resources_used_) {
    // Bindless mode (SM 6.6): Use ResourceDescriptorHeap and
    // SamplerDescriptorHeap. Descriptor indices constant buffer contains
    // indices into the heaps.
    EmitLine("// Descriptor indices constant buffer for bindless resources.");
    EmitLine("cbuffer xe_descriptor_indices : register(b4) {");
    Indent();
    // The buffer contains uint indices packed into uint4 vectors. Sized for
    // up to a 4096-byte descriptor-indices CBV.
    EmitLine("uint4 xe_descriptor_indices_data[256];");
    Outdent();
    EmitLine("};");
    EmitLine("");

    // In SM 6.6, we use ResourceDescriptorHeap[] and SamplerDescriptorHeap[]
    // directly in the shader code, no explicit declarations needed.
    EmitLine(
        "// SM 6.6 bindless: Using ResourceDescriptorHeap[] and "
        "SamplerDescriptorHeap[]");
    EmitLine("");
  } else {
    // Bindful textures and samplers are declared one per binding at registers
    // matching DxbcShaderTranslator (t[1 + binding index], s[binding index]).
    // The bindings are only discovered while translating the body, so emit a
    // marker that CompleteTranslation replaces with the declarations.
    EmitLine(kBindfulResourceDeclarationsMarker);
    EmitLine("");
  }
}

void HlslShaderTranslator::EmitInputDeclarations() {
  Modification modification = GetHlslShaderModification();

  if (is_vertex_shader()) {
    if (IsDomainShader()) {
      const char* domain = "quad";
      uint32_t control_point_count = 1;
      uint32_t domain_location_component_count = 2;
      GetDomainShaderInfo(domain, control_point_count,
                          domain_location_component_count);
      // Control points produced by the host hull shader.
      EmitLine("struct XeHSControlPointOutput {");
      Indent();
      EmitLine("float index : XEVERTEXID;");
      Outdent();
      EmitLine("};");
      // Tessellation factors produced by the host hull shader. Unused by the
      // guest, declared to match the hull shader patch constant signature.
      bool is_triangle = domain_location_component_count == 3;
      EmitLine("struct XeHSConstantDataOutput {");
      Indent();
      EmitLine(std::string("float edges[") + (is_triangle ? "3" : "4") +
               "] : SV_TessFactor;");
      EmitLine(is_triangle ? "float inside : SV_InsideTessFactor;"
                           : "float inside[2] : SV_InsideTessFactor;");
      Outdent();
      EmitLine("};");
      EmitLine("");
    } else {
      EmitLine("struct VSInput {");
      Indent();
      EmitLine("uint xe_vertex_id : SV_VertexID;");
      Outdent();
      EmitLine("};");
      EmitLine("");
    }
  } else {
    // Pixel shader input - must match vertex shader output signature.
    // Only declare interpolators that are in the interpolator_mask.
    uint32_t interpolator_mask = modification.pixel.interpolator_mask;
    // With precise interpolation the interpolators are declared nointerpolation
    // so GetAttributeAtVertex can read the raw per-vertex values, and the
    // shader interpolates them manually using SV_Barycentrics.
    bool precise = UsePreciseInterpolation();
    const char* interp_prefix = precise ? "nointerpolation " : "";
    // Interpolators sampling at the centroid under MSAA, matching the guest
    // sampling pattern. Not applicable with precise interpolation, where the
    // raw per-vertex values are read flat.
    uint32_t centroid_mask =
        precise ? 0u : modification.pixel.interpolators_centroid;
    EmitLine("struct PSInput {");
    Indent();
    // Interpolators are packed contiguously by TEXCOORD index.
    uint32_t texcoord_index = 0;
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      if (interpolator_mask & (1u << i)) {
        const char* centroid_prefix =
            (centroid_mask & (1u << i)) ? "centroid " : "";
        EmitLine(interp_prefix + std::string(centroid_prefix) +
                 "float4 xe_interpolator_" + std::to_string(i) + " : TEXCOORD" +
                 std::to_string(texcoord_index) + ";");
        ++texcoord_index;
      }
    }
    // Point parameters after interpolators - only when param_gen_point is set.
    if (modification.pixel.param_gen_point) {
      EmitLine("float3 xe_point_parameters : TEXCOORD" +
               std::to_string(texcoord_index) + ";");
    }
    // SV_Position must come before SV_IsFrontFace so it stays at the same
    // signature register as the VS/GS output (SV_IsFrontFace otherwise consumes
    // a register and shifts SV_Position, breaking inter-stage linkage).
    // At sample rate (per-sample float24 depth), position is sample-frequency
    // interpolated so each invocation reads its own sample's Z, and the
    // rasterizer runs the shader once per covered sample.
    EmitLine(std::string(IsSampleRate() ? "noperspective sample " : "") +
             "float4 xe_position : SV_Position;");
    EmitLine("bool xe_is_front_face : SV_IsFrontFace;");
    // Sample index and input coverage, used to narrow memexport to a single
    // sample per guest pixel when shading runs at sample rate.
    if (IsSampleRate() && MemExportUsed()) {
      EmitLine("uint xe_sample_index : SV_SampleIndex;");
      EmitLine("uint xe_coverage_in : SV_Coverage;");
    }
    if (precise) {
      EmitLine("float3 xe_barycentrics : SV_Barycentrics;");
    }
    Outdent();
    EmitLine("};");
    EmitLine("");
  }
}

void HlslShaderTranslator::EmitOutputDeclarations() {
  Modification modification = GetHlslShaderModification();

  if (is_vertex_shader()) {
    auto hlsl_float_vector_type = [](uint32_t component_count) {
      return component_count == 1
                 ? std::string("float")
                 : std::string("float") + std::to_string(component_count);
    };
    auto emit_distance_declarations = [&](const char* name,
                                          const char* semantic,
                                          uint32_t distance_count) {
      if (!distance_count) {
        return;
      }
      if (distance_count <= 4) {
        EmitLine(hlsl_float_vector_type(distance_count) + " " + name + " : " +
                 semantic + "0;");
      } else {
        EmitLine("float4 " + std::string(name) + "_0123 : " + semantic + "0;");
        EmitLine(hlsl_float_vector_type(distance_count - 4) + " " + name +
                 "_45 : " + semantic + "1;");
      }
    };

    // Vertex shader output - only declare interpolators in the mask.
    // Must match pixel shader input signature (packed contiguously).
    uint32_t interpolator_mask = modification.vertex.interpolator_mask;
    EmitLine("struct VSOutput {");
    Indent();
    // Interpolators are packed contiguously by TEXCOORD index.
    uint32_t texcoord_index = 0;
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      if (interpolator_mask & (1u << i)) {
        EmitLine("float4 xe_interpolator_" + std::to_string(i) + " : TEXCOORD" +
                 std::to_string(texcoord_index) + ";");
        ++texcoord_index;
      }
    }
    // Point parameters after interpolators - only when output_point_size is
    // set.
    if (modification.vertex.output_point_size) {
      EmitLine("float3 xe_point_parameters : TEXCOORD" +
               std::to_string(texcoord_index) + ";");
    }
    EmitLine("float4 xe_position : SV_Position;");
    emit_distance_declarations("xe_clip_distance", "SV_ClipDistance",
                               modification.GetVertexClipDistanceCount());
    emit_distance_declarations("xe_cull_distance", "SV_CullDistance",
                               modification.GetVertexCullDistanceCount());
    Outdent();
    EmitLine("};");
    EmitLine("");
  } else {
    EmitLine("struct PSOutput {");
    Indent();
    // Color outputs - only declare render targets that are actually written.
    uint32_t color_targets_written = current_shader().writes_color_targets();
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_targets_written & (1u << i)) {
        EmitLine("float4 xe_color_" + std::to_string(i) + " : SV_Target" +
                 std::to_string(i) + ";");
      }
    }
    if (PixelShaderNeedsCoverageOutput()) {
      EmitLine("uint xe_coverage : SV_Coverage;");
    }
    // Depth output if needed.
    if (PixelShaderWritesDepthOutput()) {
      std::string depth_semantic = "SV_Depth";
      if (!current_shader().writes_depth() &&
          GetHlslShaderModification().pixel.depth_stencil_mode ==
              Modification::DepthStencilMode::kFloat24Truncating) {
        depth_semantic = "SV_DepthLessEqual";
      }
      EmitLine("float xe_depth : " + depth_semantic + ";");
    }
    Outdent();
    EmitLine("};");
    EmitLine("");
  }
}

void HlslShaderTranslator::EmitHelperFunctions() {
  // Helper function for endian swap.
  EmitLine("uint XeEndianSwap(uint value, uint endian) {");
  Indent();
  EmitLine("switch (endian) {");
  Indent();
  EmitLine("case 1u: // 8-in-16");
  EmitLine("case 2u: // 8-in-32");
  Indent();
  EmitLine(
      "value = ((value & 0x00FF00FFu) << 8u) | "
      "((value & 0xFF00FF00u) >> 8u);");
  EmitLine("if (endian == 1u) return value;");
  Outdent();
  EmitLine("// Fall through for 8-in-32");
  EmitLine("case 3u: // 16-in-32");
  Indent();
  EmitLine(
      "value = ((value & 0x0000FFFFu) << 16u) | "
      "((value & 0xFFFF0000u) >> 16u);");
  EmitLine("break;");
  Outdent();
  Outdent();
  EmitLine("}");
  EmitLine("return value;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Helper for shared memory load.
  // Checks xe_flags bit 0 to select between SRV (t0) and UAV (u0).
  EmitLine("uint XeSharedMemoryLoad(uint addr) {");
  Indent();
  EmitLine("if ((xe_flags & 1u) == 0u) {");
  Indent();
  EmitLine("return xe_shared_memory_srv.Load(addr);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("return xe_shared_memory_uav.Load(addr);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Multi-component shared memory loads using Load2/Load3/Load4.
  // These compile to single multi-component loads like DXBC's ld_raw.
  EmitLine("uint2 XeSharedMemoryLoad2(uint addr) {");
  Indent();
  EmitLine("if ((xe_flags & 1u) == 0u) {");
  Indent();
  EmitLine("return xe_shared_memory_srv.Load2(addr);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("return xe_shared_memory_uav.Load2(addr);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("uint3 XeSharedMemoryLoad3(uint addr) {");
  Indent();
  EmitLine("if ((xe_flags & 1u) == 0u) {");
  Indent();
  EmitLine("return xe_shared_memory_srv.Load3(addr);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("return xe_shared_memory_uav.Load3(addr);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("uint4 XeSharedMemoryLoad4(uint addr) {");
  Indent();
  EmitLine("if ((xe_flags & 1u) == 0u) {");
  Indent();
  EmitLine("return xe_shared_memory_srv.Load4(addr);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("return xe_shared_memory_uav.Load4(addr);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Helper for bool constant fetch.
  EmitLine("bool XeGetBoolConstant(uint index) {");
  Indent();
  EmitLine("uint vec_index = index >> 5u;");
  EmitLine("uint bit_index = index & 31u;");
  EmitLine(
      "return (xe_bool_loop_constants_data[vec_index >> 2u]"
      "[(vec_index & 3u)] & (1u << bit_index)) != 0u;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Helper for loop constant fetch.
  EmitLine("uint XeGetLoopConstant(uint index) {");
  Indent();
  EmitLine("uint vec_index = 8u + index;");
  EmitLine(
      "return xe_bool_loop_constants_data[vec_index >> 2u]"
      "[(vec_index & 3u)];");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeSetFloatSignBit(float value) {");
  Indent();
  EmitLine("return asfloat(asuint(value) | 0x80000000u);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // SM3-compliant multiply: 0 * anything = 0 (handles infinity/NaN edge cases).
  EmitLine("float XeMulSM3(float a, float b) {");
  Indent();
  EmitLine("return (min(abs(a), abs(b)) == 0.0) ? 0.0 : (a * b);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float2 XeMulSM3(float2 a, float2 b) {");
  Indent();
  EmitLine("float2 result = a * b;");
  EmitLine("bool2 isZero = (min(abs(a), abs(b)) == float2(0.0, 0.0));");
  EmitLine("return select(isZero, float2(0.0, 0.0), result);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float3 XeMulSM3(float3 a, float3 b) {");
  Indent();
  EmitLine("float3 result = a * b;");
  EmitLine("bool3 isZero = (min(abs(a), abs(b)) == float3(0.0, 0.0, 0.0));");
  EmitLine("return select(isZero, float3(0.0, 0.0, 0.0), result);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float4 XeMulSM3(float4 a, float4 b) {");
  Indent();
  EmitLine("float4 result = a * b;");
  EmitLine(
      "bool4 isZero = (min(abs(a), abs(b)) == float4(0.0, 0.0, 0.0, 0.0));");
  EmitLine("return select(isZero, float4(0.0, 0.0, 0.0, 0.0), result);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // SM3 dot product: each term obeys 0 * anything = 0, summed left to right
  // without fused multiply-add (matches DxbcShaderTranslator's dp# lowering).
  EmitLine("float XeDotSM3(float2 a, float2 b) {");
  Indent();
  EmitLine("float2 p = XeMulSM3(a, b);");
  EmitLine("return p.x + p.y;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeDotSM3(float3 a, float3 b) {");
  Indent();
  EmitLine("float3 p = XeMulSM3(a, b);");
  EmitLine("return (p.x + p.y) + p.z;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeDotSM3(float4 a, float4 b) {");
  Indent();
  EmitLine("float4 p = XeMulSM3(a, b);");
  EmitLine("return ((p.x + p.y) + p.z) + p.w;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_8_8_8_8 format (normalized unsigned).
  EmitLine("float4 XeUnpack8888(uint packed) {");
  Indent();
  EmitLine("return float4(packed & 0xFFu, (packed >> 8u) & 0xFFu,");
  EmitLine(
      "              (packed >> 16u) & 0xFFu, (packed >> 24u) & 0xFFu) / "
      "255.0;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_8_8_8_8 signed format.
  EmitLine("float4 XeUnpack8888Signed(uint packed) {");
  Indent();
  EmitLine("int4 unpacked = int4(packed & 0xFFu, (packed >> 8u) & 0xFFu,");
  EmitLine(
      "                     (packed >> 16u) & 0xFFu, (packed >> 24u) & "
      "0xFFu);");
  EmitLine("unpacked = select(unpacked >= 128, unpacked - 256, unpacked);");
  EmitLine(
      "return max(float4(unpacked) / 127.0, float4(-1.0, -1.0, -1.0, -1.0));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_2_10_10_10 format (normalized unsigned).
  EmitLine("float4 XeUnpack2101010(uint packed) {");
  Indent();
  EmitLine("return float4(packed & 0x3FFu, (packed >> 10u) & 0x3FFu,");
  EmitLine("              (packed >> 20u) & 0x3FFu, (packed >> 30u) & 0x3u) /");
  EmitLine("       float4(1023.0, 1023.0, 1023.0, 3.0);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_2_10_10_10 signed format.
  EmitLine("float4 XeUnpack2101010Signed(uint packed) {");
  Indent();
  EmitLine("int4 unpacked = int4(packed & 0x3FFu, (packed >> 10u) & 0x3FFu,");
  EmitLine(
      "                     (packed >> 20u) & 0x3FFu, (packed >> 30u) & "
      "0x3u);");
  EmitLine(
      "unpacked.xyz = select(unpacked.xyz >= 512, unpacked.xyz - 1024, "
      "unpacked.xyz);");
  EmitLine("unpacked.w = (unpacked.w >= 2) ? (unpacked.w - 4) : unpacked.w;");
  EmitLine("return max(float4(unpacked) / float4(511.0, 511.0, 511.0, 1.0),");
  EmitLine("           float4(-1.0, -1.0, -1.0, -1.0));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_10_11_11 format.
  EmitLine("float3 XeUnpack101111(uint packed) {");
  Indent();
  EmitLine("return float3(packed & 0x7FFu, (packed >> 11u) & 0x7FFu,");
  EmitLine(
      "              (packed >> 22u) & 0x3FFu) / float3(2047.0, 2047.0, "
      "1023.0);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_11_11_10 format.
  EmitLine("float3 XeUnpack111110(uint packed) {");
  Indent();
  EmitLine("return float3(packed & 0x3FFu, (packed >> 10u) & 0x7FFu,");
  EmitLine(
      "              (packed >> 21u) & 0x7FFu) / float3(1023.0, 2047.0, "
      "2047.0);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_16_16 format (normalized unsigned).
  EmitLine("float2 XeUnpack1616(uint packed) {");
  Indent();
  EmitLine(
      "return float2(packed & 0xFFFFu, (packed >> 16u) & 0xFFFFu) / 65535.0;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack k_16_16 signed format.
  EmitLine("float2 XeUnpack1616Signed(uint packed) {");
  Indent();
  EmitLine(
      "int2 unpacked = int2(packed & 0xFFFFu, (packed >> 16u) & 0xFFFFu);");
  EmitLine("unpacked = select(unpacked >= 32768, unpacked - 65536, unpacked);");
  EmitLine("return max(float2(unpacked) / 32767.0, float2(-1.0, -1.0));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Unpack half-float from uint.
  EmitLine("float XeUnpackFloat16(uint packed) {");
  Indent();
  EmitLine("return f16tof32(packed);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float2 XeUnpackFloat16x2(uint packed) {");
  Indent();
  EmitLine(
      "return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16u));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("int XeSignExtend(uint value, uint bits) {");
  Indent();
  EmitLine("uint mask = (1u << bits) - 1u;");
  EmitLine("uint sign_bit = 1u << (bits - 1u);");
  EmitLine("value &= mask;");
  EmitLine(
      "return ((value & sign_bit) != 0u) ? (int(value) - int(1u << bits)) : "
      "int(value);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeNormalizeUnsigned(uint value, uint bits) {");
  Indent();
  EmitLine("if (bits == 32u) { return float(value) * (1.0 / 4294967295.0); }");
  EmitLine("return float(value) / float((1u << bits) - 1u);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeNormalizeSignedZeroClampMinusOne(int value, uint bits) {");
  Indent();
  EmitLine(
      "float scale = (bits == 32u) ? (1.0 / 2147483647.0) : "
      "(1.0 / float((1u << (bits - 1u)) - 1u));");
  EmitLine("return max(float(value) * scale, -1.0);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeNormalizeSignedNoZero(int value, uint bits) {");
  Indent();
  EmitLine(
      "if (bits == 32u) { return float(value) * (1.0 / 2147483647.5) + "
      "(0.5 / 2147483647.5); }");
  EmitLine("float denom = float((1u << bits) - 1u);");
  EmitLine("return float(value) * (2.0 / denom) + (1.0 / denom);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine(
      "uint XeGetTextureFetchConstantWord(uint fetch_constant, uint word) {");
  Indent();
  EmitLine("uint dword_index = fetch_constant * 6u + word;");
  EmitLine(
      "return xe_fetch_constants_data[dword_index >> 2u][dword_index & 3u];");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("bool XeTextureFetchConstantIs3D(uint fetch_constant) {");
  Indent();
  EmitLine(
      "return (((XeGetTextureFetchConstantWord(fetch_constant, 5u) >> 9u) & "
      "3u) == 2u);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("uint XeGetTextureFetchDimension(uint fetch_constant) {");
  Indent();
  EmitLine(
      "return (XeGetTextureFetchConstantWord(fetch_constant, 5u) >> 9u) & 3u;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeGetTextureFetchSize1D(uint fetch_constant) {");
  Indent();
  EmitLine("uint word2 = XeGetTextureFetchConstantWord(fetch_constant, 2u);");
  EmitLine("return float((word2 & 0xFFFFFFu) + 1u);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float2 XeGetTextureFetchSize2D(uint fetch_constant) {");
  Indent();
  EmitLine("uint word2 = XeGetTextureFetchConstantWord(fetch_constant, 2u);");
  EmitLine(
      "return float2(float((word2 & 0x1FFFu) + 1u), "
      "float(((word2 >> 13u) & 0x1FFFu) + 1u));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float3 XeGetTextureFetchSize3DOrStacked(uint fetch_constant) {");
  Indent();
  EmitLine("uint word2 = XeGetTextureFetchConstantWord(fetch_constant, 2u);");
  EmitLine("if (XeTextureFetchConstantIs3D(fetch_constant)) {");
  Indent();
  EmitLine(
      "return float3(float((word2 & 0x7FFu) + 1u), "
      "float(((word2 >> 11u) & 0x7FFu) + 1u), "
      "float(((word2 >> 22u) & 0x3FFu) + 1u));");
  Outdent();
  EmitLine("}");
  EmitLine(
      "return float3(float((word2 & 0x1FFFu) + 1u), "
      "float(((word2 >> 13u) & 0x1FFFu) + 1u), "
      "float(((word2 >> 26u) & 0x3Fu) + 1u));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float3 XeTextureCubeDirection(float3 coord) {");
  Indent();
  EmitLine("float2 st = coord.xy * 2.0f - 3.0f;");
  EmitLine("uint face = min(uint(coord.z), 5u);");
  EmitLine("uint axis = face >> 1u;");
  EmitLine("bool negative = (face & 1u) != 0u;");
  EmitLine("if (axis == 0u) {");
  Indent();
  EmitLine(
      "return float3(negative ? -1.0f : 1.0f, -st.y, "
      "negative ? st.x : -st.x);");
  Outdent();
  EmitLine("}");
  EmitLine("if (axis == 1u) {");
  Indent();
  EmitLine(
      "return float3(st.x, negative ? -1.0f : 1.0f, "
      "negative ? -st.y : st.y);");
  Outdent();
  EmitLine("}");
  EmitLine(
      "return float3(negative ? -st.x : st.x, -st.y, "
      "negative ? -1.0f : 1.0f);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeGetTextureFetchLodBias(uint fetch_constant) {");
  Indent();
  EmitLine("uint word4 = XeGetTextureFetchConstantWord(fetch_constant, 4u);");
  EmitLine(
      "return float(XeSignExtend((word4 >> 12u) & 0x3FFu, 10u)) * (1.0f / "
      "32.0f);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeGetTextureFetchExpAdjust(uint fetch_constant) {");
  Indent();
  EmitLine("uint word3 = XeGetTextureFetchConstantWord(fetch_constant, 3u);");
  EmitLine("return exp2(float(XeSignExtend((word3 >> 13u) & 0x3Fu, 6u)));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeSaturateNoNaN(float value) {");
  Indent();
  EmitLine("float clamped = clamp(value, 0.0f, 1.0f);");
  EmitLine("return (clamped == clamped) ? clamped : 0.0f;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine(
      "uint XePreClampedDepthTo20e4(float depth, bool round_to_nearest_even, "
      "bool remap_from_0_to_0_5) {");
  Indent();
  EmitLine("uint f32 = asuint(depth);");
  EmitLine("uint remap_bias = remap_from_0_to_0_5 ? 1u : 0u;");
  EmitLine("uint biased_f32;");
  EmitLine("if (f32 < (0x38800000u - (remap_bias << 23u))) {");
  Indent();
  EmitLine("uint shift = min((113u - remap_bias) - (f32 >> 23u), 24u);");
  EmitLine("biased_f32 = (((f32 & 0x7FFFFFu) | 0x800000u) >> shift);");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("biased_f32 = f32 + (0xC8000000u + (remap_bias << 23u));");
  Outdent();
  EmitLine("}");
  EmitLine("if (round_to_nearest_even) {");
  Indent();
  EmitLine("biased_f32 += 3u + ((biased_f32 >> 3u) & 1u);");
  Outdent();
  EmitLine("}");
  EmitLine("return (biased_f32 >> 3u) & 0xFFFFFFu;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeDepth20e4To32(uint f24, bool remap_to_0_to_0_5) {");
  Indent();
  EmitLine("uint remap_bias = remap_to_0_to_0_5 ? 1u : 0u;");
  EmitLine("uint exponent = (f24 >> 20u) & 0xFu;");
  EmitLine("uint mantissa = f24 & 0xFFFFFu;");
  EmitLine("int exponent_signed = int(exponent);");
  EmitLine("if (exponent == 0u) {");
  Indent();
  EmitLine("if (mantissa != 0u) {");
  Indent();
  EmitLine("int shift = 20 - int(firstbithigh(mantissa));");
  EmitLine("mantissa <<= uint(shift);");
  EmitLine("exponent_signed = 1 - shift;");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine("exponent_signed = -int(112u - remap_bias);");
  Outdent();
  EmitLine("}");
  Outdent();
  EmitLine("}");
  EmitLine(
      "uint exponent_bits = uint(exponent_signed + int(112u - remap_bias)) << "
      "23u;");
  EmitLine("return asfloat(exponent_bits | ((mantissa & 0xFFFFFu) << 3u));");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeDepthFloat24TruncateToHost(float depth) {");
  Indent();
  EmitLine("depth = XeSaturateNoNaN(depth);");
  EmitLine("uint depth_uint = asuint(depth);");
  EmitLine("if (depth_uint < 0x2E800000u) { return 0.0f; }");
  EmitLine("uint exponent = (depth_uint >> 23u) & 0xFFu;");
  EmitLine("int trunc_bits_signed = max(116 - int(exponent), 3);");
  EmitLine("uint trunc_bits = uint(trunc_bits_signed);");
  EmitLine("uint trunc_mask = ~((1u << trunc_bits) - 1u);");
  EmitLine("return asfloat(depth_uint & trunc_mask) * 0.5f;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XeDepthFloat24RoundToHost(float depth) {");
  Indent();
  EmitLine("depth = XeSaturateNoNaN(depth);");
  EmitLine("uint f24 = XePreClampedDepthTo20e4(depth, true, false);");
  EmitLine("return XeDepth20e4To32(f24, true);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XePWLGammaToLinear(float value) {");
  Indent();
  EmitLine("float clamped = XeSaturateNoNaN(value);");
  EmitLine("bool piece_at_least_2 = clamped >= (96.0f / 255.0f);");
  EmitLine("bool piece_at_least_3 = clamped >= (192.0f / 255.0f);");
  EmitLine("bool piece_at_least_1 = clamped >= (64.0f / 255.0f);");
  EmitLine(
      "float scale = piece_at_least_2 ? (piece_at_least_3 ? (8.0f / "
      "1024.0f) : (4.0f / 1024.0f)) : (piece_at_least_1 ? (2.0f / "
      "1024.0f) : (1.0f / 1024.0f));");
  EmitLine(
      "float offset = piece_at_least_2 ? (piece_at_least_3 ? -1024.0f : "
      "-256.0f) : (piece_at_least_1 ? -64.0f : 0.0f);");
  EmitLine(
      "float linear_value = clamped * (255.0f * 1024.0f) * scale + offset;");
  EmitLine("linear_value += trunc(linear_value * scale);");
  EmitLine("return linear_value * (1.0f / 1023.0f);");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine("float XePreSaturatedLinearToPWLGamma(float value) {");
  Indent();
  EmitLine("// value must be pre-saturated linear in [0, 1].");
  EmitLine("bool piece_at_least_2 = value >= (128.0f / 1023.0f);");
  EmitLine("bool piece_at_least_3 = value >= (512.0f / 1023.0f);");
  EmitLine("bool piece_at_least_1 = value >= (64.0f / 1023.0f);");
  EmitLine(
      "float scale = piece_at_least_2 ? (piece_at_least_3 ? (1023.0f / 8.0f) : "
      "(1023.0f / 4.0f)) : (piece_at_least_1 ? (1023.0f / 2.0f) : 1023.0f);");
  EmitLine(
      "float offset = piece_at_least_2 ? (piece_at_least_3 ? (128.0f / 255.0f) "
      ": (64.0f / 255.0f)) : (piece_at_least_1 ? (32.0f / 255.0f) : 0.0f);");
  EmitLine("return trunc(value * scale) * (1.0f / 255.0f) + offset;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  EmitLine(
      "float XeApplyTextureSign(float unsigned_value, float signed_value, uint "
      "sign) {");
  Indent();
  EmitLine("if (sign == 1u) { return signed_value; }");
  EmitLine("if (sign == 2u) { return unsigned_value * 2.0f - 1.0f; }");
  EmitLine("if (sign == 3u) { return XePWLGammaToLinear(unsigned_value); }");
  EmitLine("return unsigned_value;");
  Outdent();
  EmitLine("}");
  EmitLine("");

  // Bindless resource helper functions.
  if (bindless_resources_used_) {
    // Guest texture SRVs start after the system descriptors in the view heap.
    // The command processor stores relative indices so add the offset back.
    EmitLine("static const uint kXeResourceDescriptorHeapStart = " +
             std::to_string(bindless_srv_heap_offset_) + "u;");
    EmitLine("");

    // Helper to get descriptor index from the descriptor indices constant
    // buffer. The descriptor indices are packed as uint values in uint4
    // vectors.
    EmitLine("uint XeGetDescriptorIndex(uint slot) {");
    Indent();
    EmitLine("uint vec_index = slot >> 2u;");
    EmitLine("uint component = slot & 3u;");
    EmitLine("return xe_descriptor_indices_data[vec_index][component];");
    Outdent();
    EmitLine("}");
    EmitLine("");
  }

  // Memory export helpers (after XeEndianSwap, which they reuse).
  if (MemExportUsed()) {
    EmitMemExportHelpers();
  }
}

void HlslShaderTranslator::EmitDomainShaderPrologue() {
  // Mirrors DxbcShaderTranslator::StartVertexOrDomainShader. Copies the domain
  // location and the host-provided control point indices into the guest
  // registers. Direct3D 12 passes coordinates in a consistent order, so the
  // per-edge swizzle the guest expects is left as identity (0.0).
  uint32_t reg_count = register_count();
  if (!reg_count) {
    return;
  }
  auto reg = [&](uint32_t i) {
    return RegisterToHlsl(i, InstructionStorageAddressingMode::kAbsolute);
  };
  switch (GetHlslShaderModification().vertex.host_vertex_shader_type) {
    case Shader::HostVertexShaderType::kTriangleDomainCPIndexed:
      EmitLine(reg(0) + ".xyz = xe_domain_location.zyx;");
      if (reg_count >= 2) {
        EmitLine(reg(1) +
                 ".xyz = float3(xe_control_points[0].index, "
                 "xe_control_points[1].index, xe_control_points[2].index);");
      }
      break;
    case Shader::HostVertexShaderType::kTriangleDomainPatchIndexed:
      EmitLine(reg(0) + ".xyz = xe_domain_location.zyx;");
      if (reg_count >= 2) {
        EmitLine(reg(1) + ".x = xe_control_points[0].index;");
        EmitLine(reg(1) + ".y = 0.0;");
      }
      break;
    case Shader::HostVertexShaderType::kQuadDomainCPIndexed:
      EmitLine(reg(0) + ".xy = xe_domain_location.xy;");
      EmitLine(reg(0) + ".z = xe_control_points[0].index;");
      if (reg_count >= 2) {
        EmitLine(reg(1) +
                 ".xyz = float3(xe_control_points[1].index, "
                 "xe_control_points[2].index, xe_control_points[3].index);");
      }
      break;
    case Shader::HostVertexShaderType::kQuadDomainPatchIndexed:
      EmitLine(reg(0) + ".x = xe_control_points[0].index;");
      EmitLine(reg(0) + ".yz = xe_domain_location.xy;");
      if (reg_count >= 2) {
        EmitLine(reg(1) + ".x = 0.0;");
      }
      break;
    default:
      EmitTranslationError(
          "Unsupported host vertex shader type in domain shader prologue");
      break;
  }
  EmitLine("");
}

void HlslShaderTranslator::EmitGlobalState() {
  // Per-invocation guest state shared between main() and (Stage 1) subroutine
  // functions. Declared without initializers; main() initializes everything
  // per invocation. The program counter is NOT here - it stays local to each
  // function's own control-flow loop.
  EmitLine(is_vertex_shader() ? "static VSOutput output;"
                              : "static PSOutput output;");
  if (is_vertex_shader() &&
      current_shader().writes_point_size_edge_flag_kill_vertex()) {
    EmitLine("static float4 xe_point_size_edge_flag_kill_vertex;");
  }
  uint32_t reg_count = register_count();
  if (reg_count > 0) {
    EmitLine("static float4 xe_gprs[" + std::to_string(reg_count) + "];");
  }
  EmitLine("static float4 xe_ps; // Previous scalar");
  EmitLine("static float4 xe_salu_src0; // Scalar operand 0 snapshot");
  EmitLine("static float xe_salu_src1; // Scalar operand 1 snapshot");
  EmitLine("static bool xe_p0; // Predicate");
  EmitLine("static int xe_a0; // Address register");
  EmitLine("static int4 xe_aL; // Loop address stack");
  EmitLine("static uint4 xe_loop_count; // Loop count stack");
  EmitLine(
      "static uint xe_vfetch_address; // Vertex fetch address for mini-fetch");
  EmitLine("static float xe_texture_lod; // Explicit texture LOD state");
  EmitLine("static float3 xe_texture_grad_h; // Explicit horizontal gradient");
  EmitLine("static float3 xe_texture_grad_v; // Explicit vertical gradient");
  if (MemExportUsed()) {
    EmitLine("static float4 xe_eA; // Export address");
    EmitLine("static float4 xe_eM[5];");
    EmitLine("static uint xe_eM_written; // eM# written this invocation");
    EmitLine("static bool xe_memexport_enabled;");
  }
  EmitLine("");
}

void HlslShaderTranslator::EmitEntryPointBegin() {
  if (is_vertex_shader()) {
    if (IsDomainShader()) {
      const char* domain = "quad";
      uint32_t control_point_count = 1;
      uint32_t domain_location_component_count = 2;
      GetDomainShaderInfo(domain, control_point_count,
                          domain_location_component_count);
      EmitLine(std::string("[domain(\"") + domain + "\")]");
      EmitLine("VSOutput main(XeHSConstantDataOutput xe_patch_constant,");
      EmitLine("              float" +
               std::to_string(domain_location_component_count) +
               " xe_domain_location : SV_DomainLocation,");
      EmitLine("              const OutputPatch<XeHSControlPointOutput, " +
               std::to_string(control_point_count) + "> xe_control_points) {");
    } else {
      EmitLine("VSOutput main(VSInput input) {");
    }
  } else {
    if (IsForceEarlyDepthStencilEnabled()) {
      EmitLine("[earlydepthstencil]");
    }
    EmitLine("PSOutput main(PSInput input) {");
  }
  Indent();

  // Initialize the output (a static global) for this invocation.
  if (is_vertex_shader()) {
    EmitLine("output = (VSOutput)0;");
    Modification modification = GetHlslShaderModification();
    if (modification.vertex.output_point_size) {
      // Negative X means the point-list expansion shader should use the
      // constant point size unless the guest shader overwrites it.
      EmitLine("output.xe_point_parameters = float3(-1.0, 0.0, 0.0);");
    }
    if (current_shader().writes_point_size_edge_flag_kill_vertex()) {
      EmitLine(
          "xe_point_size_edge_flag_kill_vertex = float4(-1.0, 0.0, 0.0, 0.0);");
    }
  } else {
    EmitLine("output = (PSOutput)0;");
    if (PixelShaderNeedsCoverageOutput()) {
      EmitLine("output.xe_coverage = 0xFFFFFFFFu;");
    }
    // The polygon-offset depth slope must be derived here, before any
    // kill/discard, while the 2x2 quad is still fully populated.
    if (PixelShaderAppliesPolygonOffset() && !current_shader().writes_depth()) {
      EmitLine(
          "float xe_poly_offset_slope = "
          "max(abs(ddx_coarse(input.xe_position.z)), "
          "abs(ddy_coarse(input.xe_position.z)));");
    }
  }
  EmitLine("");

  // Zero the general-purpose registers.
  uint32_t reg_count = register_count();
  if (reg_count > 0) {
    for (uint32_t i = 0; i < reg_count; ++i) {
      EmitLine("xe_gprs[" + std::to_string(i) +
               "] = float4(0.0, 0.0, 0.0, 0.0);");
    }
    EmitLine("");
  }

  // Initialize special registers. xe_pc is local to this function's control
  // flow loop, not shared state.
  EmitLine("int xe_pc = 0; // Program counter");
  if (current_shader().uses_subroutine_calls()) {
    EmitLine("uint xe_call_stack[8]; // Subroutine return-address stack");
    EmitLine("uint xe_call_sp = 0u;");
  }
  EmitLine("xe_ps = float4(0.0, 0.0, 0.0, 0.0);");
  EmitLine("xe_p0 = false;");
  EmitLine("xe_a0 = 0;");
  EmitLine("xe_aL = int4(0, 0, 0, 0);");
  EmitLine("xe_loop_count = uint4(0u, 0u, 0u, 0u);");
  EmitLine("xe_vfetch_address = 0u;");
  EmitLine("xe_texture_lod = 0.0;");
  EmitLine("xe_texture_grad_h = float3(0.0, 0.0, 0.0);");
  EmitLine("xe_texture_grad_v = float3(0.0, 0.0, 0.0);");
  EmitLine("");

  // Initialize memory export (memexport) state.
  if (MemExportUsed()) {
    EmitLine("xe_eA = float4(0.0, 0.0, 0.0, 0.0);");
    for (uint32_t i = 0; i < 5; ++i) {
      EmitLine("xe_eM[" + std::to_string(i) +
               "] = float4(0.0, 0.0, 0.0, 0.0);");
    }
    EmitLine("xe_eM_written = 0u;");
    // Enabled only when shared memory is bound as a UAV (xe_flags bit 0).
    EmitLine("xe_memexport_enabled = (xe_flags & 1u) != 0u;");
    // With resolution scaling, a guest pixel covers multiple host pixels, each
    // its own pixel-shader invocation. Export from only the host pixel nearest
    // the guest pixel center (scale>>1, covered under the top-left rule),
    // matching DxbcShaderTranslator.
    if (is_pixel_shader() &&
        (draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1)) {
      std::string cond;
      if (draw_resolution_scale_x_ > 1) {
        cond = "(uint(input.xe_position.x) % " +
               std::to_string(draw_resolution_scale_x_) +
               "u == " + std::to_string(draw_resolution_scale_x_ >> 1) + "u)";
      }
      if (draw_resolution_scale_y_ > 1) {
        std::string cond_y =
            "(uint(input.xe_position.y) % " +
            std::to_string(draw_resolution_scale_y_) +
            "u == " + std::to_string(draw_resolution_scale_y_ >> 1) + "u)";
        cond = cond.empty() ? cond_y : (cond + " && " + cond_y);
      }
      EmitLine("xe_memexport_enabled = xe_memexport_enabled && " + cond + ";");
    }
    // At sample rate the shader runs once per covered sample; export from only
    // the first covered sample so a guest pixel exports once. firstbitlow of
    // zero coverage is 0xFFFFFFFF, which no sample index matches. Mirrors DXBC.
    if (IsSampleRate()) {
      EmitLine(
          "xe_memexport_enabled = xe_memexport_enabled && "
          "(xe_sample_index == firstbitlow(xe_coverage_in));");
    }
    EmitLine("");
  }
}

void HlslShaderTranslator::EmitEntryPointEnd() {
  // The output struct was initialized to zero in EmitEntryPointBegin.
  // Shader ALU instructions write to outputs via storage targets
  // (kPosition, kInterpolator, kColor, kDepth).
  EmitLine("");

  // Flush any memory exports written since the last alloc.
  if (MemExportUsed()) {
    EmitMemExportFlush();
    EmitLine("");
  }

  // For vertex shaders, apply position fixups and NDC transformation.
  // This converts from Xbox 360 clip space to D3D clip space.
  if (is_vertex_shader()) {
    // System flags for position transformation (matching DXBC translator):
    // kSysFlag_XYDividedByW = 1 << 1 = 2
    // kSysFlag_ZDividedByW = 1 << 2 = 4
    // kSysFlag_WNotReciprocal = 1 << 3 = 8

    // If W is 1/W (WNotReciprocal flag NOT set), convert to W.
    EmitLine("// Convert W from 1/W to W if needed");
    EmitLine("if ((xe_flags & 8u) == 0u) {");
    Indent();
    EmitLine("output.xe_position.w = 1.0 / output.xe_position.w;");
    Outdent();
    EmitLine("}");

    // If XY is divided by W (XYDividedByW flag set), multiply by W.
    EmitLine("// Multiply XY by W if shader outputs XY/W");
    EmitLine("if ((xe_flags & 2u) != 0u) {");
    Indent();
    EmitLine("output.xe_position.xy *= output.xe_position.w;");
    Outdent();
    EmitLine("}");

    // If Z is divided by W (ZDividedByW flag set), multiply by W.
    EmitLine("// Multiply Z by W if shader outputs Z/W");
    EmitLine("if ((xe_flags & 4u) != 0u) {");
    Indent();
    EmitLine("output.xe_position.z *= output.xe_position.w;");
    Outdent();
    EmitLine("}");

    Modification modification = GetHlslShaderModification();
    auto distance_component = [](bool cull, uint32_t distance_count,
                                 uint32_t index) {
      const char* components = "xyzw";
      std::string field =
          cull ? "output.xe_cull_distance" : "output.xe_clip_distance";
      if (distance_count <= 4) {
        if (distance_count == 1) {
          return field;
        }
        return field + "." + components[index];
      }
      if (index < 4) {
        return field + "_0123." + components[index];
      }
      uint32_t tail_index = index - 4;
      if (distance_count - 4 == 1) {
        return field + "_45";
      }
      return field + "_45." + components[tail_index];
    };
    auto emit_distance_assignment = [&](bool cull, uint32_t index,
                                        const std::string& value) {
      uint32_t distance_count = cull
                                    ? modification.GetVertexCullDistanceCount()
                                    : modification.GetVertexClipDistanceCount();
      EmitLine(distance_component(cull, distance_count, index) + " = " + value +
               ";");
    };

    uint32_t clip_distance_next_component = 0;
    uint32_t cull_distance_next_component = 0;
    if (modification.vertex.user_clip_plane_count) {
      EmitLine("// Emit user clip/cull distances before NDC transform");
      EmitLine("float4 xe_guest_clip_position = output.xe_position;");
      for (uint32_t i = 0; i < modification.vertex.user_clip_plane_count; ++i) {
        std::string distance =
            "dot(xe_guest_clip_position, xe_user_clip_planes[" +
            std::to_string(i) + "])";
        if (modification.vertex.user_clip_plane_cull) {
          emit_distance_assignment(true, cull_distance_next_component++,
                                   distance);
        } else {
          emit_distance_assignment(false, clip_distance_next_component++,
                                   distance);
        }
      }
      EmitLine("");
    }

    // Apply NDC scale and offset for viewport transformation.
    EmitLine("// Apply NDC scale and offset for viewport transformation");
    EmitLine("output.xe_position.xyz *= xe_ndc_scale;");
    EmitLine("output.xe_position.xyz += xe_ndc_offset * output.xe_position.w;");
    EmitLine("");

    bool shader_writes_vertex_kill =
        (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b100) !=
        0;
    if (shader_writes_vertex_kill) {
      EmitLine(
          "uint xe_vertex_kill_bits = "
          "asuint(xe_point_size_edge_flag_kill_vertex.z) & 0x7FFFFFFFu;");
    }
    if (modification.vertex.vertex_kill_and) {
      emit_distance_assignment(true, cull_distance_next_component++,
                               shader_writes_vertex_kill
                                   ? "(xe_vertex_kill_bits != 0u) ? -1.0 : 0.0"
                                   : "0.0");
      EmitLine("");
    } else if (shader_writes_vertex_kill) {
      EmitLine("if (xe_vertex_kill_bits != 0u) {");
      Indent();
      EmitLine("output.xe_position.w = asfloat(0x7FC00000u);");
      Outdent();
      EmitLine("}");
      EmitLine("");
    }

    if (modification.vertex.output_point_size &&
        (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b001)) {
      EmitLine(
          "output.xe_point_parameters.x = "
          "xe_point_size_edge_flag_kill_vertex.x;");
      EmitLine("");
    }
  }

  // For pixel shaders, apply color exponent bias to color outputs.
  // This matches DXBC: mul r2.xyzw, r2.xyzw, CB0[0][15].xxxx
  if (is_pixel_shader()) {
    EmitPixelShaderAlphaTest();
    EmitPixelShaderAlphaToCoverage();

    uint32_t color_targets_written = current_shader().writes_color_targets();
    if (color_targets_written) {
      EmitLine("// Apply color exponent bias");
      if (color_targets_written & (1u << 0)) {
        EmitLine("output.xe_color_0 *= xe_color_exp_bias.x;");
      }
      if (color_targets_written & (1u << 1)) {
        EmitLine("output.xe_color_1 *= xe_color_exp_bias.y;");
      }
      if (color_targets_written & (1u << 2)) {
        EmitLine("output.xe_color_2 *= xe_color_exp_bias.z;");
      }
      if (color_targets_written & (1u << 3)) {
        EmitLine("output.xe_color_3 *= xe_color_exp_bias.w;");
      }
      EmitLine("");
      // Encode linear color to PWL gamma for gamma render targets after the
      // exponent bias, matching the DxbcShaderTranslator output merger. Only
      // when the host gamma render target is unorm8. Higher precision hosts
      // keep the linear value to avoid 8-bit banding.
      for (uint32_t i = 0; i < 4; ++i) {
        if (!gamma_render_target_as_unorm8_ ||
            !(color_targets_written & (1u << i))) {
          continue;
        }
        std::string color = "output.xe_color_" + std::to_string(i);
        // kSysFlag_ConvertColor0ToGamma_Shift == 10, one bit per color target.
        EmitLine("if ((xe_flags & " + std::to_string(1u << (10 + i)) +
                 "u) != 0u) {");
        Indent();
        EmitLine(color + ".rgb = saturate(" + color + ".rgb);");
        EmitLine(color + ".r = XePreSaturatedLinearToPWLGamma(" + color +
                 ".r);");
        EmitLine(color + ".g = XePreSaturatedLinearToPWLGamma(" + color +
                 ".g);");
        EmitLine(color + ".b = XePreSaturatedLinearToPWLGamma(" + color +
                 ".b);");
        Outdent();
        EmitLine("}");
      }
      EmitLine("");

      // For RT0 with a MIN/MAX blend op, pre-multiply by the source blend
      // factor: D3D12 MIN/MAX ignores blend factors, but the Xbox 360 applies
      // them. Matches DxbcShaderTranslator's output merger. The factor is
      // kOne (no-op) unless the command processor selected a MIN/MAX blend op.
      if ((color_targets_written & 1u) && !edram_rov_used_) {
        Modification modification = GetHlslShaderModification();
        xenos::BlendFactor rgb_factor =
            modification.pixel.rt0_blend_rgb_factor_for_premult;
        xenos::BlendFactor a_factor =
            modification.pixel.rt0_blend_a_factor_for_premult;
        if (rgb_factor != xenos::BlendFactor::kOne ||
            a_factor != xenos::BlendFactor::kOne) {
          const std::string c = "output.xe_color_0";
          switch (rgb_factor) {
            case xenos::BlendFactor::kZero:
              EmitLine(c + ".rgb = float3(0.0, 0.0, 0.0);");
              break;
            case xenos::BlendFactor::kSrcColor:
              EmitLine(c + ".rgb *= " + c + ".rgb;");
              break;
            case xenos::BlendFactor::kOneMinusSrcColor:
              EmitLine(c + ".rgb *= (1.0 - " + c + ".rgb);");
              break;
            case xenos::BlendFactor::kSrcAlpha:
              EmitLine(c + ".rgb *= " + c + ".a;");
              break;
            case xenos::BlendFactor::kOneMinusSrcAlpha:
              EmitLine(c + ".rgb *= (1.0 - " + c + ".a);");
              break;
            case xenos::BlendFactor::kConstantColor:
              EmitLine(c + ".rgb *= xe_edram_blend_constant.rgb;");
              break;
            case xenos::BlendFactor::kOneMinusConstantColor:
              EmitLine(c + ".rgb *= (1.0 - xe_edram_blend_constant.rgb);");
              break;
            case xenos::BlendFactor::kConstantAlpha:
              EmitLine(c + ".rgb *= xe_edram_blend_constant.a;");
              break;
            case xenos::BlendFactor::kOneMinusConstantAlpha:
              EmitLine(c + ".rgb *= (1.0 - xe_edram_blend_constant.a);");
              break;
            default:
              // kOne or a dst-based/unsupported factor - no pre-multiply.
              break;
          }
          switch (a_factor) {
            case xenos::BlendFactor::kZero:
              EmitLine(c + ".a = 0.0;");
              break;
            case xenos::BlendFactor::kSrcColor:
            case xenos::BlendFactor::kSrcAlpha:
              EmitLine(c + ".a *= " + c + ".a;");
              break;
            case xenos::BlendFactor::kOneMinusSrcColor:
            case xenos::BlendFactor::kOneMinusSrcAlpha:
              EmitLine(c + ".a *= (1.0 - " + c + ".a);");
              break;
            case xenos::BlendFactor::kConstantColor:
            case xenos::BlendFactor::kConstantAlpha:
              EmitLine(c + ".a *= xe_edram_blend_constant.a;");
              break;
            case xenos::BlendFactor::kOneMinusConstantColor:
            case xenos::BlendFactor::kOneMinusConstantAlpha:
              EmitLine(c + ".a *= (1.0 - xe_edram_blend_constant.a);");
              break;
            default:
              // kOne, kSrcAlphaSaturate (1.0 for alpha), or unsupported.
              break;
          }
          EmitLine("");
        }
      }
    }

    Modification modification = GetHlslShaderModification();
    Modification::DepthStencilMode depth_stencil_mode =
        modification.pixel.depth_stencil_mode;
    bool needs_float24_depth = PixelShaderNeedsFloat24DepthOutput();
    // Polygon offset is applied via shader depth output only when the shader
    // doesn't write its own depth, matching the DxbcShaderTranslator output
    // merger.
    bool apply_polygon_offset =
        PixelShaderAppliesPolygonOffset() && !current_shader().writes_depth();
    if (apply_polygon_offset) {
      // biased = depth + max(|ddx(z)|, |ddy(z)|) * slope + bias, per faceness.
      // xe_poly_offset_slope was captured at shader start, before any discard.
      EmitLine(
          "float2 xe_poly_offset = input.xe_is_front_face ? "
          "xe_edram_poly_offset_front : xe_edram_poly_offset_back;");
      EmitLine(
          "float xe_depth_biased = input.xe_position.z + "
          "xe_poly_offset_slope * xe_poly_offset.x + xe_poly_offset.y;");
    }
    if (needs_float24_depth) {
      if (current_shader().writes_depth()) {
        EmitLine("float xe_depth_guest = output.xe_depth;");
      } else if (apply_polygon_offset) {
        EmitLine(
            "float xe_depth_guest = XeSaturateNoNaN(xe_depth_biased * 2.0f);");
      } else {
        EmitLine(
            "float xe_depth_guest = XeSaturateNoNaN(input.xe_position.z * "
            "2.0f);");
      }
      if (depth_stencil_mode ==
              Modification::DepthStencilMode::kFloat24Truncating ||
          depth_stencil_mode ==
              Modification::DepthStencilMode::kFloat24TruncatingPolygonOffset) {
        EmitLine(
            "output.xe_depth = XeDepthFloat24TruncateToHost(xe_depth_guest);");
      } else {
        EmitLine(
            "output.xe_depth = XeDepthFloat24RoundToHost(xe_depth_guest);");
      }
      EmitLine("");
    } else if (current_shader().writes_depth()) {
      EmitLine("if ((xe_flags & 64u) != 0u) {");
      Indent();
      EmitLine("output.xe_depth *= 0.5f;");
      Outdent();
      EmitLine("}");
      EmitLine("");
    } else if (apply_polygon_offset) {
      // kPolygonOffset (no float24 conversion) - write the biased host depth.
      EmitLine("output.xe_depth = xe_depth_biased;");
      EmitLine("");
    }
  }

  EmitLine("return output;");
  Outdent();
  EmitLine("}");
}

void HlslShaderTranslator::StartTranslation() {
  // Emit all declarations.
  EmitLine("// Generated HLSL shader - Xenia Xbox 360 Emulator");
  EmitLine("// Shader Model 6.6");
  EmitLine("");

  EmitSystemConstants();
  EmitConstantBuffers();
  EmitResourceDeclarations();
  EmitInputDeclarations();
  EmitOutputDeclarations();
  EmitHelperFunctions();
  EmitGlobalState();
  EmitEntryPointBegin();

  // Initialize vertex shader with vertex index.
  if (is_vertex_shader()) {
    if (IsDomainShader()) {
      EmitDomainShaderPrologue();
    } else {
      EmitLine("// Load vertex index into r0.x");
      EmitLine("uint xe_vertex_index = input.xe_vertex_id;");
      EmitLine(
          "xe_vertex_index = (xe_vertex_index != xe_line_loop_closing_index) ? "
          "xe_vertex_index : 0u;");
      EmitLine(
          "xe_vertex_index = XeEndianSwap(xe_vertex_index, "
          "xe_vertex_index_endian);");
      EmitLine(
          "xe_vertex_index = (xe_vertex_index + xe_vertex_index_offset) & "
          "0x00FFFFFFu;");
      EmitLine(
          "xe_vertex_index = clamp(xe_vertex_index, xe_vertex_index_min_max.x, "
          "xe_vertex_index_min_max.y);");
      if (register_count()) {
        EmitLine(
            RegisterToHlsl(0, InstructionStorageAddressingMode::kAbsolute) +
            ".x = float(xe_vertex_index);");
      }
      EmitLine("");
    }
  } else {
    // Pixel shader: Load interpolated values from input struct into registers.
    // In Xenos, interpolators map directly to general-purpose registers.
    // Interpolator N in the VS output -> register N in the PS.
    Modification modification = GetHlslShaderModification();
    uint32_t interpolator_mask = modification.pixel.interpolator_mask;
    uint32_t param_gen_interpolator =
        (modification.pixel.param_gen_enable &&
         modification.pixel.param_gen_interpolator < register_count())
            ? modification.pixel.param_gen_interpolator
            : UINT32_MAX;
    bool precise = UsePreciseInterpolation();
    bool any_loaded = false;
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      if (interpolator_mask & (1u << i)) {
        if (i < register_count()) {
          if (i == param_gen_interpolator) {
            continue;
          }
          std::string reg =
              RegisterToHlsl(i, InstructionStorageAddressingMode::kAbsolute);
          std::string in = "input.xe_interpolator_" + std::to_string(i);
          if (precise) {
            // Manual v0-anchor barycentric interpolation (matches the SPIR-V
            // path): v0 + (v1 - v0) * bary.y + (v2 - v0) * bary.z. Exact when
            // all vertices are equal, avoiding hardware interpolation noise.
            EmitLine("{");
            Indent();
            EmitLine("float4 xe_v0 = GetAttributeAtVertex(" + in + ", 0);");
            EmitLine("float4 xe_v1 = GetAttributeAtVertex(" + in + ", 1);");
            EmitLine("float4 xe_v2 = GetAttributeAtVertex(" + in + ", 2);");
            EmitLine(reg +
                     " = xe_v0 + (xe_v1 - xe_v0) * input.xe_barycentrics.y + "
                     "(xe_v2 - xe_v0) * input.xe_barycentrics.z;");
            Outdent();
            EmitLine("}");
          } else {
            EmitLine(reg + " = " + in + ";");
          }
          any_loaded = true;
        }
      }
    }
    EmitPixelShaderParamGen();
    if (any_loaded) {
      EmitLine("");
    }
  }

  // Start the main control flow loop.
  // This implements a state machine pattern using a program counter and switch
  // statement, which is compatible with HLSL (unlike goto/labels).
  // Only use the state machine if there are labels (jump targets).
  has_main_switch_ = !current_shader().label_addresses().empty();
  if (has_main_switch_) {
    EmitLine("[loop] while (true) {");
    Indent();
    EmitLine("[branch] switch (xe_pc) {");
    Indent();
    EmitLine("case 0:");
    Indent();
  }
  // For shaders without labels, we emit code directly without the while/switch.
}

std::vector<uint8_t> HlslShaderTranslator::CompleteTranslation() {
  // Close any remaining exec conditionals.
  CloseExecConditionals();

  // Close the state machine if we used one.
  if (has_main_switch_) {
    // Fallthrough from the last case - break out of the switch.
    EmitLine("break;");
    Outdent();
    EmitLine("default:");
    Indent();
    EmitLine("break;");
    Outdent();
    Outdent();
    EmitLine("}");       // Close switch
    EmitLine("break;");  // Exit while loop
    Outdent();
    EmitLine("}");  // Close while
  }

  EmitEntryPointEnd();

  // Store the generated HLSL.
  hlsl_source_ = hlsl_stream_.str();

  // Splice in the bindful texture and sampler declarations now that the binding
  // lists are complete. One SRV per binding at t[1 + index] with the binding's
  // dimension, one sampler per binding at s[index], matching
  // DxbcShaderTranslator and the descriptor table order
  // WriteActiveTextureBindfulSRV writes.
  if (!bindless_resources_used_) {
    std::string bindful_declarations;
    for (uint32_t i = 0; i < uint32_t(texture_bindings_.size()); ++i) {
      const char* texture_type;
      switch (texture_bindings_[i].dimension) {
        case xenos::FetchOpDimension::k3DOrStacked:
          texture_type = "Texture3D";
          break;
        case xenos::FetchOpDimension::kCube:
          texture_type = "TextureCube";
          break;
        default:
          texture_type = "Texture2DArray";
          break;
      }
      bindful_declarations += std::string(texture_type) +
                              "<float4> xe_texture" + std::to_string(i) +
                              " : register(t" + std::to_string(i + 1) + ");\n";
    }
    for (uint32_t i = 0; i < uint32_t(sampler_bindings_.size()); ++i) {
      bindful_declarations += "SamplerState xe_sampler" + std::to_string(i) +
                              " : register(s" + std::to_string(i) + ");\n";
    }
    // The marker line keeps its own trailing newline.
    if (!bindful_declarations.empty()) {
      bindful_declarations.pop_back();
    }
    size_t marker_pos = hlsl_source_.find(kBindfulResourceDeclarationsMarker);
    if (marker_pos != std::string::npos) {
      hlsl_source_.replace(marker_pos,
                           std::strlen(kBindfulResourceDeclarationsMarker),
                           bindful_declarations);
    }
  }

  // Dump the generated HLSL into the dump_shaders directory for debugging.
  if (!cvars::dump_shaders.empty()) {
    std::filesystem::path dump_path =
        std::filesystem::absolute(cvars::dump_shaders);
    std::filesystem::create_directories(dump_path);
    std::filesystem::path hlsl_path =
        dump_path / fmt::format("shader_{:016X}_{:016X}.hlsl.{}",
                                current_shader().ucode_data_hash(),
                                current_translation().modification(),
                                is_vertex_shader() ? "vert" : "frag");
    FILE* f = filesystem::OpenFile(hlsl_path, "w");
    if (f) {
      fwrite(hlsl_source_.c_str(), 1, hlsl_source_.size(), f);
      fclose(f);
    }
  }

  // If DXC compiler is set and available, compile HLSL to DXIL.
#if XE_PLATFORM_WIN32
  if (dxc_compiler_ && dxc_compiler_->IsAvailable()) {
    std::vector<uint8_t> dxil;
    std::string error;
    std::string target = GetShaderTargetProfile();
    if (dxc_compiler_->Compile(hlsl_source_, "main", target, dxil, &error)) {
      // Dump the DXIL disassembly next to the HLSL when dumping is enabled.
      if (!cvars::dump_shaders.empty()) {
        std::string disasm;
        if (dxc_compiler_->Disassemble(dxil, disasm)) {
          std::filesystem::path disasm_path =
              std::filesystem::absolute(cvars::dump_shaders) /
              fmt::format("shader_{:016X}_{:016X}.hlsl.dxil.{}",
                          current_shader().ucode_data_hash(),
                          current_translation().modification(),
                          is_vertex_shader() ? "vert" : "frag");
          FILE* df = filesystem::OpenFile(disasm_path, "w");
          if (df) {
            fwrite(disasm.c_str(), 1, disasm.size(), df);
            fclose(df);
          }
        }
      }

      return dxil;
    }
    XELOGE("DXIL compilation failed for {} shader: {}", target, error);
    // Fall through to return HLSL for debugging.
  }
#endif  // XE_PLATFORM_WIN32

  // Return the HLSL source as bytes for storage in translation.
  std::vector<uint8_t> result(hlsl_source_.begin(), hlsl_source_.end());
  return result;
}

std::string HlslShaderTranslator::OperandToHlsl(
    const InstructionOperand& operand, uint32_t needed_components) {
  std::string result;

  // Get base register reference.
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      result = RegisterToHlsl(operand.storage_index,
                              operand.storage_addressing_mode);
      break;
    case InstructionStorageSource::kConstantFloat: {
      const Shader::ConstantRegisterMap& constant_map =
          current_shader().constant_register_map();
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kAbsolute) {
        // Use packed index for absolute addressing.
        uint32_t packed_index =
            constant_map.GetPackedFloatConstantIndex(operand.storage_index);
        if (packed_index == UINT32_MAX) {
          // Constant not found in map - shouldn't happen but handle gracefully.
          result = "float4(0.0, 0.0, 0.0, 0.0)";
        } else {
          result =
              "xe_float_constants_data[" + std::to_string(packed_index) + "]";
        }
      } else if (operand.storage_addressing_mode ==
                 InstructionStorageAddressingMode::kAddressRegisterRelative) {
        // Dynamic addressing - buffer must contain all 256 constants.
        result = "xe_float_constants_data[" +
                 std::to_string(operand.storage_index) + " + xe_a0]";
      } else {
        // Loop-relative addressing - buffer must contain all 256 constants.
        result = "xe_float_constants_data[" +
                 std::to_string(operand.storage_index) + " + xe_aL.x]";
      }
      break;
    }
    default:
      result = "float4(0.0, 0.0, 0.0, 0.0)";
      break;
  }

  // Apply swizzle.
  if (needed_components > 0 && needed_components <= 4) {
    result += "." + GetSwizzleString(operand.components, needed_components);
  }

  // Apply absolute value.
  if (operand.is_absolute_value) {
    result = "abs(" + result + ")";
  }

  // Apply negation.
  if (operand.is_negated) {
    result = "-(" + result + ")";
  }

  return result;
}

std::string HlslShaderTranslator::ResultToHlsl(
    const InstructionResult& result) {
  std::string output;

  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      output =
          RegisterToHlsl(result.storage_index, result.storage_addressing_mode);
      break;
    case InstructionStorageTarget::kInterpolator: {
      // Only write to interpolators that are in the mask.
      // If not in mask, the struct member doesn't exist.
      Modification modification = GetHlslShaderModification();
      uint32_t interpolator_mask = modification.vertex.interpolator_mask;
      uint32_t interpolator_bit = UINT32_C(1) << result.storage_index;
      if (interpolator_mask & interpolator_bit) {
        output =
            "output.xe_interpolator_" + std::to_string(result.storage_index);
      }
      // If not in mask, output stays empty and write is skipped.
      break;
    }
    case InstructionStorageTarget::kPosition:
      output = "output.xe_position";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      output = "xe_point_size_edge_flag_kill_vertex";
      break;
    case InstructionStorageTarget::kColor:
      output = "output.xe_color_" + std::to_string(result.storage_index);
      break;
    case InstructionStorageTarget::kDepth:
      output = "output.xe_depth";
      break;
    case InstructionStorageTarget::kExportAddress:
      output = "xe_eA";
      break;
    case InstructionStorageTarget::kExportData:
      output = "xe_eM[" + std::to_string(result.storage_index) + "]";
      break;
    default:
      return "";
  }

  if (output.empty()) {
    return "";
  }

  // Apply write mask.
  uint32_t write_mask = result.GetUsedWriteMask();
  if (write_mask != 0b1111) {
    output += GetWriteMaskString(write_mask);
  }

  return output;
}

std::string HlslShaderTranslator::RegisterToHlsl(
    uint32_t storage_index, InstructionStorageAddressingMode mode) const {
  switch (mode) {
    case InstructionStorageAddressingMode::kAbsolute:
      return "xe_gprs[" + std::to_string(storage_index) + "]";
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      return "xe_gprs[" + std::to_string(storage_index) + " + xe_a0]";
    case InstructionStorageAddressingMode::kLoopRelative:
      return "xe_gprs[" + std::to_string(storage_index) + " + xe_aL.x]";
  }
  return "xe_gprs[" + std::to_string(storage_index) + "]";
}

bool HlslShaderTranslator::ResultNeedsSaturation(
    const InstructionResult& result) const {
  return result.is_clamped ||
         result.storage_target == InstructionStorageTarget::kDepth;
}

std::string HlslShaderTranslator::SaturateExpressionIfNeeded(
    const InstructionResult& result, const std::string& expression) const {
  return ResultNeedsSaturation(result) ? "saturate(" + expression + ")"
                                       : expression;
}

std::string HlslShaderTranslator::GetSwizzleString(
    const SwizzleSource* components, uint32_t component_count) {
  std::string swizzle;
  for (uint32_t i = 0; i < component_count; ++i) {
    swizzle += GetCharForSwizzle(components[i]);
  }
  return swizzle;
}

std::string HlslShaderTranslator::GetWriteMaskString(uint32_t write_mask) {
  std::string mask = ".";
  if (write_mask & 0b0001) {
    mask += "x";
  }
  if (write_mask & 0b0010) {
    mask += "y";
  }
  if (write_mask & 0b0100) {
    mask += "z";
  }
  if (write_mask & 0b1000) {
    mask += "w";
  }
  return mask;
}

// Emit an assignment with proper swizzle matching for write masks.
// Uses result.components[] to determine which source component goes to each
// destination, matching DXBC's StoreResult behavior.
void HlslShaderTranslator::EmitVectorResultAssignment(
    const InstructionResult& result, const std::string& source_expr) {
  uint32_t write_mask = result.GetUsedWriteMask();
  if (write_mask == 0) {
    return;  // No components written.
  }

  // Get base destination without write mask.
  std::string dest_base;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      dest_base =
          RegisterToHlsl(result.storage_index, result.storage_addressing_mode);
      break;
    case InstructionStorageTarget::kInterpolator: {
      Modification modification = GetHlslShaderModification();
      uint32_t interpolator_mask = modification.vertex.interpolator_mask;
      uint32_t interpolator_bit = UINT32_C(1) << result.storage_index;
      if (interpolator_mask & interpolator_bit) {
        dest_base =
            "output.xe_interpolator_" + std::to_string(result.storage_index);
      }
      break;
    }
    case InstructionStorageTarget::kPosition:
      dest_base = "output.xe_position";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      dest_base = "xe_point_size_edge_flag_kill_vertex";
      break;
    case InstructionStorageTarget::kColor:
      dest_base = "output.xe_color_" + std::to_string(result.storage_index);
      break;
    case InstructionStorageTarget::kDepth:
      dest_base = "output.xe_depth";
      break;
    case InstructionStorageTarget::kExportAddress:
      dest_base = "xe_eA";
      break;
    case InstructionStorageTarget::kExportData:
      dest_base = "xe_eM[" + std::to_string(result.storage_index) + "]";
      break;
    default:
      return;
  }

  if (dest_base.empty()) {
    return;  // No output target.
  }

  const char* comp_chars = "xyzw";
  const std::string source = SaturateExpressionIfNeeded(result, source_expr);

  // Check if this is a standard swizzle (identity) - if so we can use a single
  // assignment with matching swizzles.
  bool is_standard_swizzle = true;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(write_mask & (1 << i))) {
      continue;
    }
    SwizzleSource expected = static_cast<SwizzleSource>(
        static_cast<uint32_t>(SwizzleSource::kX) + i);
    if (result.components[i] != expected) {
      is_standard_swizzle = false;
      break;
    }
  }

  if (is_standard_swizzle) {
    // Standard swizzle - use single assignment with matching write mask.
    std::string dest_swizzle = ".";
    std::string vector_expr;
    uint32_t written_component_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (write_mask & (1 << i)) {
        dest_swizzle += comp_chars[i];
        if (!vector_expr.empty()) {
          vector_expr += ", ";
        }
        vector_expr += "(" + source + ").";
        vector_expr += comp_chars[i];
        ++written_component_count;
      }
    }
    if (write_mask == 0b1111) {
      // All components - no swizzle needed.
      EmitLine(dest_base + " = " + source + ";");
    } else {
      EmitLine(dest_base + dest_swizzle + " = float" +
               std::to_string(written_component_count) + "(" + vector_expr +
               ");");
    }
  } else {
    // Non-standard swizzle - write each component separately.
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(write_mask & (1 << i))) {
        continue;
      }
      SwizzleSource src_component = result.components[i];
      if (src_component >= SwizzleSource::kX &&
          src_component <= SwizzleSource::kW) {
        uint32_t src_idx = static_cast<uint32_t>(src_component) -
                           static_cast<uint32_t>(SwizzleSource::kX);
        EmitLine(dest_base + "." + comp_chars[i] + " = (" + source + ")." +
                 comp_chars[src_idx] + ";");
      }
      // Constants (k0, k1) are handled by StoreConstantComponents.
    }
  }

  uint32_t point_size_write_mask = 0;
  if ((write_mask & 0b0001) && result.components[0] >= SwizzleSource::kX &&
      result.components[0] <= SwizzleSource::kW) {
    point_size_write_mask = 0b0001;
  }
  EmitPointSizeClampIfNeeded(result, point_size_write_mask);
  EmitMemExportWrittenMark(result);
}

// Emit a scalar result assignment. Scalar values need to be replicated
// to match the write mask component count.
void HlslShaderTranslator::EmitScalarResultAssignment(
    const InstructionResult& result, const std::string& scalar_expr) {
  std::string dest = ResultToHlsl(result);
  if (dest.empty()) {
    return;  // No output target.
  }

  uint32_t write_mask = result.GetUsedWriteMask();
  if (write_mask == 0) {
    return;  // No components written.
  }

  // Count how many components are written.
  uint32_t component_count = 0;
  if (write_mask & 0b0001) {
    component_count++;
  }
  if (write_mask & 0b0010) {
    component_count++;
  }
  if (write_mask & 0b0100) {
    component_count++;
  }
  if (write_mask & 0b1000) {
    component_count++;
  }

  const std::string source = SaturateExpressionIfNeeded(result, scalar_expr);
  if (component_count == 1) {
    // Single component - assign scalar directly.
    EmitLine(dest + " = " + source + ";");
  } else {
    // Multiple components - replicate scalar into a vector.
    std::string replicated = "float" + std::to_string(component_count) + "(";
    for (uint32_t i = 0; i < component_count; ++i) {
      if (i != 0) {
        replicated += ", ";
      }
      replicated += source;
    }
    replicated += ")";
    EmitLine(dest + " = " + replicated + ";");
  }

  EmitPointSizeClampIfNeeded(result, write_mask);
  EmitMemExportWrittenMark(result);
}

void HlslShaderTranslator::EmitPointSizeClampIfNeeded(
    const InstructionResult& result, uint32_t write_mask) {
  if (result.storage_target !=
          InstructionStorageTarget::kPointSizeEdgeFlagKillVertex ||
      !(write_mask & 0b0001)) {
    return;
  }

  EmitLine(
      "xe_point_size_edge_flag_kill_vertex.x = "
      "asfloat(min(asint(xe_point_vertex_diameter_max), "
      "max(asint(xe_point_vertex_diameter_min), "
      "asint(xe_point_size_edge_flag_kill_vertex.x))));");
}

// Store constant components (0 or 1) to the result destination.
// This handles cases where all components come from constants, not computed
// values. In such cases, the ALU operation returns early without storing
// anything, but we still need to write the constants to the destination.
void HlslShaderTranslator::StoreConstantComponents(
    const InstructionResult& result) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }

  std::string dest = ResultToHlsl(result);
  if (dest.empty()) {
    return;  // No output target.
  }

  // Find constant components (components that are k0 or k1, not from xyzw).
  uint32_t constant_mask = 0;
  uint32_t constant_1_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1 << i))) {
      continue;
    }
    SwizzleSource component = result.components[i];
    if (component == SwizzleSource::k0) {
      constant_mask |= 1 << i;
    } else if (component == SwizzleSource::k1) {
      constant_mask |= 1 << i;
      constant_1_mask |= 1 << i;
    }
    // Components >= kX and <= kW are computed values, not constants.
  }

  if (!constant_mask) {
    return;  // No constant components to store.
  }

  // Build the constant value expression.
  // For each component in the mask, write 0.0 or 1.0 based on constant_1_mask.
  uint32_t component_count = 0;
  if (constant_mask & 0b0001) {
    component_count++;
  }
  if (constant_mask & 0b0010) {
    component_count++;
  }
  if (constant_mask & 0b0100) {
    component_count++;
  }
  if (constant_mask & 0b1000) {
    component_count++;
  }

  std::string value_expr;
  if (component_count == 1) {
    // Single component - just a scalar.
    for (uint32_t i = 0; i < 4; ++i) {
      if (constant_mask & (1 << i)) {
        value_expr = (constant_1_mask & (1 << i)) ? "1.0" : "0.0";
        break;
      }
    }
  } else {
    // Multiple components - need a vector.
    value_expr = "float" + std::to_string(component_count) + "(";
    bool first = true;
    for (uint32_t i = 0; i < 4; ++i) {
      if (constant_mask & (1 << i)) {
        if (!first) {
          value_expr += ", ";
        }
        value_expr += (constant_1_mask & (1 << i)) ? "1.0" : "0.0";
        first = false;
      }
    }
    value_expr += ")";
  }

  // Build the write mask for the constant components only.
  std::string write_mask_str = ".";
  if (constant_mask & 0b0001) {
    write_mask_str += "x";
  }
  if (constant_mask & 0b0010) {
    write_mask_str += "y";
  }
  if (constant_mask & 0b0100) {
    write_mask_str += "z";
  }
  if (constant_mask & 0b1000) {
    write_mask_str += "w";
  }

  // Get the base destination without any existing write mask.
  // ResultToHlsl might already include a write mask, so we need to strip it.
  std::string dest_base = ResultToHlsl(result);
  // Find and remove any existing write mask (everything after the last '.')
  // that looks like a component mask (.x, .xy, .xyz, .xyzw, etc.)
  size_t dot_pos = dest_base.rfind('.');
  if (dot_pos != std::string::npos) {
    std::string potential_mask = dest_base.substr(dot_pos + 1);
    bool is_mask = !potential_mask.empty();
    for (char c : potential_mask) {
      if (c != 'x' && c != 'y' && c != 'z' && c != 'w') {
        is_mask = false;
        break;
      }
    }
    if (is_mask) {
      dest_base = dest_base.substr(0, dot_pos);
    }
  }

  EmitLine(dest_base + write_mask_str + " = " + value_expr + ";");
  EmitPointSizeClampIfNeeded(result, constant_mask);
  EmitMemExportWrittenMark(result);
}

void HlslShaderTranslator::ProcessLabel(uint32_t cf_index) {
  if (cf_index == 0) {
    // Already in case 0 from StartTranslation.
    return;
  }
  // Close any open exec conditionals before switching labels.
  CloseExecConditionals();
  if (has_main_switch_) {
    // Fallthrough to the next label - set pc and continue.
    EmitLine("xe_pc = " + std::to_string(cf_index) + ";");
    EmitLine("continue;");
    Outdent();
    EmitLine("case " + std::to_string(cf_index) + ":");
    Indent();
  }
}

void HlslShaderTranslator::ProcessExecInstructionBegin(
    const ParsedExecInstruction& instr) {
  // Handle conditional execution.
  switch (instr.type) {
    case ParsedExecInstruction::Type::kConditional:
      cf_exec_bool_constant_ = instr.bool_constant_index;
      cf_exec_bool_constant_condition_ = instr.condition;
      EmitLine("if (XeGetBoolConstant(" +
               std::to_string(instr.bool_constant_index) + ") " +
               (instr.condition ? "==" : "!=") + " true) {");
      Indent();
      break;
    case ParsedExecInstruction::Type::kPredicated:
      cf_exec_predicated_ = true;
      cf_exec_predicate_condition_ = instr.condition;
      EmitLine("if (xe_p0 " + std::string(instr.condition ? "==" : "!=") +
               " true) {");
      Indent();
      break;
    default:
      break;
  }
}

void HlslShaderTranslator::ProcessExecInstructionEnd(
    const ParsedExecInstruction& instr) {
  // Handle shader termination.
  if (instr.is_end) {
    CloseInstructionPredication();
    if (has_main_switch_) {
      // Set pc to invalid value and continue - will hit default case and break.
      EmitLine("xe_pc = 0x7FFFFFFF;");
      EmitLine("continue;");
    }
    // For shaders without labels, is_end just means the last exec block.
    // No special handling needed - execution naturally falls through to
    // the output writes and return.
  }
  // Close conditional blocks.
  if (cf_exec_predicated_ || cf_exec_bool_constant_ != UINT32_MAX) {
    Outdent();
    EmitLine("}");
  }
  cf_exec_predicated_ = false;
  cf_exec_bool_constant_ = UINT32_MAX;
}

void HlslShaderTranslator::ProcessLoopStartInstruction(
    const ParsedLoopStartInstruction& instr) {
  // Loop control is outside execs - close any open exec conditionals.
  CloseExecConditionals();

  // Loops require the state machine (while/switch) to be active.
  // This should always be true for shaders with loop instructions.
  if (!has_main_switch_) {
    EmitLine("// ERROR: Loop instruction without state machine");
    return;
  }

  EmitLine("// Loop start - constant " +
           std::to_string(instr.loop_constant_index));
  EmitLine("{");
  Indent();
  EmitLine("uint xe_loop_const = XeGetLoopConstant(" +
           std::to_string(instr.loop_constant_index) + ");");
  EmitLine("uint xe_loop_count_val = xe_loop_const & 0xFFu;");

  // Skip the loop if count is zero.
  EmitLine("if (xe_loop_count_val == 0u) {");
  Indent();
  EmitLine("xe_pc = " + std::to_string(instr.loop_skip_address) + ";");
  EmitLine("continue;");
  Outdent();
  EmitLine("}");

  // Push loop count to stack - move xyz to yzw and set x to new count.
  EmitLine("xe_loop_count = uint4(xe_loop_count_val, xe_loop_count.xyz);");

  // Push aL - keep the same value if repeating, or initialize from constant.
  if (instr.is_repeat) {
    EmitLine("xe_aL = int4(xe_aL.x, xe_aL.xyz);");
  } else {
    EmitLine("xe_aL = int4(int((xe_loop_const >> 8u) & 0xFFu), xe_aL.xyz);");
  }
  Outdent();
  EmitLine("}");
}

void HlslShaderTranslator::ProcessLoopEndInstruction(
    const ParsedLoopEndInstruction& instr) {
  // Loop control is outside execs - close any open exec conditionals.
  CloseExecConditionals();

  // Loops require the state machine (while/switch) to be active.
  if (!has_main_switch_) {
    EmitLine("// ERROR: Loop instruction without state machine");
    return;
  }

  EmitLine("// Loop end - constant " +
           std::to_string(instr.loop_constant_index));

  // Decrement the loop counter.
  EmitLine("xe_loop_count.x = xe_loop_count.x - 1u;");

  // Determine if we should break - either count reached 0, or predicated break.
  if (instr.is_predicated_break) {
    // Break if count == 0 || predicate matches condition.
    EmitLine("if (xe_loop_count.x == 0u || xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
  } else {
    // Break if count == 0.
    EmitLine("if (xe_loop_count.x == 0u) {");
  }
  Indent();

  // Pop the loop count stack - move yzw to xyz, set w to 0.
  EmitLine("xe_loop_count = uint4(xe_loop_count.yzw, 0u);");
  // Pop the aL stack - move yzw to xyz, set w to 0.
  EmitLine("xe_aL = int4(xe_aL.yzw, 0);");
  // Fall through to next instruction (no jump needed).

  Outdent();
  EmitLine("} else {");
  Indent();

  // Continue the loop - update aL and jump back to loop body.
  EmitLine("{");
  Indent();
  EmitLine("uint xe_loop_const = XeGetLoopConstant(" +
           std::to_string(instr.loop_constant_index) + ");");
  EmitLine("int xe_loop_step = int((xe_loop_const >> 16u) & 0xFFu);");
  EmitLine("if (xe_loop_step > 127) xe_loop_step -= 256;");
  EmitLine("xe_aL.x = xe_aL.x + xe_loop_step;");
  Outdent();
  EmitLine("}");
  EmitLine("xe_pc = " + std::to_string(instr.loop_body_address) + ";");
  EmitLine("continue;");

  Outdent();
  EmitLine("}");
}

void HlslShaderTranslator::ProcessCallInstruction(
    const ParsedCallInstruction& instr) {
  CloseInstructionPredication();
  CloseExecConditionals();

  // Push the return point (the instruction after the call, registered as a
  // label) and jump to the subroutine. ret pops and jumps back.
  std::string return_address = std::to_string(instr.dword_index + 1);
  std::string target = std::to_string(instr.target_address);
  auto emit_call = [&]() {
    EmitLine("xe_call_stack[xe_call_sp++] = " + return_address + "u;");
    EmitLine("xe_pc = " + target + ";");
    EmitLine("continue;");
  };

  switch (instr.type) {
    case ParsedCallInstruction::Type::kUnconditional:
      emit_call();
      break;
    case ParsedCallInstruction::Type::kConditional:
      EmitLine("if (XeGetBoolConstant(" +
               std::to_string(instr.bool_constant_index) + ") " +
               (instr.condition ? "==" : "!=") + " true) {");
      Indent();
      emit_call();
      Outdent();
      EmitLine("}");
      break;
    case ParsedCallInstruction::Type::kPredicated:
      EmitLine("if (xe_p0 " + std::string(instr.condition ? "==" : "!=") +
               " true) {");
      Indent();
      emit_call();
      Outdent();
      EmitLine("}");
      break;
  }
}

void HlslShaderTranslator::ProcessReturnInstruction(
    const ParsedReturnInstruction& instr) {
  CloseInstructionPredication();
  CloseExecConditionals();
  // Pop the return address and jump back to the instruction after the call.
  EmitLine("xe_pc = int(xe_call_stack[--xe_call_sp]);");
  EmitLine("continue;");
}

void HlslShaderTranslator::ProcessJumpInstruction(
    const ParsedJumpInstruction& instr) {
  // Close instruction-level predication before flow control.
  CloseInstructionPredication();

  // Jumps require the state machine (while/switch) to be active.
  if (!has_main_switch_) {
    EmitLine("// ERROR: Jump instruction without state machine");
    return;
  }

  std::string target = std::to_string(instr.target_address);

  switch (instr.type) {
    case ParsedJumpInstruction::Type::kUnconditional:
      EmitLine("xe_pc = " + target + ";");
      EmitLine("continue;");
      break;
    case ParsedJumpInstruction::Type::kConditional:
      EmitLine("if (XeGetBoolConstant(" +
               std::to_string(instr.bool_constant_index) + ") " +
               (instr.condition ? "==" : "!=") + " true) {");
      Indent();
      EmitLine("xe_pc = " + target + ";");
      EmitLine("continue;");
      Outdent();
      EmitLine("}");
      break;
    case ParsedJumpInstruction::Type::kPredicated:
      EmitLine("if (xe_p0 " + std::string(instr.condition ? "==" : "!=") +
               " true) {");
      Indent();
      EmitLine("xe_pc = " + target + ";");
      EmitLine("continue;");
      Outdent();
      EmitLine("}");
      break;
  }
}

void HlslShaderTranslator::ProcessAllocInstruction(
    const ParsedAllocInstruction& instr, uint8_t export_eM) {
  const bool starts_memexport = instr.type == ucode::AllocType::kMemory &&
                                current_shader().memexport_eM_written() != 0;
  if (export_eM) {
    // Flush the elements written before this alloc, then reset for the next
    // batch (matching DxbcShaderTranslator::ProcessAllocInstruction).
    EmitMemExportFlush();
    EmitLine("xe_eM_written = 0u;");
    for (uint32_t i = 0; i < ucode::kMaxMemExportElementCount; ++i) {
      if (export_eM & (uint8_t(1) << i)) {
        EmitLine("xe_eM[" + std::to_string(i) +
                 "] = float4(0.0, 0.0, 0.0, 0.0);");
      }
    }
  }
  if (starts_memexport) {
    // Reset eA to an invalid address.
    EmitLine("xe_eA = float4(0.0, 0.0, 0.0, 0.0);");
  }
}

std::string HlslShaderTranslator::OperandToHlslNoSwizzle(
    const InstructionOperand& operand) {
  std::string result;

  // Get base register reference.
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      result = RegisterToHlsl(operand.storage_index,
                              operand.storage_addressing_mode);
      break;
    case InstructionStorageSource::kConstantFloat: {
      const Shader::ConstantRegisterMap& constant_map =
          current_shader().constant_register_map();
      if (operand.storage_addressing_mode ==
          InstructionStorageAddressingMode::kAbsolute) {
        // Use packed index for absolute addressing.
        uint32_t packed_index =
            constant_map.GetPackedFloatConstantIndex(operand.storage_index);
        if (packed_index == UINT32_MAX) {
          // Constant not found in map - shouldn't happen but handle gracefully.
          result = "float4(0.0, 0.0, 0.0, 0.0)";
        } else {
          result =
              "xe_float_constants_data[" + std::to_string(packed_index) + "]";
        }
      } else if (operand.storage_addressing_mode ==
                 InstructionStorageAddressingMode::kAddressRegisterRelative) {
        // Dynamic addressing - buffer must contain all 256 constants.
        result = "xe_float_constants_data[" +
                 std::to_string(operand.storage_index) + " + xe_a0]";
      } else {
        // Loop-relative addressing - buffer must contain all 256 constants.
        result = "xe_float_constants_data[" +
                 std::to_string(operand.storage_index) + " + xe_aL.x]";
      }
      break;
    }
    default:
      result = "float4(0.0, 0.0, 0.0, 0.0)";
      break;
  }

  return result;
}

void HlslShaderTranslator::CloseInstructionPredication() {
  if (cf_instruction_predicate_if_open_) {
    Outdent();
    EmitLine("}");
    cf_instruction_predicate_if_open_ = false;
  }
}

void HlslShaderTranslator::CloseExecConditionals() {
  CloseInstructionPredication();
  if (cf_exec_predicated_ || cf_exec_bool_constant_ != UINT32_MAX) {
    Outdent();
    EmitLine("}");
    cf_exec_predicated_ = false;
    cf_exec_bool_constant_ = UINT32_MAX;
  }
}

}  // namespace gpu
}  // namespace xe
