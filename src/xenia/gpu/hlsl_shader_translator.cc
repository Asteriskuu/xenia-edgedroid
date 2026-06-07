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

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/ucode.h"
#include "xenia/gpu/xenos.h"
#if XE_PLATFORM_WIN32
// DXC (HLSL->DXIL) is part of the Windows-only D3D12 backend.
#include "xenia/gpu/d3d12/dxc_compiler.h"
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace gpu {

namespace {

std::string HlslFloatLiteral(float value) {
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

bool IsVectorKillOpcode(ucode::AluVectorOpcode opcode) {
  switch (opcode) {
    case ucode::AluVectorOpcode::kKillEq:
    case ucode::AluVectorOpcode::kKillGt:
    case ucode::AluVectorOpcode::kKillGe:
    case ucode::AluVectorOpcode::kKillNe:
      return true;
    default:
      return false;
  }
}

bool IsScalarKillOpcode(ucode::AluScalarOpcode opcode) {
  switch (opcode) {
    case ucode::AluScalarOpcode::kKillsEq:
    case ucode::AluScalarOpcode::kKillsGt:
    case ucode::AluScalarOpcode::kKillsGe:
    case ucode::AluScalarOpcode::kKillsNe:
    case ucode::AluScalarOpcode::kKillsOne:
      return true;
    default:
      return false;
  }
}

}  // namespace

HlslShaderTranslator::HlslShaderTranslator(
    ui::GraphicsProvider::GpuVendorID vendor_id, bool bindless_resources_used,
    bool edram_rov_used, bool use_shader_model_6_6,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y)
    : vendor_id_(vendor_id),
      bindless_resources_used_(bindless_resources_used),
      edram_rov_used_(edram_rov_used),
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
    // Bindful mode: static texture and sampler declarations.
    EmitLine("// 2D textures (register t1-t32)");
    for (uint32_t i = 0; i < 32; ++i) {
      EmitLine("Texture2D<float4> xe_texture2d_" + std::to_string(i) +
               " : register(t" + std::to_string(i + 1) + ");");
    }
    EmitLine("");

    EmitLine("// 3D textures (register t33-t64)");
    for (uint32_t i = 0; i < 32; ++i) {
      EmitLine("Texture3D<float4> xe_texture3d_" + std::to_string(i) +
               " : register(t" + std::to_string(i + 33) + ");");
    }
    EmitLine("");

    EmitLine("// Cube textures (register t65-t96)");
    for (uint32_t i = 0; i < 32; ++i) {
      EmitLine("TextureCube<float4> xe_texturecube_" + std::to_string(i) +
               " : register(t" + std::to_string(i + 65) + ");");
    }
    EmitLine("");

    // Sampler declarations.
    EmitLine("// Samplers (register s0-s31)");
    for (uint32_t i = 0; i < 32; ++i) {
      EmitLine("SamplerState xe_sampler_" + std::to_string(i) +
               " : register(s" + std::to_string(i) + ");");
    }
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
    EmitLine("struct PSInput {");
    Indent();
    // Interpolators are packed contiguously by TEXCOORD index.
    uint32_t texcoord_index = 0;
    for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
      if (interpolator_mask & (1u << i)) {
        EmitLine(interp_prefix + std::string("float4 xe_interpolator_") +
                 std::to_string(i) + " : TEXCOORD" +
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
    EmitLine("float4 xe_position : SV_Position;");
    EmitLine("bool xe_is_front_face : SV_IsFrontFace;");
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
      // exponent bias, matching the DxbcShaderTranslator output merger.
      // RGB only, gated on the per target flag (set for k_8_8_8_8_GAMMA).
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(color_targets_written & (1u << i))) {
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
    bool needs_float24_depth = PixelShaderNeedsFloat24DepthOutput();
    if (needs_float24_depth) {
      if (current_shader().writes_depth()) {
        EmitLine("float xe_depth_guest = output.xe_depth;");
      } else {
        EmitLine(
            "float xe_depth_guest = XeSaturateNoNaN(input.xe_position.z * "
            "2.0f);");
      }
      if (modification.pixel.depth_stencil_mode ==
          Modification::DepthStencilMode::kFloat24Truncating) {
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

  // Dump HLSL source for debugging.
  {
    uint64_t hash = current_shader().ucode_data_hash();
    std::string filename =
        "shaders/shader_" + fmt::format("{:016X}", hash) + "_" +
        fmt::format("{:016X}", current_translation().modification()) +
        (is_vertex_shader() ? ".hlsl.vert" : ".hlsl.frag");
    FILE* f = fopen(filename.c_str(), "w");
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
      XELOGI("Shader compiled to DXIL ({} bytes, target {})", dxil.size(),
             target);

      // Dump DXIL disassembly for debugging
      std::string disasm;
      if (dxc_compiler_->Disassemble(dxil, disasm)) {
        uint64_t hash = current_shader().ucode_data_hash();
        std::string dxil_filename =
            "shaders/shader_" + fmt::format("{:016X}", hash) + "_" +
            fmt::format("{:016X}", current_translation().modification()) +
            (is_vertex_shader() ? ".hlsl.dxil.vert" : ".hlsl.dxil.frag");
        FILE* df = fopen(dxil_filename.c_str(), "w");
        if (df) {
          fwrite(disasm.c_str(), 1, disasm.size(), df);
          fclose(df);
          XELOGI("DXIL disassembly written to {}", dxil_filename);
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

bool HlslShaderTranslator::PixelShaderNeedsFloat24DepthOutput() const {
  if (!is_pixel_shader()) {
    return false;
  }
  Modification::DepthStencilMode mode =
      GetHlslShaderModification().pixel.depth_stencil_mode;
  return mode == Modification::DepthStencilMode::kFloat24Truncating ||
         mode == Modification::DepthStencilMode::kFloat24Rounding;
}

bool HlslShaderTranslator::PixelShaderWritesDepthOutput() const {
  return is_pixel_shader() && (current_shader().writes_depth() ||
                               PixelShaderNeedsFloat24DepthOutput());
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

  EmitLine("// Alpha test");
  EmitLine("{");
  Indent();
  EmitLine("uint xe_alpha_test_function = (xe_flags >> 7u) & 7u;");
  EmitLine("if (xe_alpha_test_function != 7u) {");
  Indent();
  EmitLine("float xe_alpha_test_alpha = output.xe_color_0.a;");
  EmitLine("bool xe_alpha_test_pass = false;");
  EmitLine("if (xe_alpha_test_function == 5u) {");
  Indent();
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_alpha != xe_alpha_test_reference;");
  Outdent();
  EmitLine("} else {");
  Indent();
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_pass || "
      "(((xe_alpha_test_function & 1u) != 0u) && "
      "(xe_alpha_test_alpha < xe_alpha_test_reference));");
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_pass || "
      "(((xe_alpha_test_function & 2u) != 0u) && "
      "(xe_alpha_test_alpha == xe_alpha_test_reference));");
  EmitLine(
      "xe_alpha_test_pass = xe_alpha_test_pass || "
      "(((xe_alpha_test_function & 4u) != 0u) && "
      "(xe_alpha_test_reference < xe_alpha_test_alpha));");
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
  EmitLine(
      "xe_atoc_coverage |= (xe_atoc_alpha >= "
      "(0.5f - xe_atoc_offset * (1.0f / 8.0f))) ? 1u : 0u;");
  EmitLine(
      "xe_atoc_coverage |= (xe_atoc_alpha >= "
      "(1.0f - xe_atoc_offset * (1.0f / 8.0f))) ? 2u : 0u;");
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

void HlslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  if (!used_result_components && instr.is_mini_fetch) {
    // Nothing to load.
    EmitVectorResultAssignment(instr.result, "float4(0.0, 0.0, 0.0, 0.0)");
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

  EmitLine("{");
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
    return;
  }

  // Get texture and sampler indices from fetch constant.
  uint32_t fetch_constant_index = instr.operands[1].storage_index;

  // Get coordinates from source operand.
  std::string coords = "(" + OperandToHlsl(instr.operands[0], 4) + ")";

  EmitLine("{");
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
      (!is_pixel_shader() || !instr.attributes.use_computed_lod ||
       instr.attributes.use_register_lod ||
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
                 offset_x_literal + " / xe_tf_width), 0.0);");
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
        EmitLine("float2 xe_tf_uv = " + coords + ".xy + float2(" +
                 offset_x_literal + ", " + offset_y_literal +
                 ") / xe_tf_size;");
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
        EmitLine("xe_tf_uvw.xy = " + coords + ".xy + float2(" +
                 offset_x_literal + ", " + offset_y_literal +
                 ") / xe_tf_size.xy;");
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
                 ".xy + float2(" + offset_x_literal + ", " + offset_y_literal +
                 ") / xe_tf_size, " + coords + ".z + " + offset_z_literal +
                 ");");
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
    const bool use_sample_grad =
        instr.attributes.use_register_gradients && is_pixel_shader() &&
        instr.dimension != xenos::FetchOpDimension::k1D;
    const bool use_sample_level =
        !use_sample_grad &&
        (instr.attributes.use_register_lod || is_vertex_shader() ||
         !instr.attributes.use_computed_lod);
    const xenos::TextureFilter sampler_mip_filter =
        instr.opcode == FetchOpcode::kGetTextureComputedLod
            ? xenos::TextureFilter::kLinear
            : instr.attributes.mip_filter;
    const xenos::AnisoFilter sampler_aniso_filter =
        use_sample_level ? xenos::AnisoFilter::kDisabled
                         : instr.attributes.aniso_filter;

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

    auto emit_bindless_texture_index = [&](const std::string& variable_name,
                                           uint32_t binding_index) {
      uint32_t descriptor_index =
          texture_bindings_[binding_index].bindless_descriptor_index;
      EmitLine("uint " + variable_name + " = XeGetDescriptorIndex(" +
               std::to_string(descriptor_index) +
               "u) + kXeResourceDescriptorHeapStart;");
    };
    auto emit_sample =
        [&](const std::string& result_name, const std::string& texture_name,
            const std::string& sampler_name, const std::string& sample_coord,
            const std::string& lod_coord, const std::string& grad_h,
            const std::string& grad_v) {
          if (instr.opcode == FetchOpcode::kGetTextureComputedLod) {
            if (is_pixel_shader() && !instr.attributes.use_register_lod &&
                !instr.attributes.use_register_gradients) {
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

    if (bindless_resources_used_) {
      uint32_t sampler_descriptor_index =
          sampler_bindings_[sampler_binding_index].bindless_descriptor_index;
      EmitLine("uint xe_tf_smp_idx = XeGetDescriptorIndex(" +
               std::to_string(sampler_descriptor_index) + "u);");
      EmitLine(
          "SamplerState xe_tf_smp = "
          "SamplerDescriptorHeap[NonUniformResourceIndex(xe_tf_smp_idx)];");

      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
        case xenos::FetchOpDimension::k2D:
          emit_bindless_texture_index("xe_tf_tex_idx_unsigned",
                                      texture_binding_unsigned);
          if (texture_fetch) {
            emit_bindless_texture_index("xe_tf_tex_idx_signed",
                                        texture_binding_signed);
          }
          EmitLine(
              "Texture2DArray<float4> xe_tf_tex_unsigned = "
              "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_idx_"
              "unsigned)];");
          if (texture_fetch) {
            EmitLine(
                "Texture2DArray<float4> xe_tf_tex_signed = "
                "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_idx_"
                "signed)];");
          }
          EmitLine("float3 xe_tf_uvl = float3(xe_tf_uv, 0.0);");
          emit_texture_fetch_sample(
              "xe_tf_needs_unsigned", "xe_tf_result_unsigned",
              "xe_tf_tex_unsigned", "xe_tf_smp", "xe_tf_uvl", "xe_tf_uv",
              "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          if (texture_fetch) {
            emit_texture_fetch_sample("xe_tf_needs_signed",
                                      "xe_tf_result_signed", "xe_tf_tex_signed",
                                      "xe_tf_smp", "xe_tf_uvl", "xe_tf_uv",
                                      "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          }
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          emit_bindless_texture_index("xe_tf_tex_3d_idx_unsigned",
                                      texture_binding_3d_unsigned);
          emit_bindless_texture_index("xe_tf_tex_2d_idx_unsigned",
                                      texture_binding_2d_unsigned);
          if (texture_fetch) {
            emit_bindless_texture_index("xe_tf_tex_3d_idx_signed",
                                        texture_binding_3d_signed);
            emit_bindless_texture_index("xe_tf_tex_2d_idx_signed",
                                        texture_binding_2d_signed);
          }
          EmitLine("if (XeTextureFetchConstantIs3D(" + fetch_constant_literal +
                   ")) {");
          Indent();
          EmitLine(
              "Texture3D<float4> xe_tf_tex_3d_unsigned = "
              "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_3d_idx_"
              "unsigned)];");
          if (texture_fetch) {
            EmitLine(
                "Texture3D<float4> xe_tf_tex_3d_signed = "
                "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_3d_"
                "idx_signed)];");
          }
          emit_texture_fetch_sample(
              "xe_tf_needs_unsigned", "xe_tf_result_unsigned",
              "xe_tf_tex_3d_unsigned", "xe_tf_smp", "xe_tf_uvw", "xe_tf_uvw",
              "xe_tf_grad_h", "xe_tf_grad_v");
          if (texture_fetch) {
            emit_texture_fetch_sample(
                "xe_tf_needs_signed", "xe_tf_result_signed",
                "xe_tf_tex_3d_signed", "xe_tf_smp", "xe_tf_uvw", "xe_tf_uvw",
                "xe_tf_grad_h", "xe_tf_grad_v");
          }
          Outdent();
          EmitLine("} else {");
          Indent();
          EmitLine(
              "Texture2DArray<float4> xe_tf_tex_2d_unsigned = "
              "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_2d_idx_"
              "unsigned)];");
          if (texture_fetch) {
            EmitLine(
                "Texture2DArray<float4> xe_tf_tex_2d_signed = "
                "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_2d_"
                "idx_signed)];");
          }
          emit_texture_fetch_sample(
              "xe_tf_needs_unsigned", "xe_tf_result_unsigned",
              "xe_tf_tex_2d_unsigned", "xe_tf_smp", "xe_tf_uvw", "xe_tf_uvw.xy",
              "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          if (texture_fetch) {
            emit_texture_fetch_sample(
                "xe_tf_needs_signed", "xe_tf_result_signed",
                "xe_tf_tex_2d_signed", "xe_tf_smp", "xe_tf_uvw", "xe_tf_uvw.xy",
                "xe_tf_grad_h.xy", "xe_tf_grad_v.xy");
          }
          Outdent();
          EmitLine("}");
          break;
        case xenos::FetchOpDimension::kCube:
          emit_bindless_texture_index("xe_tf_tex_idx_unsigned",
                                      texture_binding_unsigned);
          if (texture_fetch) {
            emit_bindless_texture_index("xe_tf_tex_idx_signed",
                                        texture_binding_signed);
          }
          EmitLine(
              "TextureCube<float4> xe_tf_tex_unsigned = "
              "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_idx_"
              "unsigned)];");
          if (texture_fetch) {
            EmitLine(
                "TextureCube<float4> xe_tf_tex_signed = "
                "ResourceDescriptorHeap[NonUniformResourceIndex(xe_tf_tex_idx_"
                "signed)];");
          }
          emit_texture_fetch_sample(
              "xe_tf_needs_unsigned", "xe_tf_result_unsigned",
              "xe_tf_tex_unsigned", "xe_tf_smp", "xe_tf_dir", "xe_tf_dir",
              "xe_tf_grad_h", "xe_tf_grad_v");
          if (texture_fetch) {
            emit_texture_fetch_sample("xe_tf_needs_signed",
                                      "xe_tf_result_signed", "xe_tf_tex_signed",
                                      "xe_tf_smp", "xe_tf_dir", "xe_tf_dir",
                                      "xe_tf_grad_h", "xe_tf_grad_v");
          }
          break;
        default:
          break;
      }
    } else {
      std::string sampler_name =
          "xe_sampler_" + std::to_string(fetch_constant_index);
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
        case xenos::FetchOpDimension::k2D: {
          std::string texture_name =
              "xe_texture2d_" + std::to_string(fetch_constant_index);
          emit_sample("xe_tf_result_unsigned", texture_name, sampler_name,
                      "xe_tf_uv", "xe_tf_uv", "xe_tf_grad_h.xy",
                      "xe_tf_grad_v.xy");
        } break;
        case xenos::FetchOpDimension::k3DOrStacked: {
          EmitLine("if (XeTextureFetchConstantIs3D(" + fetch_constant_literal +
                   ")) {");
          Indent();
          std::string texture_name =
              "xe_texture3d_" + std::to_string(fetch_constant_index);
          emit_sample("xe_tf_result_unsigned", texture_name, sampler_name,
                      "xe_tf_uvw", "xe_tf_uvw", "xe_tf_grad_h", "xe_tf_grad_v");
          Outdent();
          EmitLine("} else {");
          Indent();
          texture_name = "xe_texture2d_" + std::to_string(fetch_constant_index);
          emit_sample("xe_tf_result_unsigned", texture_name, sampler_name,
                      "xe_tf_uvw.xy", "xe_tf_uvw.xy", "xe_tf_grad_h.xy",
                      "xe_tf_grad_v.xy");
          Outdent();
          EmitLine("}");
        } break;
        case xenos::FetchOpDimension::kCube: {
          std::string texture_name =
              "xe_texturecube_" + std::to_string(fetch_constant_index);
          emit_sample("xe_tf_result_unsigned", texture_name, sampler_name,
                      "xe_tf_dir", "xe_tf_dir", "xe_tf_grad_h", "xe_tf_grad_v");
        } break;
        default:
          break;
      }
      EmitLine("xe_tf_result_signed = xe_tf_result_unsigned;");
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

void HlslShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr,
    uint8_t memexport_eM_potentially_written_before) {
  if (memexport_eM_potentially_written_before &&
      (IsVectorKillOpcode(instr.vector_opcode) ||
       IsScalarKillOpcode(instr.scalar_opcode))) {
    // A pixel killed here never reaches the end-of-shader flush, so its pending
    // exports are dropped. Flushing unconditionally would double-export for
    // surviving pixels, so the per-killed-lane flush is left for later.
    EmitLine("// memexport-before-kill flush not emitted");
  }

  // Handle instruction predication.
  bool needs_predicate_close = false;
  if (instr.is_predicated) {
    EmitLine("if (xe_p0 " +
             std::string(instr.predicate_condition ? "==" : "!=") + " true) {");
    Indent();
    needs_predicate_close = true;
  }

  // Process vector operation.
  // Note: ProcessVectorAluInstruction calls StoreConstantComponents internally.
  ProcessVectorAluInstruction(instr);

  // Process scalar operation.
  ProcessScalarAluInstruction(instr);

  // Store any constant-only components for scalar results.
  // Vector constant components are handled inside ProcessVectorAluInstruction.
  StoreConstantComponents(instr.scalar_result);

  if (needs_predicate_close) {
    Outdent();
    EmitLine("}");
  }
}

void HlslShaderTranslator::ProcessVectorAluInstruction(
    const ParsedAluInstruction& instr) {
  uint32_t used_result_components =
      instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used_result_components &&
      !ucode::GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    return;
  }

  // Get operands.
  std::string op0, op1, op2;
  if (instr.vector_operand_count > 0) {
    op0 = OperandToHlsl(instr.vector_operands[0], 4);
  }
  if (instr.vector_operand_count > 1) {
    op1 = OperandToHlsl(instr.vector_operands[1], 4);
  }
  if (instr.vector_operand_count > 2) {
    op2 = OperandToHlsl(instr.vector_operands[2], 4);
  }

  std::string result;
  std::string result_swizzle;  // For scalar results like dp4 that replicate

  using AluVectorOpcode = ucode::AluVectorOpcode;
  switch (instr.vector_opcode) {
    case AluVectorOpcode::kAdd:
      result = "(" + op0 + " + " + op1 + ")";
      break;

    case AluVectorOpcode::kMul:
      // SM3: 0 * anything = 0
      result = "XeMulSM3(" + op0 + ", " + op1 + ")";
      break;

    case AluVectorOpcode::kMad:
      // SM3: 0 * anything = 0, then add
      result = "(XeMulSM3(" + op0 + ", " + op1 + ") + " + op2 + ")";
      break;

    case AluVectorOpcode::kMax:
      // SM3 NaN behavior: a >= b ? a : b (not fmax)
      // Optimization: if both operands are identical, just use the operand
      // directly. This avoids a DXC optimizer bug where select(cond, a, a)
      // with fast-math enabled incorrectly replaces 0.0 values with 1.0.
      if (op0 == op1) {
        result = op0;
      } else {
        result =
            "select((" + op0 + " >= " + op1 + "), " + op0 + ", " + op1 + ")";
      }
      break;

    case AluVectorOpcode::kMin:
      // SM3 NaN behavior: a < b ? a : b (not fmin)
      // Optimization: if both operands are identical, just use the operand.
      if (op0 == op1) {
        result = op0;
      } else {
        result =
            "select((" + op0 + " < " + op1 + "), " + op0 + ", " + op1 + ")";
      }
      break;

    case AluVectorOpcode::kSeq:
      result = "select((" + op0 + " == " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kSgt:
      result = "select((" + op0 + " > " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kSge:
      result = "select((" + op0 + " >= " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kSne:
      result = "select((" + op0 + " != " + op1 +
               "), float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0))";
      break;

    case AluVectorOpcode::kFrc:
      result = "frac(" + op0 + ")";
      break;

    case AluVectorOpcode::kTrunc:
      result = "trunc(" + op0 + ")";
      break;

    case AluVectorOpcode::kFloor:
      result = "floor(" + op0 + ")";
      break;

    case AluVectorOpcode::kCndEq:
      result = "select((" + op0 + " == float4(0.0, 0.0, 0.0, 0.0)), " + op1 +
               ", " + op2 + ")";
      break;

    case AluVectorOpcode::kCndGe:
      result = "select((" + op0 + " >= float4(0.0, 0.0, 0.0, 0.0)), " + op1 +
               ", " + op2 + ")";
      break;

    case AluVectorOpcode::kCndGt:
      result = "select((" + op0 + " > float4(0.0, 0.0, 0.0, 0.0)), " + op1 +
               ", " + op2 + ")";
      break;

    case AluVectorOpcode::kDp4:
      // Result is scalar replicated to all components - inline the expression.
      result = "(dot(" + op0 + ", " + op1 + ")).xxxx";
      break;

    case AluVectorOpcode::kDp3: {
      std::string op0_xyz = OperandToHlsl(instr.vector_operands[0], 3);
      std::string op1_xyz = OperandToHlsl(instr.vector_operands[1], 3);
      result = "(dot(" + op0_xyz + ", " + op1_xyz + ")).xxxx";
    } break;

    case AluVectorOpcode::kDp2Add: {
      std::string op0_xy = OperandToHlsl(instr.vector_operands[0], 2);
      std::string op1_xy = OperandToHlsl(instr.vector_operands[1], 2);
      // src2 swizzle component 0
      std::string op2_x = OperandToHlsl(instr.vector_operands[2], 1);
      result = "(dot(" + op0_xy + ", " + op1_xy + ") + " + op2_x + ").xxxx";
    } break;

    case AluVectorOpcode::kCube: {
      // Cube map coordinate calculation.
      // Input is in z_xy order (.zzxy swizzle applied to operand).
      // Result is (T coord, S coord, 2*major axis, face ID).
      EmitLine("{");
      Indent();
      EmitLine("float3 xe_cube_src = " +
               OperandToHlsl(instr.vector_operands[0], 3) + ";");
      EmitLine("float xe_cube_x = " +
               OperandToHlsl(instr.vector_operands[0], 4) + ".z;");
      EmitLine("float xe_cube_y = " +
               OperandToHlsl(instr.vector_operands[0], 4) + ".w;");
      EmitLine("float xe_cube_z = " +
               OperandToHlsl(instr.vector_operands[0], 4) + ".x;");
      EmitLine("float xe_cube_abs_x = abs(xe_cube_x);");
      EmitLine("float xe_cube_abs_y = abs(xe_cube_y);");
      EmitLine("float xe_cube_abs_z = abs(xe_cube_z);");
      EmitLine("float4 xe_cube_result;");
      EmitLine(
          "if (xe_cube_abs_z >= xe_cube_abs_x && xe_cube_abs_z >= "
          "xe_cube_abs_y) {");
      Indent();
      EmitLine("// Z is major axis");
      EmitLine("xe_cube_result.x = -xe_cube_y;");
      EmitLine(
          "xe_cube_result.y = (xe_cube_z < 0.0) ? -xe_cube_x : xe_cube_x;");
      EmitLine("xe_cube_result.z = 2.0 * xe_cube_z;");
      EmitLine("xe_cube_result.w = (xe_cube_z < 0.0) ? 5.0 : 4.0;");
      Outdent();
      EmitLine("} else if (xe_cube_abs_y >= xe_cube_abs_x) {");
      Indent();
      EmitLine("// Y is major axis");
      EmitLine(
          "xe_cube_result.x = (xe_cube_y < 0.0) ? -xe_cube_z : xe_cube_z;");
      EmitLine("xe_cube_result.y = xe_cube_x;");
      EmitLine("xe_cube_result.z = 2.0 * xe_cube_y;");
      EmitLine("xe_cube_result.w = (xe_cube_y < 0.0) ? 3.0 : 2.0;");
      Outdent();
      EmitLine("} else {");
      Indent();
      EmitLine("// X is major axis");
      EmitLine("xe_cube_result.x = -xe_cube_y;");
      EmitLine(
          "xe_cube_result.y = (xe_cube_x < 0.0) ? xe_cube_z : -xe_cube_z;");
      EmitLine("xe_cube_result.z = 2.0 * xe_cube_x;");
      EmitLine("xe_cube_result.w = (xe_cube_x < 0.0) ? 1.0 : 0.0;");
      Outdent();
      EmitLine("}");
      // Store result.
      EmitVectorResultAssignment(instr.vector_and_constant_result,
                                 "xe_cube_result");
      StoreConstantComponents(instr.vector_and_constant_result);
      Outdent();
      EmitLine("}");
      // Return early - we handled the result store.
      return;
    }

    case AluVectorOpcode::kMax4:
      // Find maximum of all 4 components.
      result = "(max(max(" + op0 + ".x, " + op0 + ".y), max(" + op0 + ".z, " +
               op0 + ".w))).xxxx";
      break;

    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush: {
      // These set the predicate and return a value.
      // predicate = comparison(src0.w, 0) && comparison2(src1.w, 0)
      // result.x = comparison(src0.x, 0) && comparison2(src1.x, 0) ? 0 : src0.x
      // + 1
      std::string cmp_op, cmp_op2;
      switch (instr.vector_opcode) {
        case AluVectorOpcode::kSetpEqPush:
          cmp_op = "==";
          cmp_op2 = "==";
          break;
        case AluVectorOpcode::kSetpNePush:
          cmp_op = "==";
          cmp_op2 = "!=";
          break;
        case AluVectorOpcode::kSetpGtPush:
          cmp_op = "==";
          cmp_op2 = ">";
          break;
        case AluVectorOpcode::kSetpGePush:
          cmp_op = "==";
          cmp_op2 = ">=";
          break;
        default:
          break;
      }
      EmitLine("xe_p0 = (" + op0 + ".w " + cmp_op + " 0.0) && (" + op1 + ".w " +
               cmp_op2 + " 0.0);");
      // Scalar condition for ternary is fine
      result = "(((" + op0 + ".x " + cmp_op + " 0.0) && (" + op1 + ".x " +
               cmp_op2 +
               " 0.0)) ? "
               "float4(0.0, 0.0, 0.0, 0.0) : (" +
               op0 + " + float4(1.0, 1.0, 1.0, 1.0)))";
    } break;

    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe: {
      std::string cmp_op;
      switch (instr.vector_opcode) {
        case AluVectorOpcode::kKillEq:
          cmp_op = "==";
          break;
        case AluVectorOpcode::kKillGt:
          cmp_op = ">";
          break;
        case AluVectorOpcode::kKillGe:
          cmp_op = ">=";
          break;
        case AluVectorOpcode::kKillNe:
          cmp_op = "!=";
          break;
        default:
          break;
      }
      EmitLine("{");
      Indent();
      EmitLine("bool4 xe_kill_cmp = (" + op0 + " " + cmp_op + " " + op1 + ");");
      EmitLine("if (any(xe_kill_cmp)) { discard; }");
      Outdent();
      EmitLine("}");
      // Scalar result replicated to all components
      result = "(any(" + op0 + " " + cmp_op + " " + op1 +
               ") ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 0.0))";
    } break;

    case AluVectorOpcode::kDst:
      // dst instruction: dest = (1, src0.y*src1.y, src0.z, src1.w)
      result = "float4(1.0, XeMulSM3(" + op0 + ".y, " + op1 + ".y), " + op0 +
               ".z, " + op1 + ".w)";
      break;

    case AluVectorOpcode::kMaxA: {
      // Update address register and return max.
      EmitLine("xe_a0 = clamp(int(floor(" + op0 + ".w + 0.5)), -256, 255);");
      // Optimization: if both operands are identical, just use the operand.
      if (op0 == op1) {
        result = op0;
      } else {
        result =
            "select((" + op0 + " >= " + op1 + "), " + op0 + ", " + op1 + ")";
      }
    } break;

    default:
      // Unhandled opcode - emit as zero.
      XELOGW("HLSL: Unhandled vector opcode: {}", instr.vector_opcode_name);
      result = "float4(0.0, 0.0, 0.0, 0.0)";
      break;
  }

  // Store result with proper swizzle matching.
  if (!result.empty()) {
    EmitVectorResultAssignment(instr.vector_and_constant_result, result);
    // Store any constant components (k0 or k1) in the write mask.
    // For example, oC0.0y01 means X=0, Y=computed, Z=0, W=1.
    StoreConstantComponents(instr.vector_and_constant_result);
  }
}

void HlslShaderTranslator::ProcessScalarAluInstruction(
    const ParsedAluInstruction& instr) {
  using AluScalarOpcode = ucode::AluScalarOpcode;

  // kRetainPrev is a no-op.
  if (instr.scalar_opcode == AluScalarOpcode::kRetainPrev) {
    return;
  }

  // Get operands. Scalar ops take 1-2 operands.
  // The first operand has two components (a, b) accessed differently per op.
  std::string op0_a, op0_b, op1;
  if (instr.scalar_operand_count > 0) {
    // Get component a and b from the first operand.
    const auto& operand0 = instr.scalar_operands[0];
    SwizzleSource comp_a = operand0.components[0];
    SwizzleSource comp_b = operand0.components[1];
    std::string base = OperandToHlslNoSwizzle(operand0);

    // Apply modifiers.
    std::string base_mod = base;
    if (operand0.is_absolute_value) {
      base_mod = "abs(" + base + ")";
    }
    if (operand0.is_negated) {
      base_mod = "-(" + base_mod + ")";
    }

    op0_a = base_mod + "." + GetCharForSwizzle(comp_a);
    op0_b = base_mod + "." + GetCharForSwizzle(comp_b);
  }
  if (instr.scalar_operand_count > 1) {
    op1 = OperandToHlsl(instr.scalar_operands[1], 1);
  }

  std::string result;

  switch (instr.scalar_opcode) {
    case AluScalarOpcode::kAdds:
      result = "(" + op0_a + " + " + op0_b + ")";
      break;

    case AluScalarOpcode::kAddsPrev:
      result = "(" + op0_a + " + xe_ps.x)";
      break;

    case AluScalarOpcode::kMuls:
      result = "XeMulSM3(" + op0_a + ", " + op0_b + ")";
      break;

    case AluScalarOpcode::kMulsPrev:
      result = "XeMulSM3(" + op0_a + ", xe_ps.x)";
      break;

    case AluScalarOpcode::kMulsPrev2:
      // Complex LIT emulation operation.
      result =
          "((xe_ps.x == -3.402823466e+38 || !isfinite(xe_ps.x) || "
          "!isfinite(" +
          op0_b + ") || " + op0_b +
          " <= 0.0) ? "
          "-3.402823466e+38 : XeMulSM3(" +
          op0_a + ", xe_ps.x))";
      break;

    case AluScalarOpcode::kMaxs:
      // SM3 NaN behavior: a >= b ? a : b
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result = "((" + op0_a + " >= " + op0_b + ") ? " + op0_a + " : " +
                 op0_b + ")";
      }
      break;

    case AluScalarOpcode::kMins:
      // SM3 NaN behavior: a < b ? a : b
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result =
            "((" + op0_a + " < " + op0_b + ") ? " + op0_a + " : " + op0_b + ")";
      }
      break;

    case AluScalarOpcode::kSeqs:
      result = "((" + op0_a + " == 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSgts:
      result = "((" + op0_a + " > 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSges:
      result = "((" + op0_a + " >= 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSnes:
      result = "((" + op0_a + " != 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kFrcs:
      result = "frac(" + op0_a + ")";
      break;

    case AluScalarOpcode::kTruncs:
      result = "trunc(" + op0_a + ")";
      break;

    case AluScalarOpcode::kFloors:
      result = "floor(" + op0_a + ")";
      break;

    case AluScalarOpcode::kExp:
      result = "exp2(" + op0_a + ")";
      break;

    case AluScalarOpcode::kLogc:
      result = "((log2(" + op0_a + ") == -1.0/0.0) ? -3.402823466e+38 : log2(" +
               op0_a + "))";
      break;

    case AluScalarOpcode::kLog:
      result = "log2(" + op0_a + ")";
      break;

    case AluScalarOpcode::kRcpc:
      // Reciprocal with infinity clamped to FLT_MAX.
      result = "((abs(1.0 / " + op0_a +
               ") == 1.0/0.0) ? "
               "(sign(1.0 / " +
               op0_a + ") * 3.402823466e+38) : (1.0 / " + op0_a + "))";
      break;

    case AluScalarOpcode::kRcpf:
      // Reciprocal with infinity flushed to zero.
      result = "((abs(1.0 / " + op0_a + ") == 1.0/0.0) ? 0.0 : (1.0 / " +
               op0_a + "))";
      break;

    case AluScalarOpcode::kRcp:
      result = "(1.0 / " + op0_a + ")";
      break;

    case AluScalarOpcode::kRsqc:
      // Reciprocal square root with infinity clamped.
      result = "((abs(rsqrt(" + op0_a +
               ")) == 1.0/0.0) ? "
               "(sign(rsqrt(" +
               op0_a + ")) * 3.402823466e+38) : rsqrt(" + op0_a + "))";
      break;

    case AluScalarOpcode::kRsqf:
      // Reciprocal square root with infinity flushed.
      result = "((abs(rsqrt(" + op0_a + ")) == 1.0/0.0) ? 0.0 : rsqrt(" +
               op0_a + "))";
      break;

    case AluScalarOpcode::kRsq:
      result = "rsqrt(" + op0_a + ")";
      break;

    case AluScalarOpcode::kMaxAs:
      // Update address register (clamped 0-255) and return max.
      EmitLine("xe_a0 = clamp(int(floor(" + op0_a + " + 0.5)), 0, 255);");
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result = "((" + op0_a + " >= " + op0_b + ") ? " + op0_a + " : " +
                 op0_b + ")";
      }
      break;

    case AluScalarOpcode::kMaxAsf:
      // Update address register (floored, clamped 0-255) and return max.
      EmitLine("xe_a0 = clamp(int(floor(" + op0_a + ")), 0, 255);");
      // Optimization: if both operands are identical, just use the operand.
      if (op0_a == op0_b) {
        result = op0_a;
      } else {
        result = "((" + op0_a + " >= " + op0_b + ") ? " + op0_a + " : " +
                 op0_b + ")";
      }
      break;

    case AluScalarOpcode::kSubs:
      result = "(" + op0_a + " - " + op0_b + ")";
      break;

    case AluScalarOpcode::kSubsPrev:
      result = "(" + op0_a + " - xe_ps.x)";
      break;

    case AluScalarOpcode::kSetpEq:
      EmitLine("xe_p0 = (" + op0_a + " == 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpNe:
      EmitLine("xe_p0 = (" + op0_a + " != 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpGt:
      EmitLine("xe_p0 = (" + op0_a + " > 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpGe:
      EmitLine("xe_p0 = (" + op0_a + " >= 0.0);");
      result = "(xe_p0 ? 0.0 : 1.0)";
      break;

    case AluScalarOpcode::kSetpInv:
      // Sets predicate to (src == 1.0), result is (src == 0.0 ? 1.0 : src)
      // unless pred true then 0.
      EmitLine("xe_p0 = (" + op0_a + " == 1.0);");
      result = "(xe_p0 ? 0.0 : ((" + op0_a + " == 0.0) ? 1.0 : " + op0_a + "))";
      break;

    case AluScalarOpcode::kSetpPop:
      // Decrements and sets predicate if <= 0. Use inline expression.
      EmitLine("xe_p0 = ((" + op0_a + " - 1.0) <= 0.0);");
      result = "(xe_p0 ? 0.0 : (" + op0_a + " - 1.0))";
      break;

    case AluScalarOpcode::kSetpClr:
      EmitLine("xe_p0 = false;");
      result = "3.402823466e+38";
      break;

    case AluScalarOpcode::kSetpRstr:
      EmitLine("xe_p0 = (" + op0_a + " == 0.0);");
      result = "(xe_p0 ? 0.0 : " + op0_a + ")";
      break;

    case AluScalarOpcode::kKillsEq:
      EmitLine("if (" + op0_a + " == 0.0) { discard; }");
      result = "((" + op0_a + " == 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsGt:
      EmitLine("if (" + op0_a + " > 0.0) { discard; }");
      result = "((" + op0_a + " > 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsGe:
      EmitLine("if (" + op0_a + " >= 0.0) { discard; }");
      result = "((" + op0_a + " >= 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsNe:
      EmitLine("if (" + op0_a + " != 0.0) { discard; }");
      result = "((" + op0_a + " != 0.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kKillsOne:
      EmitLine("if (" + op0_a + " == 1.0) { discard; }");
      result = "((" + op0_a + " == 1.0) ? 1.0 : 0.0)";
      break;

    case AluScalarOpcode::kSqrt:
      result = "sqrt(" + op0_a + ")";
      break;

    case AluScalarOpcode::kSin:
      result = "sin(" + op0_a + ")";
      break;

    case AluScalarOpcode::kCos:
      result = "cos(" + op0_a + ")";
      break;

    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1:
      result = "XeMulSM3(" + op0_a + ", " + op1 + ")";
      break;

    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1:
      result = "(" + op0_a + " + " + op1 + ")";
      break;

    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1:
      result = "(" + op0_a + " - " + op1 + ")";
      break;

    default:
      // Unhandled opcode.
      XELOGW("HLSL: Unhandled scalar opcode: {}", instr.scalar_opcode_name);
      result = "0.0";
      break;
  }

  // Update ps with the scalar result, then write the dest from ps.
  if (!result.empty()) {
    EmitLine("xe_ps = float4(" + result + ", " + result + ", " + result + ", " +
             result + ");");
    EmitScalarResultAssignment(instr.scalar_result, "xe_ps.x");
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
