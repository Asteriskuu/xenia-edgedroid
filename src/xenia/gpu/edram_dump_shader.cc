/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/edram_dump_shader.h"

#include <initializer_list>
#include <optional>
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

namespace {

// Positions of the fields the direct resolve store reads out of the packed
// draw_util::Resolve* structures, the same ones resolve.xesli unpacks, derived
// from the widths their bitfields are declared with.
constexpr uint32_t kResolveEdramInfoBaseTilesShift =
    xenos::kEdramPitchTilesBits + xenos::kMsaaSamplesBits + 1;
constexpr uint32_t kResolveEdramInfoFormatShift =
    kResolveEdramInfoBaseTilesShift + xenos::kEdramBaseTilesBits;

constexpr uint32_t kResolveCoordinateInfoOffsetXShift = 0;
constexpr uint32_t kResolveCoordinateInfoOffsetYShift = 4;
constexpr uint32_t kResolveCoordinateInfoWidthShift = 5;
constexpr uint32_t kResolveCoordinateInfoWidthBits =
    xenos::kResolveSizeBits - xenos::kResolveAlignmentPixelsLog2;

constexpr uint32_t kResolveDestInfoEndianBits = 3;
constexpr uint32_t kResolveDestInfoIsArrayShift = 3;
constexpr uint32_t kResolveDestInfoSliceShift = 4;
constexpr uint32_t kResolveDestInfoSwapShift = 24;

constexpr uint32_t kResolveDestCoordinateInfoPitchBits =
    xenos::kTexture2DCubeMaxWidthHeightLog2 + 2 -
    xenos::kTextureTileWidthHeightLog2;
constexpr uint32_t kResolveDestCoordinateInfoHeightShift =
    kResolveDestCoordinateInfoPitchBits;
constexpr uint32_t kResolveDestCoordinateInfoOffsetXShift =
    2 * kResolveDestCoordinateInfoPitchBits;
constexpr uint32_t kResolveDestCoordinateInfoOffsetYShift =
    kResolveDestCoordinateInfoOffsetXShift + 7 -
    xenos::kResolveAlignmentPixelsLog2;
constexpr uint32_t kResolveDestCoordinateInfoSampleSelectShift =
    kResolveDestCoordinateInfoOffsetYShift + 7 -
    xenos::kResolveAlignmentPixelsLog2;

// XENOS_TEXTURE_MACRO_TILE_HEIGHT_3D_LOG2 in texture_address.xesli.
constexpr uint32_t kTextureMacroTileHeight3DLog2 = 4;

}  // namespace

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
  spv::Id type_bool = builder.makeBoolType();

  // A direct resolve leaves EDRAM alone, so it can't be the scaled layout or a
  // native source dumped into one.
  assert_false(key.direct_resolve && draw_resolution_scaled);
  assert_false(key.direct_resolve && key.native_layout);

  // Bindings.
  // Destination buffer - EDRAM in whole samples, or the resolve destination
  // addressed in dwords because the guest texture layout isn't sample-aligned.
  bool format_is_64bpp = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(
                                              key.GetColorFormat());
  bool dest_is_dwords = key.direct_resolve;
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeRuntimeArray(
      (format_is_64bpp && !dest_is_dwords) ? type_uint2 : type_uint));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        sizeof(uint32_t)
                            << uint32_t(format_is_64bpp && !dest_is_dwords));
  spv::Id type_dest = builder.makeStructType(
      id_vector_temp, key.direct_resolve ? "XeResolveDest" : "XeEdram");
  builder.addMemberName(type_dest, 0,
                        key.direct_resolve ? "resolve_dest" : "edram");
  builder.addMemberDecoration(type_dest, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_dest, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_dest, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id dest_buffer = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniform, type_dest,
      key.direct_resolve ? "xe_resolve_dest" : "xe_edram");
  builder.addDecoration(dest_buffer, spv::DecorationDescriptorSet,
                        options.descriptor_set_dest);
  builder.addDecoration(dest_buffer, spv::DecorationBinding, 0);
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
  // Push constants. The direct resolve ones are declared but unreferenced in
  // the dump shaders, which costs nothing but keeps one layout for both.
  static const char* const kPushConstantNames[] = {
      "pitches",
      "offsets",
      "resolve_edram_info",
      "resolve_coordinate_info",
      "resolve_dest_info",
      "resolve_dest_coordinate_info",
      "resolve_dest_base",
      "resolve_height_div_8",
  };
  static_assert(
      xe::countof(kPushConstantNames) == kEdramDumpShaderPushConstantCount,
      "Every push constant needs a name in the shader");
  id_vector_temp.clear();
  id_vector_temp.reserve(kEdramDumpShaderPushConstantCount);
  for (uint32_t i = 0; i < kEdramDumpShaderPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants =
      builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  for (uint32_t i = 0; i < kEdramDumpShaderPushConstantCount; ++i) {
    builder.addMemberName(type_push_constants, i, kPushConstantNames[i]);
    builder.addMemberDecoration(type_push_constants, i, spv::DecorationOffset,
                                int(sizeof(uint32_t) * i));
  }
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

  auto load_push_constant = [&](EdramDumpShaderPushConstant constant) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeIntConstant(int(constant)));
    return builder.createLoad(
        builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                  id_vector_temp),
        spv::NoPrecision);
  };
  auto extract = [&](spv::Id source, uint32_t offset, uint32_t count) {
    return builder.createTriOp(spv::OpBitFieldUExtract, type_uint, source,
                               builder.makeUintConstant(offset),
                               builder.makeUintConstant(count));
  };
  auto shift_left = [&](spv::Id value, uint32_t amount) {
    return builder.createBinOp(spv::OpShiftLeftLogical, type_uint, value,
                               builder.makeUintConstant(amount));
  };
  auto shift_right = [&](spv::Id value, uint32_t amount) {
    return builder.createBinOp(spv::OpShiftRightLogical, type_uint, value,
                               builder.makeUintConstant(amount));
  };
  auto bitwise_and = [&](spv::Id value, uint32_t mask) {
    return builder.createBinOp(spv::OpBitwiseAnd, type_uint, value,
                               builder.makeUintConstant(mask));
  };
  auto bitwise_or = [&](spv::Id a, spv::Id b) {
    return builder.createBinOp(spv::OpBitwiseOr, type_uint, a, b);
  };
  auto add = [&](spv::Id a, spv::Id b) {
    return builder.createBinOp(spv::OpIAdd, type_uint, a, b);
  };
  auto multiply = [&](spv::Id a, spv::Id b) {
    return builder.createBinOp(spv::OpIMul, type_uint, a, b);
  };
  auto equals = [&](spv::Id value, uint32_t constant) {
    return builder.createBinOp(spv::OpIEqual, type_bool, value,
                               builder.makeUintConstant(constant));
  };

  // Where a direct resolve stores, and whether this invocation's sample is one
  // the resolve reads at all. Both are resolved before the source is loaded so
  // the samples MSAA discards never cost a fetch.
  spv::Id direct_resolve_dest_dword_index = spv::NoResult;
  spv::Id direct_resolve_write_condition = spv::NoResult;
  spv::Id resolve_edram_info = spv::NoResult;
  spv::Id resolve_dest_info = spv::NoResult;

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
  spv::Id pitches_constant =
      load_push_constant(kEdramDumpShaderPushConstantPitches);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits =
      builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id dest_pitch_tiles =
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, pitches_constant,
                          const_uint_0, const_edram_pitch_tiles_bits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, dest_pitch_tiles,
                          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  spv::Id offsets_constant =
      load_push_constant(kEdramDumpShaderPushConstantOffsets);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_uint_0, const_edram_base_tiles_bits_plus_1),
      rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index. A direct resolve never touches
  // EDRAM - and the depth column swap below would only be undone again by the
  // resolve copy's own addressing, so it drops out entirely.
  spv::Id edram_sample_address = spv::NoResult;
  if (!key.direct_resolve) {
    edram_sample_address = builder.createBinOp(
        spv::OpIAdd, type_uint,
        builder.createBinOp(
            spv::OpIMul, type_uint,
            builder.makeUintConstant(tile_width * tile_height),
            builder.createBinOp(
                spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
                builder.makeUintConstant(xenos::kEdramTileCount - 1))),
        builder.createBinOp(
            spv::OpIAdd, type_uint,
            builder.createBinOp(spv::OpIMul, type_uint, const_tile_width,
                                tile_sample_y),
            tile_sample_x));
    if (key.is_depth) {
      // Swap 40-sample columns in the depth buffer in the destination address
      // to get the final address of the sample in the EDRAM.
      uint32_t tile_width_half = tile_width >> 1;
      edram_sample_address = builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int,
                                    edram_sample_address),
              builder.createTriOp(
                  spv::OpSelect, type_int,
                  builder.createBinOp(
                      spv::OpULessThan, builder.makeBoolType(), tile_sample_x,
                      builder.makeUintConstant(tile_width_half)),
                  builder.makeIntConstant(int32_t(tile_width_half)),
                  builder.makeIntConstant(-int32_t(tile_width_half)))));
    }
  }

  if (key.direct_resolve) {
    // Locate the sample in the resolve destination, in the guest texture
    // layout, mirroring resolve.xesli and texture_address.xesli so the result
    // is bit-identical to what the round trip would have written.
    resolve_edram_info =
        load_push_constant(kEdramDumpShaderPushConstantResolveEdramInfo);
    spv::Id resolve_coordinate_info =
        load_push_constant(kEdramDumpShaderPushConstantResolveCoordinateInfo);
    resolve_dest_info =
        load_push_constant(kEdramDumpShaderPushConstantResolveDestInfo);
    spv::Id resolve_dest_coordinate_info = load_push_constant(
        kEdramDumpShaderPushConstantResolveDestCoordinateInfo);

    // The sample's position within the resolve's own tile grid. The dispatch
    // rectangle is that grid - GetResolveEdramTileSpan derives its base and
    // pitch from the same ResolveEdramInfo - so dest_pitch_tiles is already
    // the resolve's pitch, and the tile index only needs rebasing.
    spv::Id resolve_tile_index = builder.createBinOp(
        spv::OpISub, type_uint, edram_tile_index_non_wrapped,
        extract(resolve_edram_info, kResolveEdramInfoBaseTilesShift,
                xenos::kEdramBaseTilesBits));
    spv::Id resolve_sample_x =
        add(multiply(const_tile_width,
                     builder.createBinOp(spv::OpUMod, type_uint,
                                         resolve_tile_index, dest_pitch_tiles)),
            tile_sample_x);
    spv::Id resolve_sample_y =
        add(multiply(const_tile_height,
                     builder.createBinOp(spv::OpUDiv, type_uint,
                                         resolve_tile_index, dest_pitch_tiles)),
            tile_sample_y);

    // Undo XeEdramOffsetBytes' sample placement to get the guest pixel: it
    // scales the pixel by the MSAA dimensions, then adds bit 1 of the sample
    // index to X and bit 0 to Y. Subtracting those offsets before the shift
    // also reproduces 1x MSAA, where a nonzero index addresses a neighbouring
    // pixel rather than selecting a sample. Eligibility guarantees a single
    // selected sample, so XeResolveFirstSampleIndex is the raw field.
    spv::Id sample_select =
        extract(resolve_dest_coordinate_info,
                kResolveDestCoordinateInfoSampleSelectShift, 3);
    spv::Id pixel_x =
        builder.createBinOp(spv::OpISub, type_uint, resolve_sample_x,
                            bitwise_and(shift_right(sample_select, 1), 1));
    spv::Id pixel_y =
        builder.createBinOp(spv::OpISub, type_uint, resolve_sample_y,
                            bitwise_and(sample_select, 1));
    auto require = [&](spv::Id condition) {
      direct_resolve_write_condition =
          direct_resolve_write_condition == spv::NoResult
              ? condition
              : builder.createBinOp(spv::OpLogicalAnd, type_bool,
                                    direct_resolve_write_condition, condition);
    };
    // Only the selected sample of a pixel reaches the destination. Along an
    // axis with one sample per pixel there's nothing to select and no mask.
    uint32_t msaa_x_log2 =
        uint32_t(key.msaa_samples >= xenos::MsaaSamples::k4X);
    uint32_t msaa_y_log2 =
        uint32_t(key.msaa_samples >= xenos::MsaaSamples::k2X);
    if (msaa_x_log2) {
      require(
          equals(bitwise_and(pixel_x, (uint32_t(1) << msaa_x_log2) - 1), 0));
      pixel_x = shift_right(pixel_x, msaa_x_log2);
    }
    if (msaa_y_log2) {
      require(
          equals(bitwise_and(pixel_y, (uint32_t(1) << msaa_y_log2) - 1), 0));
      pixel_y = shift_right(pixel_y, msaa_y_log2);
    }

    // Clip to the resolve rectangle - the dispatch covers the tiles containing
    // it, which round outward on both axes. Pixels before the rectangle's
    // origin wrap around to huge values and fail the same comparison.
    spv::Id dest_x = builder.createBinOp(
        spv::OpISub, type_uint, pixel_x,
        shift_left(extract(resolve_coordinate_info,
                           kResolveCoordinateInfoOffsetXShift, 4),
                   xenos::kResolveAlignmentPixelsLog2));
    spv::Id dest_y = builder.createBinOp(
        spv::OpISub, type_uint, pixel_y,
        shift_left(extract(resolve_coordinate_info,
                           kResolveCoordinateInfoOffsetYShift, 1),
                   xenos::kResolveAlignmentPixelsLog2));
    require(builder.createBinOp(
        spv::OpULessThan, type_bool, dest_x,
        shift_left(
            extract(resolve_coordinate_info, kResolveCoordinateInfoWidthShift,
                    kResolveCoordinateInfoWidthBits),
            xenos::kResolveAlignmentPixelsLog2)));
    require(builder.createBinOp(
        spv::OpULessThan, type_bool, dest_y,
        shift_left(
            load_push_constant(kEdramDumpShaderPushConstantResolveHeightDiv8),
            xenos::kResolveAlignmentPixelsLog2)));

    // XeResolveDestPixelAddress. The guest and host positions are the same
    // without resolution scaling.
    dest_x = add(dest_x,
                 shift_left(extract(resolve_dest_coordinate_info,
                                    kResolveDestCoordinateInfoOffsetXShift, 4),
                            xenos::kResolveAlignmentPixelsLog2));
    dest_y = add(dest_y,
                 shift_left(extract(resolve_dest_coordinate_info,
                                    kResolveDestCoordinateInfoOffsetYShift, 4),
                            xenos::kResolveAlignmentPixelsLog2));
    uint32_t bytes_per_block_log2 = 2 + uint32_t(format_is_64bpp);
    spv::Id dest_pitch_macro_tiles = extract(
        resolve_dest_coordinate_info, 0, kResolveDestCoordinateInfoPitchBits);
    // XenosTextureTiledAddressCombine.
    auto combine = [&](spv::Id outer_inner_bytes, spv::Id bank, spv::Id pipe,
                       spv::Id y_lsb) {
      spv::Id address =
          bitwise_or(bitwise_or(shift_left(y_lsb, 4), shift_left(pipe, 6)),
                     shift_left(bank, 11));
      address = bitwise_or(address, bitwise_and(outer_inner_bytes, 0xF));
      address = bitwise_or(
          address,
          shift_left(bitwise_and(shift_right(outer_inner_bytes, 4), 0x1), 5));
      address = bitwise_or(
          address,
          shift_left(bitwise_and(shift_right(outer_inner_bytes, 5), 0x7), 8));
      return bitwise_or(address,
                        shift_left(shift_right(outer_inner_bytes, 8), 12));
    };
    // XenosTextureTiledAddress2D.
    spv::Id outer_blocks_2d =
        shift_left(add(multiply(shift_right(dest_y, 5), dest_pitch_macro_tiles),
                       shift_right(dest_x, 5)),
                   6);
    spv::Id inner_blocks_2d =
        bitwise_or(shift_left(bitwise_and(shift_right(dest_y, 1), 0x7), 3),
                   bitwise_and(dest_x, 0x7));
    spv::Id outer_inner_bytes_2d = shift_left(
        bitwise_or(outer_blocks_2d, inner_blocks_2d), bytes_per_block_log2);
    spv::Id bank_2d = bitwise_and(shift_right(dest_y, 4), 0x1);
    spv::Id pipe_2d = builder.createBinOp(
        spv::OpBitwiseXor, type_uint, bitwise_and(shift_right(dest_x, 3), 0x3),
        shift_left(bitwise_and(shift_right(dest_y, 3), 0x1), 1));
    spv::Id address_2d =
        combine(outer_inner_bytes_2d, bank_2d, pipe_2d, bitwise_and(dest_y, 1));
    // XenosTextureTiledAddress3D, for array destinations.
    spv::Id dest_slice =
        extract(resolve_dest_info, kResolveDestInfoSliceShift, 3);
    spv::Id dest_slice_pitch_macro_tiles = shift_left(
        extract(resolve_dest_coordinate_info,
                kResolveDestCoordinateInfoHeightShift,
                kResolveDestCoordinateInfoPitchBits),
        xenos::kTextureTileWidthHeightLog2 - kTextureMacroTileHeight3DLog2);
    spv::Id outer_blocks_3d = shift_left(
        add(multiply(add(multiply(shift_right(dest_slice, 2),
                                  dest_slice_pitch_macro_tiles),
                         shift_right(dest_y, kTextureMacroTileHeight3DLog2)),
                     dest_pitch_macro_tiles),
            shift_right(dest_x, 5)),
        7);
    spv::Id inner_blocks_3d = bitwise_or(
        bitwise_or(shift_left(bitwise_and(dest_slice, 0x3), 5),
                   shift_left(bitwise_and(shift_right(dest_y, 1), 0x3), 3)),
        bitwise_and(dest_x, 0x7));
    spv::Id outer_inner_bytes_3d = shift_left(
        bitwise_or(outer_blocks_3d, inner_blocks_3d), bytes_per_block_log2);
    spv::Id bank_3d = bitwise_and(
        builder.createBinOp(spv::OpBitwiseXor, type_uint,
                            shift_right(dest_y, 3), shift_right(dest_slice, 2)),
        0x1);
    spv::Id pipe_3d = builder.createBinOp(
        spv::OpBitwiseXor, type_uint, bitwise_and(shift_right(dest_x, 3), 0x3),
        shift_left(bank_3d, 1));
    spv::Id address_3d =
        combine(outer_inner_bytes_3d, bank_3d, pipe_3d, bitwise_and(dest_y, 1));
    spv::Id dest_is_array = builder.createBinOp(
        spv::OpINotEqual, type_bool,
        bitwise_and(resolve_dest_info, uint32_t(1)
                                           << kResolveDestInfoIsArrayShift),
        const_uint_0);
    spv::Id dest_address =
        add(builder.createTriOp(spv::OpSelect, type_uint, dest_is_array,
                                address_3d, address_2d),
            load_push_constant(kEdramDumpShaderPushConstantResolveDestBase));
    // The destination is addressed in dwords - the guest texture layout isn't
    // sample-aligned for 64bpp.
    direct_resolve_dest_dword_index = shift_right(dest_address, 2);
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

  // Everything from here on is per selected sample, so a direct resolve skips
  // it for the samples it discards rather than fetching them and throwing the
  // result away.
  std::optional<SpirvBuilder::IfBuilder> if_direct_resolve_writes;
  if (key.direct_resolve) {
    if_direct_resolve_writes.emplace(direct_resolve_write_condition,
                                     spv::SelectionControlMaskNone, builder);
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

  if (!key.direct_resolve) {
    // Write the packed value to the EDRAM buffer.
    spv::Id store_value = packed[0];
    if (format_is_64bpp) {
      id_vector_temp.clear();
      id_vector_temp.push_back(packed[0]);
      id_vector_temp.push_back(packed[1]);
      store_value =
          builder.createCompositeConstruct(type_uint2, id_vector_temp);
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
                                                  dest_buffer, id_vector_temp));
  } else {
    // Everything the resolve copy would have done to these bits after reading
    // them back out of EDRAM.

    // Red/blue swap, keyed on the resolve's format rather than the render
    // target's - the round trip packs in the render target's layout and swaps
    // in the resolve's, and the two differ when a surface is being aliased.
    spv::Id dest_swap = builder.createBinOp(
        spv::OpINotEqual, type_bool,
        bitwise_and(resolve_dest_info, uint32_t(1)
                                           << kResolveDestInfoSwapShift),
        const_uint_0);
    spv::Id edram_format =
        extract(resolve_edram_info, kResolveEdramInfoFormatShift,
                xenos::kRenderTargetFormatBits);
    auto format_is =
        [&](std::initializer_list<xenos::ColorRenderTargetFormat> formats) {
          spv::Id result = spv::NoResult;
          for (xenos::ColorRenderTargetFormat format : formats) {
            spv::Id is_format = equals(edram_format, uint32_t(format));
            result = result == spv::NoResult
                         ? is_format
                         : builder.createBinOp(spv::OpLogicalOr, type_bool,
                                               result, is_format);
          }
          return result;
        };
    if (!format_is_64bpp) {
      // XeResolveSwapRedBlue_8_8_8_8 / XeResolveSwapRedBlue_2_10_10_10.
      spv::Id swapped_8888 =
          bitwise_or(bitwise_or(bitwise_and(packed[0], ~uint32_t(0xFF00FF)),
                                shift_left(bitwise_and(packed[0], 0xFF), 16)),
                     bitwise_and(shift_right(packed[0], 16), 0xFF));
      spv::Id swapped_2101010 =
          bitwise_or(bitwise_or(bitwise_and(packed[0], ~uint32_t(0x3FF003FF)),
                                shift_left(bitwise_and(packed[0], 0x3FF), 20)),
                     bitwise_and(shift_right(packed[0], 20), 0x3FF));
      spv::Id swapped = builder.createTriOp(
          spv::OpSelect, type_uint,
          format_is({xenos::ColorRenderTargetFormat::k_8_8_8_8,
                     xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA}),
          swapped_8888,
          builder.createTriOp(
              spv::OpSelect, type_uint,
              format_is(
                  {xenos::ColorRenderTargetFormat::k_2_10_10_10,
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT,
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10,
                   xenos::ColorRenderTargetFormat::
                       k_2_10_10_10_FLOAT_AS_16_16_16_16}),
              swapped_2101010, packed[0]));
      packed[0] = builder.createTriOp(spv::OpSelect, type_uint, dest_swap,
                                      swapped, packed[0]);
    } else {
      // XeResolveSwap4PixelsRedBlue64bpp - the low halves of the two dwords.
      spv::Id swap_64bpp = builder.createBinOp(
          spv::OpLogicalAnd, type_bool, dest_swap,
          format_is({xenos::ColorRenderTargetFormat::k_16_16_16_16,
                     xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT}));
      spv::Id swapped_0 = bitwise_or(bitwise_and(packed[0], ~uint32_t(0xFFFF)),
                                     bitwise_and(packed[1], 0xFFFF));
      spv::Id swapped_1 = bitwise_or(bitwise_and(packed[1], ~uint32_t(0xFFFF)),
                                     bitwise_and(packed[0], 0xFFFF));
      packed[0] = builder.createTriOp(spv::OpSelect, type_uint, swap_64bpp,
                                      swapped_0, packed[0]);
      packed[1] = builder.createTriOp(spv::OpSelect, type_uint, swap_64bpp,
                                      swapped_1, packed[1]);
    }

    // Endian swap. The fast copy shaders only ever call XeEndianSwap32 and
    // XeEndianSwap64, neither of which handles 8in128, so no invocation ever
    // needs a neighbour's dwords.
    spv::Id endian = extract(resolve_dest_info, 0, kResolveDestInfoEndianBits);
    if (format_is_64bpp) {
      // XeEndianSwap64 exchanges the dwords for 8in64, then swaps as 8in32.
      spv::Id is_8in64 = equals(endian, uint32_t(xenos::Endian128::k8in64));
      spv::Id packed_0 = builder.createTriOp(spv::OpSelect, type_uint, is_8in64,
                                             packed[1], packed[0]);
      spv::Id packed_1 = builder.createTriOp(spv::OpSelect, type_uint, is_8in64,
                                             packed[0], packed[1]);
      packed[0] = packed_0;
      packed[1] = packed_1;
      endian = builder.createTriOp(
          spv::OpSelect, type_uint, is_8in64,
          builder.makeUintConstant(uint32_t(xenos::Endian128::k8in32)), endian);
    }
    // XeEndianSwap32.
    spv::Id endian_is_8in32 =
        equals(endian, uint32_t(xenos::Endian128::k8in32));
    spv::Id swap_bytes_in_16 = builder.createBinOp(
        spv::OpLogicalOr, type_bool,
        equals(endian, uint32_t(xenos::Endian128::k8in16)), endian_is_8in32);
    spv::Id swap_halves_in_32 = builder.createBinOp(
        spv::OpLogicalOr, type_bool, endian_is_8in32,
        equals(endian, uint32_t(xenos::Endian128::k16in32)));
    for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
      spv::Id value = packed[i];
      value = builder.createTriOp(
          spv::OpSelect, type_uint, swap_bytes_in_16,
          bitwise_or(shift_left(bitwise_and(value, 0x00FF00FF), 8),
                     shift_right(bitwise_and(value, 0xFF00FF00), 8)),
          value);
      value = builder.createTriOp(
          spv::OpSelect, type_uint, swap_halves_in_32,
          bitwise_or(shift_left(value, 16), shift_right(value, 16)), value);
      packed[i] = value;
    }

    for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
      id_vector_temp.clear();
      // The only SSBO structure member.
      id_vector_temp.push_back(builder.makeIntConstant(0));
      id_vector_temp.push_back(builder.createUnaryOp(
          spv::OpBitcast, type_int,
          i ? add(direct_resolve_dest_dword_index, builder.makeUintConstant(1))
            : direct_resolve_dest_dword_index));
      builder.createStore(
          packed[i], builder.createAccessChain(spv::StorageClassUniform,
                                               dest_buffer, id_vector_temp));
    }
  }

  if (if_direct_resolve_writes.has_value()) {
    if_direct_resolve_writes->makeEndIf();
  }

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
