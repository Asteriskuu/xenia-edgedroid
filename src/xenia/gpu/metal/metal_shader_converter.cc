/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/metal/metal_shader_converter.h"

#include <algorithm>
#include <vector>

#include "metal_irconverter.h"

#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace metal {

namespace {

// SpirvShaderTranslator needs real IEEE semantics - NaN position culling, NaN
// flushed to 0 on saturate, ordered alpha test compares, INFINITY clamps in
// rcp/rsq/log - which MSC 4.0 optimizes away unless DisableNanInfOptimization
// keeps its pre-4.0 behavior. ForceTextureArray matches the 2D array textures
// the texture cache creates.
constexpr IRCompatibilityFlags kCompatibilityFlags = IRCompatibilityFlags(
    IRCompatibilityFlagForceTextureArray | IRCompatibilityFlagBoundsCheck |
    IRCompatibilityFlagVertexPositionInfToNan |
    IRCompatibilityFlagDisableNanInfOptimization);

// Descriptor set 0 bindings, which Mesa maps to space0 registers of the
// matching index. 2 and 3 are the per-stage index buffers the bindless lowering
// adds for descriptor sets 2 and 3.
constexpr uint32_t kSpace0SharedMemory = 0;
constexpr uint32_t kSpace0TextureIndicesVertex = 2;
constexpr uint32_t kSpace0TextureIndicesPixel = 3;

// Descriptor sets 2 (vertex textures) and 3 (pixel textures) map to the
// register spaces of the same number.
constexpr uint32_t kSpaceTexturesVertex = 2;
constexpr uint32_t kSpaceTexturesPixel = 3;

// Where Mesa parks Dozen's runtime data CBV, per MakeRuntimeConf.
constexpr uint32_t kSpaceRuntimeData = 31;

// SpirvShaderTranslator::ConstantBuffer, mapped to space1 registers of the
// matching index.
constexpr uint32_t kSpaceConstants = 1;

IRShaderStage ToIRShaderStage(MetalShaderStage stage) {
  switch (stage) {
    case MetalShaderStage::kVertex:
      return IRShaderStageVertex;
    case MetalShaderStage::kFragment:
      return IRShaderStageFragment;
    default:
      return IRShaderStageCompute;
  }
}

const char* StageName(MetalShaderStage stage) {
  switch (stage) {
    case MetalShaderStage::kVertex:
      return "vertex";
    case MetalShaderStage::kFragment:
      return "fragment";
    default:
      return "compute";
  }
}

// What a root parameter should look like in MSC's reflection. Descriptor tables
// report no space or slot, so they carry IRResourceTypeInvalid.
struct RootParameterKey {
  IRResourceType type;
  uint32_t space;
  uint32_t slot;
};

RootParameterKey RootParameterKeyOf(MetalRootParameter parameter) {
  switch (parameter) {
    case MetalRootParameter::kSystemConstants:
      return {IRResourceTypeCBV, kSpaceConstants, 0};
    case MetalRootParameter::kFloatConstantsVertex:
      return {IRResourceTypeCBV, kSpaceConstants, 1};
    case MetalRootParameter::kFloatConstantsPixel:
      return {IRResourceTypeCBV, kSpaceConstants, 2};
    case MetalRootParameter::kBoolLoopConstants:
      return {IRResourceTypeCBV, kSpaceConstants, 3};
    case MetalRootParameter::kFetchConstants:
      return {IRResourceTypeCBV, kSpaceConstants, 4};
    case MetalRootParameter::kRuntimeData:
      return {IRResourceTypeCBV, kSpaceRuntimeData, 0};
    case MetalRootParameter::kSharedMemorySrv:
      return {IRResourceTypeSRV, 0, kSpace0SharedMemory};
    case MetalRootParameter::kSharedMemoryUav:
      return {IRResourceTypeUAV, 0, kSpace0SharedMemory};
    case MetalRootParameter::kTextureIndicesVertex:
      return {IRResourceTypeSRV, 0, kSpace0TextureIndicesVertex};
    case MetalRootParameter::kTextureIndicesPixel:
      return {IRResourceTypeSRV, 0, kSpace0TextureIndicesPixel};
    default:
      return {IRResourceTypeInvalid, 0, 0};
  }
}

}  // namespace

MetalShaderConverter::~MetalShaderConverter() {
  if (root_signature_) {
    IRRootSignatureDestroy(root_signature_);
  }
}

bool MetalShaderConverter::Initialize() {
  IRCompiler* probe = IRCompilerCreate();
  if (!probe) {
    XELOGE("MetalShaderConverter: failed to create an IR compiler");
    return false;
  }
  IRCompilerDestroy(probe);

  root_signature_ = CreateRootSignature();
  if (!root_signature_) {
    return false;
  }
  if (!QueryRootParameterOffsets()) {
    return false;
  }

  is_available_ = true;
  XELOGI("MetalShaderConverter: initialized, {} byte top-level argument buffer",
         argument_buffer_size_);
  return true;
}

IRRootSignature* MetalShaderConverter::CreateRootSignature() const {
  IRDescriptorRange1 ranges[uint32_t(MetalRootParameter::kCount)] = {};
  IRRootDescriptorTable1 tables[uint32_t(MetalRootParameter::kCount)] = {};
  IRRootParameter1 parameters[uint32_t(MetalRootParameter::kCount)] = {};
  uint32_t range_count = 0;
  uint32_t table_count = 0;
  uint32_t parameter_count = 0;

  auto append_root_descriptor = [&](IRRootParameterType type,
                                    uint32_t shader_register,
                                    uint32_t register_space) {
    IRRootParameter1& parameter = parameters[parameter_count++];
    parameter.ParameterType = type;
    parameter.Descriptor.ShaderRegister = shader_register;
    parameter.Descriptor.RegisterSpace = register_space;
    parameter.Descriptor.Flags = IRRootDescriptorFlagNone;
    parameter.ShaderVisibility = IRShaderVisibilityAll;
  };

  // The bindless lowering leaves the original texture and sampler declarations
  // in the DXIL, so the root signature still has to cover them.
  auto append_unbounded_table = [&](IRDescriptorRangeType type,
                                    uint32_t register_space) {
    IRDescriptorRange1& range = ranges[range_count++];
    range.RangeType = type;
    range.NumDescriptors = UINT32_MAX;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = register_space;
    range.Flags = IRDescriptorRangeFlagNone;
    range.OffsetInDescriptorsFromTableStart = 0;

    IRRootDescriptorTable1& table = tables[table_count++];
    table.NumDescriptorRanges = 1;
    table.pDescriptorRanges = &range;

    IRRootParameter1& parameter = parameters[parameter_count++];
    parameter.ParameterType = IRRootParameterTypeDescriptorTable;
    parameter.DescriptorTable = table;
    parameter.ShaderVisibility = IRShaderVisibilityAll;
  };

  // Order must match MetalRootParameter so the offsets line up one to one.
  append_root_descriptor(IRRootParameterTypeCBV, 0, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 1, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 2, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 3, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 4, kSpaceConstants);
  append_root_descriptor(IRRootParameterTypeCBV, 0, kSpaceRuntimeData);
  append_root_descriptor(IRRootParameterTypeSRV, kSpace0SharedMemory, 0);
  append_root_descriptor(IRRootParameterTypeUAV, kSpace0SharedMemory, 0);
  append_root_descriptor(IRRootParameterTypeSRV, kSpace0TextureIndicesVertex,
                         0);
  append_root_descriptor(IRRootParameterTypeSRV, kSpace0TextureIndicesPixel, 0);
  append_unbounded_table(IRDescriptorRangeTypeSRV, kSpaceTexturesVertex);
  append_unbounded_table(IRDescriptorRangeTypeSRV, kSpaceTexturesPixel);
  append_unbounded_table(IRDescriptorRangeTypeSampler, kSpaceTexturesVertex);
  append_unbounded_table(IRDescriptorRangeTypeSampler, kSpaceTexturesPixel);

  IRRootSignatureDescriptor1 descriptor = {};
  descriptor.NumParameters = parameter_count;
  descriptor.pParameters = parameters;
  descriptor.NumStaticSamplers = 0;
  descriptor.pStaticSamplers = nullptr;
  // The lowered shader indexes ResourceDescriptorHeap / SamplerDescriptorHeap
  // directly, which MSC serves from the heaps bound at
  // kIRDescriptorHeapBindPoint and kIRSamplerHeapBindPoint.
  descriptor.Flags =
      IRRootSignatureFlags(IRRootSignatureFlagCBVSRVUAVHeapDirectlyIndexed |
                           IRRootSignatureFlagSamplerHeapDirectlyIndexed);

  IRVersionedRootSignatureDescriptor versioned = {};
  versioned.version = IRRootSignatureVersion_1_1;
  versioned.desc_1_1 = descriptor;

  IRError* error = nullptr;
  IRRootSignature* root_signature =
      IRRootSignatureCreateFromDescriptor(&versioned, &error);
  if (error) {
    const char* message = static_cast<const char*>(IRErrorGetPayload(error));
    XELOGE("MetalShaderConverter: failed to create the root signature: {}",
           message ? message : "unknown error");
    IRErrorDestroy(error);
    return nullptr;
  }
  return root_signature;
}

bool MetalShaderConverter::QueryRootParameterOffsets() {
  std::fill(std::begin(root_parameter_offsets_),
            std::end(root_parameter_offsets_), UINT32_MAX);

  constexpr uint32_t kParameterCount = uint32_t(MetalRootParameter::kCount);
  size_t location_count = IRRootSignatureGetResourceCount(root_signature_);
  if (location_count != kParameterCount) {
    XELOGE(
        "MetalShaderConverter: the root signature reports {} top-level "
        "resources, expected {}",
        location_count, kParameterCount);
    return false;
  }
  std::vector<IRResourceLocation> locations(location_count);
  IRRootSignatureGetResourceLocations(root_signature_, locations.data());

  // Locations come back in declaration order, which MetalRootParameter mirrors,
  // so position is what identifies them - table entries carry no space or slot
  // to match on. Checking the rest against their declared register turns a
  // reordering on either side into a failure here rather than a misbind.
  for (uint32_t i = 0; i < kParameterCount; ++i) {
    const IRResourceLocation& location = locations[i];
    RootParameterKey key = RootParameterKeyOf(MetalRootParameter(i));
    if (key.type != IRResourceTypeInvalid &&
        (location.resourceType != key.type || location.space != key.space ||
         location.slot != key.slot)) {
      XELOGE(
          "MetalShaderConverter: root parameter {} is type {} space {} slot "
          "{}, "
          "expected type {} space {} slot {}",
          i, uint32_t(location.resourceType), location.space, location.slot,
          uint32_t(key.type), key.space, key.slot);
      return false;
    }
    root_parameter_offsets_[i] = location.topLevelOffset;
    argument_buffer_size_ =
        std::max(argument_buffer_size_,
                 uint32_t(location.topLevelOffset + location.sizeBytes));
  }
  return true;
}

MetalShaderConversionResult MetalShaderConverter::Convert(
    MetalShaderStage stage, const std::vector<uint8_t>& dxil) const {
  MetalShaderConversionResult result;
  if (!is_available_) {
    result.error_message = "MetalShaderConverter is not initialized";
    return result;
  }
  if (dxil.empty()) {
    result.error_message = "Empty DXIL";
    return result;
  }

  IRObject* dxil_object =
      IRObjectCreateFromDXIL(dxil.data(), dxil.size(), IRBytecodeOwnershipNone);
  if (!dxil_object) {
    result.error_message = "Failed to create the DXIL object";
    return result;
  }
  IRCompiler* compiler = IRCompilerCreate();
  if (!compiler) {
    IRObjectDestroy(dxil_object);
    result.error_message = "Failed to create an IR compiler";
    return result;
  }

  IRCompilerSetCompatibilityFlags(compiler, kCompatibilityFlags);
  IRCompilerSetGlobalRootSignature(compiler, root_signature_);
  // Mesa embeds no root signature, but the flag also keeps MSC from inferring
  // one and disagreeing with ours.
  IRCompilerIgnoreRootSignature(compiler, true);

  IRError* error = nullptr;
  IRObject* metal_object =
      IRCompilerAllocCompileAndLink(compiler, nullptr, dxil_object, &error);
  if (error) {
    const char* message = static_cast<const char*>(IRErrorGetPayload(error));
    result.error_message = std::string("MSC compilation failed: ") +
                           (message ? message : "unknown error");
    IRErrorDestroy(error);
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxil_object);
    return result;
  }
  if (!metal_object) {
    result.error_message = "MSC returned no object and no error";
    IRCompilerDestroy(compiler);
    IRObjectDestroy(dxil_object);
    return result;
  }

  IRShaderStage ir_stage = ToIRShaderStage(stage);
  IRMetalLibBinary* metallib = IRMetalLibBinaryCreate();
  if (metallib) {
    if (IRObjectGetMetalLibBinary(metal_object, ir_stage, metallib)) {
      size_t size = IRMetalLibGetBytecodeSize(metallib);
      if (size) {
        result.metallib.resize(size);
        IRMetalLibGetBytecode(metallib, result.metallib.data());
      }
    }
    IRMetalLibBinaryDestroy(metallib);
  }

  IRShaderReflection* reflection = IRShaderReflectionCreate();
  if (reflection) {
    if (IRObjectGetReflection(metal_object, ir_stage, reflection)) {
      const char* entry_point_name =
          IRShaderReflectionGetEntryPointFunctionName(reflection);
      if (entry_point_name) {
        result.entry_point_name = entry_point_name;
      }
    }
    IRShaderReflectionDestroy(reflection);
  }

  IRObjectDestroy(metal_object);
  IRCompilerDestroy(compiler);
  IRObjectDestroy(dxil_object);

  if (result.metallib.empty()) {
    result.error_message =
        std::string("MSC produced an empty ") + StageName(stage) + " metallib";
    return result;
  }
  if (result.entry_point_name.empty()) {
    result.error_message = std::string("MSC reported no ") + StageName(stage) +
                           " entry point name";
    return result;
  }

  result.success = true;
  return result;
}

}  // namespace metal
}  // namespace gpu
}  // namespace xe
