/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_HLSL_SHADER_TRANSLATOR_H_
#define XENIA_GPU_HLSL_SHADER_TRANSLATOR_H_

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/shader_translator.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/graphics_provider.h"

namespace xe {
namespace gpu {

namespace d3d12 {
class DxcCompiler;
}  // namespace d3d12

// Generates HLSL source code for compilation via DXC to DXIL (SM 6.x).
// This translator inherits the Modification structure from DxbcShaderTranslator
// for compatibility with the existing pipeline cache infrastructure.
//
// The generated HLSL must match the constant buffer layouts and resource
// bindings defined in xenos_draw.hlsli.
class HlslShaderTranslator : public ShaderTranslator {
 public:
  // Use the same Modification type as DxbcShaderTranslator for compatibility.
  using Modification = DxbcShaderTranslator::Modification;

  // use_shader_model_6_6: If true, uses SM 6.6 with ResourceDescriptorHeap.
  //                        If false, uses SM 6.0 with unbounded SRV arrays.
  HlslShaderTranslator(ui::GraphicsProvider::GpuVendorID vendor_id,
                       bool bindless_resources_used, bool edram_rov_used,
                       bool gamma_render_target_as_unorm8 = false,
                       bool msaa_2x_supported = true,
                       bool use_shader_model_6_6 = true,
                       uint32_t draw_resolution_scale_x = 1,
                       uint32_t draw_resolution_scale_y = 1);
  ~HlslShaderTranslator() override;

  // Returns the generated HLSL source code. Valid after translation.
  const std::string& GetHlslSource() const { return hlsl_source_; }

  // Get the shader target profile string for DXC compilation.
  // Returns "vs_6_0", "ps_6_0", etc.
  std::string GetShaderTargetProfile() const;

  // Set the DXC compiler for DXIL output. If null, CompleteTranslation returns
  // HLSL source as bytes.
  void SetDxcCompiler(d3d12::DxcCompiler* compiler) {
    dxc_compiler_ = compiler;
  }

  // Offset to the unbounded guest SRV range in the view heap. Added to
  // bindless texture indices, which the command processor stores relative to
  // that range.
  void SetBindlessSrvHeapOffset(uint32_t offset) {
    bindless_srv_heap_offset_ = offset;
  }

  uint64_t GetDefaultVertexShaderModification(
      uint32_t dynamic_addressable_register_count,
      Shader::HostVertexShaderType host_vertex_shader_type =
          Shader::HostVertexShaderType::kVertex) const override;
  uint64_t GetDefaultPixelShaderModification(
      uint32_t dynamic_addressable_register_count) const override;

 protected:
  void Reset() override;
  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;
  uint32_t GetModificationRegisterCount() const override;

  void ProcessLabel(uint32_t cf_index) override;
  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(
      const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(
      const ParsedLoopEndInstruction& instr) override;
  void ProcessCallInstruction(const ParsedCallInstruction& instr) override;
  void ProcessReturnInstruction(const ParsedReturnInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                               uint8_t export_eM) override;
  void ProcessVertexFetchInstruction(
      const ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(
      const ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(
      const ParsedAluInstruction& instr,
      uint8_t memexport_eM_potentially_written_before) override;

 private:
  Modification GetHlslShaderModification() const {
    return Modification(current_translation().modification());
  }

  // Whether the current shader is a tessellation domain shader (a guest vertex
  // shader evaluated per tessellated patch point).
  bool IsDomainShader() const {
    return is_vertex_shader() &&
           Shader::IsHostVertexShaderTypeDomain(
               GetHlslShaderModification().vertex.host_vertex_shader_type);
  }
  // Fills the HLSL domain ("tri"/"quad"), control point count and
  // SV_DomainLocation component count for the current domain shader. Returns
  // false for unsupported domain types.
  bool GetDomainShaderInfo(const char*& domain_out,
                           uint32_t& control_point_count_out,
                           uint32_t& domain_location_component_count_out) const;
  // Emit the domain shader prologue: domain location and host-provided control
  // point indices into the guest registers, matching the DXBC translator.
  void EmitDomainShaderPrologue();

  // Emit system constant buffer declaration (must match xenos_draw.hlsli).
  void EmitSystemConstants();
  // Emit float/bool/loop constant buffer declarations.
  void EmitConstantBuffers();
  // Emit texture and sampler declarations.
  void EmitResourceDeclarations();
  // Emit input structure for the shader stage.
  void EmitInputDeclarations();
  // Emit output structure for the shader stage.
  void EmitOutputDeclarations();
  // Emit helper functions used by the shader.
  void EmitHelperFunctions();

  // Whether this shader performs memory export (writes to eM#).
  bool MemExportUsed() const {
    return current_shader().memexport_eM_written() != 0;
  }

  // Whether the pixel shader manually interpolates inputs via barycentric
  // coordinates (precise interpolation). Set only when the device supports
  // SV_Barycentrics; not used for point primitives (no barycentrics).
  bool UsePreciseInterpolation() const {
    return is_pixel_shader() &&
           GetHlslShaderModification().pixel.precise_interpolation &&
           !GetHlslShaderModification().pixel.param_gen_point;
  }
  // Emit the fixed memory-export helper functions (format conversion + store).
  void EmitMemExportHelpers();
  // Emit a flush of the currently-written eM# elements to shared memory.
  void EmitMemExportFlush();
  // Mark an eM# element as written when a result targets export data.
  void EmitMemExportWrittenMark(const InstructionResult& result);
  // Emit a kill conditional: flush any in-flight exports, then discard.
  void EmitKill(const std::string& condition);

  // Emit per-invocation shader state as static globals, so subroutine functions
  // can share it. The program counter stays local to each function.
  void EmitGlobalState();
  // Emit the shader entry point signature.
  void EmitEntryPointBegin();
  // Emit the shader entry point closing.
  void EmitEntryPointEnd();

  // Write a line to the current output stream with proper indentation.
  void EmitLine(const std::string& line);
  // Write raw text to the current output stream.
  void Emit(const std::string& text);
  // Increase indentation level.
  void Indent();
  // Decrease indentation level.
  void Outdent();

  // Code-gen helpers shared across the split translator .cc files (static so
  // they have external linkage rather than being file-local).
  static std::string HlslFloatLiteral(float value);

  // Convert an operand to HLSL representation.
  std::string OperandToHlsl(const InstructionOperand& operand,
                            uint32_t needed_components);
  // Convert an operand to HLSL without applying swizzle (for manual swizzling).
  std::string OperandToHlslNoSwizzle(const InstructionOperand& operand);
  // Convert a result storage target to HLSL.
  std::string ResultToHlsl(const InstructionResult& result);
  // Convert temporary register storage to HLSL.
  std::string RegisterToHlsl(uint32_t storage_index,
                             InstructionStorageAddressingMode mode) const;
  // Whether this pixel shader needs an SV_Depth output for float24 conversion.
  bool PixelShaderNeedsFloat24DepthOutput() const;
  // Whether this pixel shader writes an SV_Depth-family output.
  bool PixelShaderWritesDepthOutput() const;
  // Whether this non-ROV pixel shader applies polygon offset via shader depth
  // output instead of fixed-function bias.
  bool PixelShaderAppliesPolygonOffset() const;
  // Whether early-depth/stencil is forced globally for this pixel shader.
  bool IsForceEarlyDepthStencilEnabled() const;
  // Whether the pixel shader runs at sample rate (per-sample float24 depth).
  bool IsSampleRate() const;
  // Whether this pixel shader needs an SV_Coverage output for alpha-to-mask.
  bool PixelShaderNeedsCoverageOutput() const;
  // Whether a result write must be saturated to 0..1.
  bool ResultNeedsSaturation(const InstructionResult& result) const;
  // Wrap an emitted expression in saturate() when the result requires it.
  std::string SaturateExpressionIfNeeded(const InstructionResult& result,
                                         const std::string& expression) const;
  // Get swizzle string for components.
  std::string GetSwizzleString(const SwizzleSource* components,
                               uint32_t component_count);
  // Get write mask string from mask bits.
  std::string GetWriteMaskString(uint32_t write_mask);
  // Emit assignment with proper swizzle matching for write masks.
  void EmitVectorResultAssignment(const InstructionResult& result,
                                  const std::string& source_expr);
  // Emit assignment for scalar results (replicates scalar to match mask).
  void EmitScalarResultAssignment(const InstructionResult& result,
                                  const std::string& scalar_expr);
  // Clamp point size exports after writing them, matching the DXBC path's
  // signed-integer bitwise clamp behavior.
  void EmitPointSizeClampIfNeeded(const InstructionResult& result,
                                  uint32_t write_mask);
  // Emit PsParamGen pseudo-interpolator initialization for pixel shaders.
  void EmitPixelShaderParamGen();
  // Emit fixed-function-style pixel kill / coverage logic.
  void EmitPixelShaderAlphaTest();
  void EmitPixelShaderAlphaToCoverage();
  // Store constant-only components (k0 or k1) to the result destination.
  // Called after ALU processing to handle cases where all components are
  // constants and the ALU operation returned early.
  void StoreConstantComponents(const InstructionResult& result);

  // Process vector ALU operations.
  void ProcessVectorAluInstruction(const ParsedAluInstruction& instr);
  // Process scalar ALU operations.
  void ProcessScalarAluInstruction(const ParsedAluInstruction& instr);

  // Called after translation completes to copy bindings to shader object.
  void PostTranslation() override;

  // Texture binding tracking for bindless resources.
  struct TextureBinding {
    uint32_t bindful_srv_index;
    uint32_t bindless_descriptor_index;
    uint32_t fetch_constant;
    xenos::FetchOpDimension dimension;
    bool is_signed;
  };
  // Find an existing texture binding or add a new one.
  uint32_t FindOrAddTextureBinding(uint32_t fetch_constant,
                                   xenos::FetchOpDimension dimension,
                                   bool is_signed);

  // Sampler binding tracking for bindless resources.
  struct SamplerBinding {
    uint32_t bindless_descriptor_index;
    uint32_t fetch_constant;
    xenos::TextureFilter mag_filter;
    xenos::TextureFilter min_filter;
    xenos::TextureFilter mip_filter;
    xenos::AnisoFilter aniso_filter;
  };
  // Find an existing sampler binding or add a new one.
  uint32_t FindOrAddSamplerBinding(uint32_t fetch_constant,
                                   xenos::TextureFilter mag_filter,
                                   xenos::TextureFilter min_filter,
                                   xenos::TextureFilter mip_filter,
                                   xenos::AnisoFilter aniso_filter);

  // Get total number of bindless resources (textures + samplers).
  uint32_t GetBindlessResourceCount() const {
    return uint32_t(texture_bindings_.size() + sampler_bindings_.size());
  }

  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;

  ui::GraphicsProvider::GpuVendorID vendor_id_;
  bool bindless_resources_used_;
  bool edram_rov_used_;
  bool gamma_render_target_as_unorm8_;
  bool msaa_2x_supported_;
  bool use_shader_model_6_6_;
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;
  // Added to bindless texture indices to reach the unbounded SRV range.
  uint32_t bindless_srv_heap_offset_ = 0;

  // DXC compiler for HLSL to DXIL compilation. If set, CompleteTranslation
  // compiles HLSL to DXIL. If null, returns HLSL source as bytes.
  d3d12::DxcCompiler* dxc_compiler_ = nullptr;

  // String stream for building HLSL code.
  std::ostringstream hlsl_stream_;
  // Final HLSL source code.
  std::string hlsl_source_;
  // Current indentation level.
  uint32_t indent_level_ = 0;
  // Current indentation string.
  std::string indent_string_;

  // Tracking for control flow.
  bool cf_exec_predicated_ = false;
  bool cf_exec_predicate_condition_ = false;
  uint32_t cf_exec_bool_constant_ = UINT32_MAX;
  bool cf_exec_bool_constant_condition_ = false;

  // State machine tracking.
  bool has_main_switch_ = false;  // True if shader needs switch (has labels)

  // eM# possibly written before the ALU instruction currently being processed.
  // Nonzero means a kill in this instruction must flush exports before discard.
  uint8_t memexport_eM_written_before_kill_ = 0;

  // Predication tracking helpers.
  bool cf_instruction_predicate_if_open_ = false;

  // Helper methods for control flow.
  void CloseInstructionPredication();
  void CloseExecConditionals();
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_HLSL_SHADER_TRANSLATOR_H_
