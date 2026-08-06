/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/edram_dump_shader.h"

#include <vector>

#include "third_party/glslang/SPIRV/GLSL.std.450.h"

#include "xenia/base/assert.h"
#include "xenia/base/math.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

std::vector<uint32_t> BuildEdramDumpShaderSpirv(
    EdramDumpShaderKey key, const EdramDumpShaderOptions& options) {
  bool draw_resolution_scaled =
      options.resolution_scale_x > 1 || options.resolution_scale_y > 1;

  std::vector<spv::Id> id_vector_temp;

  SpirvBuilder builder(options.spirv_version,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_float = builder.makeFloatType(32);

  // Bindings.
  // EDRAM buffer.
  bool format_is_64bpp = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(
                                              key.GetColorFormat());
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeRuntimeArray(format_is_64bpp ? type_uint2 : type_uint));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        sizeof(uint32_t) << uint32_t(format_is_64bpp));
  spv::Id type_edram = builder.makeStructType(id_vector_temp, "XeEdram");
  builder.addMemberName(type_edram, 0, "edram");
  builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_edram, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id edram_buffer = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniform, type_edram, "xe_edram");
  builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet,
                        options.descriptor_set_edram);
  builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    source_is_uint = options.source_is_uint;
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false,
                            source_is_multisampled, 1, spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet,
                        options.descriptor_set_source);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  // Stencil source.
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false,
                              source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          options.descriptor_set_source);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  // Push constants.
  id_vector_temp.clear();
  id_vector_temp.reserve(kEdramDumpShaderPushConstantCount);
  for (uint32_t i = 0; i < kEdramDumpShaderPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants =
      builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  builder.addMemberName(type_push_constants,
                        kEdramDumpShaderPushConstantPitches, "pitches");
  builder.addMemberDecoration(
      type_push_constants, kEdramDumpShaderPushConstantPitches,
      spv::DecorationOffset,
      int(sizeof(uint32_t) * kEdramDumpShaderPushConstantPitches));
  builder.addMemberName(type_push_constants,
                        kEdramDumpShaderPushConstantOffsets, "offsets");
  builder.addMemberDecoration(
      type_push_constants, kEdramDumpShaderPushConstantOffsets,
      spv::DecorationOffset,
      int(sizeof(uint32_t) * kEdramDumpShaderPushConstantOffsets));
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants = builder.createVariable(
      spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
      "xe_edram_dump_push_constants");

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                             type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::GlobalInvocationId));

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id =
      builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x =
      builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  // Dumps for fully native resolves address the EDRAM buffer with the plain
  // 1x1 tile layout.
  uint32_t layout_scale_x = key.native_layout ? 1 : options.resolution_scale_x;
  uint32_t layout_scale_y = key.native_layout ? 1 : options.resolution_scale_y;
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(format_is_64bpp)) *
      layout_scale_x;
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x = builder.createBinOp(
      spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x = builder.createBinOp(
      spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y =
      builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  uint32_t tile_height = xenos::kEdramTileHeightSamples * layout_scale_y;
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y = builder.createBinOp(
      spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  // Get the tile index in the EDRAM relative to the dump rectangle base tile.
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeIntConstant(kEdramDumpShaderPushConstantPitches));
  spv::Id pitches_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits =
      builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                              pitches_constant, const_uint_0,
                              const_edram_pitch_tiles_bits),
          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeIntConstant(kEdramDumpShaderPushConstantOffsets));
  spv::Id offsets_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_uint_0, const_edram_base_tiles_bits_plus_1),
      rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index.
  spv::Id edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(
              spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
              builder.makeUintConstant(xenos::kEdramTileCount - 1))),
      builder.createBinOp(spv::OpIAdd, type_uint,
                          builder.createBinOp(spv::OpIMul, type_uint,
                                              const_tile_width, tile_sample_y),
                          tile_sample_x));
  if (key.is_depth) {
    // Swap 40-sample columns in the depth buffer in the destination address to
    // get the final address of the sample in the EDRAM.
    uint32_t tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int,
                                  edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(),
                                    tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(int32_t(tile_width_half)),
                builder.makeIntConstant(-int32_t(tile_width_half)))));
  }

  // Get the linear tile index within the source texture.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpISub, type_uint, edram_tile_index_non_wrapped,
      builder.createTriOp(
          spv::OpBitFieldUExtract, type_uint, offsets_constant,
          const_edram_base_tiles_bits_plus_1,
          builder.makeUintConstant(xenos::kEdramBaseTilesBits)));
  // Split the linear tile index in the source texture into X and Y in tiles.
  spv::Id source_pitch_tiles = builder.createTriOp(
      spv::OpBitFieldUExtract, type_uint, pitches_constant,
      const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x = builder.createBinOp(
      spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Combine the source tile offset and the sample index within the tile.
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width,
                          source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height,
                          source_tile_index_y),
      tile_sample_y);
  // Get the source pixel coordinate and the sample index within the pixel.
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                         source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      source_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                           source_sample_x, const_uint_1);
      // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1 for
      // vertical.
      source_sample_id = builder.createQuadOp(
          spv::OpBitFieldInsert, type_uint,
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x,
                              const_uint_1),
          source_sample_y, const_uint_1, const_uint_1);
    } else {
      // 2x MSAA source texture sample index - convert from the guest to
      // the Vulkan standard sample locations.
      source_sample_id = builder.createTriOp(
          spv::OpSelect, type_uint,
          builder.createBinOp(
              spv::OpINotEqual, builder.makeBoolType(),
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_y,
                                  const_uint_1),
              const_uint_0),
          builder.makeUintConstant(draw_util::GetD3D10SampleIndexForGuest2xMSAA(
              1, options.msaa_2x_attachments_supported)),
          builder.makeUintConstant(draw_util::GetD3D10SampleIndexForGuest2xMSAA(
              0, options.msaa_2x_attachments_supported)));
    }
  }
  if (key.source_scale_native && !key.native_layout && draw_resolution_scaled) {
    // Native source dumped to the scaled EDRAM layout. Duplicate each pixel
    // into all the scaled sample slots covering it. Done after the sample index
    // is extracted since MSAA isn't affected by scale.
    source_pixel_x = builder.createBinOp(
        spv::OpUDiv, type_uint, source_pixel_x,
        builder.makeUintConstant(options.resolution_scale_x));
    source_pixel_y = builder.createBinOp(
        spv::OpUDiv, type_uint, source_pixel_y,
        builder.makeUintConstant(options.resolution_scale_y));
  }

  // Load the source, and pack the value into one or two 32-bit integers.
  spv::Id packed[2] = {};
  spv::Builder::TextureParameters source_texture_parameters = {};
  source_texture_parameters.sampler =
      builder.createLoad(source_texture, spv::NoPrecision);
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
  source_texture_parameters.coords =
      builder.createCompositeConstruct(type_int2, id_vector_temp);
  if (source_is_multisampled) {
    source_texture_parameters.sample =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  spv::Id source_vec4 = builder.createTextureCall(
      spv::NoPrecision, builder.makeVectorType(source_component_type, 4), false,
      true, false, false, false, source_texture_parameters,
      spv::ImageOperandsMaskNone);
  if (key.is_depth) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    spv::Id source_stencil = builder.createCompositeExtract(
        builder.createTextureCall(
            spv::NoPrecision, builder.makeVectorType(type_uint, 4), false, true,
            false, false, false, source_texture_parameters,
            spv::ImageOperandsMaskNone),
        type_uint, 0);
    spv::Id source_depth32 =
        builder.createCompositeExtract(source_vec4, type_float, 0);
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8: {
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(
                    spv::OpFMul, type_float, source_depth32,
                    builder.makeFloatConstant(float(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32,
            !options.depth_float24_convert_in_pixel_shader &&
                options.depth_float24_round,
            true, ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
        builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        // k_8_8_8_8_GAMMA is stored as linear in the unorm16 host render
        // target, so encode RGB linear -> gamma before packing (alpha stays
        // linear). Reaching the gamma resource format implies
        // gamma_render_target_as_unorm16.
        bool is_gamma = key.GetColorFormat() ==
                        xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
        spv::Id source_red =
            builder.createCompositeExtract(source_vec4, type_float, 0);
        if (is_gamma) {
          source_red = SpirvShaderTranslator::LinearToPWLGamma(
              &builder, source_red, true, ext_inst_glsl_std_450);
        }
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float,
                                                    source_red, unorm_scale),
                                unorm_round_offset));
        spv::Id component_width = builder.makeUintConstant(8);
        for (uint32_t i = 1; i < 4; ++i) {
          spv::Id source_component =
              builder.createCompositeExtract(source_vec4, type_float, i);
          if (is_gamma && i < 3) {
            source_component = SpirvShaderTranslator::LinearToPWLGamma(
                &builder, source_component, true, ext_inst_glsl_std_450);
          }
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(spv::OpFMul, type_float,
                                          source_component, unorm_scale),
                      unorm_round_offset)),
              builder.makeUintConstant(8 * i), component_width);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createCompositeExtract(source_vec4, type_float, 0),
                    unorm_scale_rgb),
                unorm_round_offset));
        spv::Id width_rgb = builder.makeUintConstant(10);
        spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
        spv::Id width_a = builder.makeUintConstant(2);
        for (uint32_t i = 1; i < 4; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(
                          spv::OpFMul, type_float,
                          builder.createCompositeExtract(source_vec4,
                                                         type_float, i),
                          i == 3 ? unorm_scale_a : unorm_scale_rgb),
                      unorm_round_offset)),
              builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        // Float16 has a wider range for both color and alpha, also NaNs - clamp
        // and convert.
        packed[0] = SpirvShaderTranslator::UnclampedFloat32To7e3(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450);
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (uint32_t i = 1; i < 3; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              SpirvShaderTranslator::UnclampedFloat32To7e3(
                  builder,
                  builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        // Saturate and convert the alpha.
        spv::Id alpha_saturated = builder.createTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, packed[0],
            builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        alpha_saturated,
                                        builder.makeFloatConstant(3.0f)),
                    builder.makeFloatConstant(0.5f))),
            builder.makeUintConstant(30), builder.makeUintConstant(2));
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaN encodings and -32768 / -32767.
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        spv::Id component_offset_width = builder.makeUintConstant(16);
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          packed[i] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createCompositeExtract(source_vec4, type_uint, 2 * i),
              builder.createCompositeExtract(source_vec4, type_uint, 2 * i + 1),
              component_offset_width, component_offset_width);
        }
      } break;
      // Float32 is transferred as uint32 to preserve NaN encodings. However,
      // multisampled sampled image support is optional in Vulkan.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4,
                                                      source_component_type, i);
          if (!source_is_uint) {
            packed_ref =
                builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  // Write the packed value to the EDRAM buffer.
  spv::Id store_value = packed[0];
  if (format_is_64bpp) {
    id_vector_temp.clear();
    id_vector_temp.push_back(packed[0]);
    id_vector_temp.push_back(packed[1]);
    store_value = builder.createCompositeConstruct(type_uint2, id_vector_temp);
  }
  id_vector_temp.clear();
  // The only SSBO structure member.
  id_vector_temp.push_back(builder.makeIntConstant(0));
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  builder.createStore(store_value,
                      builder.createAccessChain(spv::StorageClassUniform,
                                                edram_buffer, id_vector_temp));

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize,
                           kEdramDumpShaderSamplesPerGroupX,
                           kEdramDumpShaderSamplesPerGroupY, 1);
  spv::Instruction* entry_point = builder.addEntryPoint(
      spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting with
  // SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);
  return std::vector<uint32_t>(shader_code.begin(), shader_code.end());
}

}  // namespace gpu
}  // namespace xe
