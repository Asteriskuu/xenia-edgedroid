/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_EDRAM_DUMP_SHADER_H_
#define XENIA_GPU_EDRAM_DUMP_SHADER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {

// Emits the compute shader that copies a host render target's samples into the
// EDRAM buffer in guest tile layout, the first half of a resolve on the host
// render target path. Backend-independent: the result is SPIR-V, which Vulkan
// consumes directly and Metal takes through spirv_to_dxil and Apple's Metal
// Shader Converter.

union EdramDumpShaderKey {
  uint32_t key;
  struct {
    xenos::MsaaSamples msaa_samples : 2;
    uint32_t resource_format : 4;
    // Last bit because this affects the pipeline - after sorting, only change
    // it at most once. Depth buffers have an additional stencil SRV.
    uint32_t is_depth : 1;
    // Dumping to the scaled EDRAM layout duplicates this native render
    // target's guest pixels.
    uint32_t source_scale_native : 1;
    // source_scale_native only.
    // Address the EDRAM buffer with the plain 1x1 tile layout.
    uint32_t native_layout : 1;
  };

  EdramDumpShaderKey() : key(0) { static_assert_size(*this, sizeof(key)); }

  struct Hasher {
    size_t operator()(const EdramDumpShaderKey& key) const {
      return std::hash<uint32_t>{}(key.key);
    }
  };
  bool operator==(const EdramDumpShaderKey& other_key) const {
    return key == other_key.key;
  }
  bool operator!=(const EdramDumpShaderKey& other_key) const {
    return !(*this == other_key);
  }
  bool operator<(const EdramDumpShaderKey& other_key) const {
    return key < other_key.key;
  }

  xenos::ColorRenderTargetFormat GetColorFormat() const {
    assert_false(is_depth);
    return xenos::ColorRenderTargetFormat(resource_format);
  }
  xenos::DepthRenderTargetFormat GetDepthFormat() const {
    assert_true(is_depth);
    return xenos::DepthRenderTargetFormat(resource_format);
  }
};

// There's no strict dependency on the group size in dumping, for simplicity of
// calculations especially with resolution scaling, dividing manually (as the
// group size is not unlimited). The only restriction is that an integer
// multiple of it must be 80x16 samples (and no larger than that) for 32bpp, or
// 40x16 samples for 64bpp (because only a half of the pair of tiles may need to
// be dumped). Using 8x16 since that's 128 - the minimum required group size on
// Vulkan, and the maximum number of lanes in a subgroup on Vulkan.
constexpr uint32_t kEdramDumpShaderSamplesPerGroupX = 8;
constexpr uint32_t kEdramDumpShaderSamplesPerGroupY = 16;

// Push constants, in the order the shader declares them.
enum EdramDumpShaderPushConstant : uint32_t {
  // May be different for different sources.
  kEdramDumpShaderPushConstantPitches,
  // May be changed multiple times for the same source.
  kEdramDumpShaderPushConstantOffsets,

  kEdramDumpShaderPushConstantCount,
};

union EdramDumpShaderPitches {
  uint32_t pitches;
  struct {
    // Both in tiles.
    uint32_t dest_pitch : xenos::kEdramPitchTilesBits;
    uint32_t source_pitch : xenos::kEdramPitchTilesBits;
  };
  EdramDumpShaderPitches() : pitches(0) {
    static_assert_size(*this, sizeof(pitches));
  }
  bool operator==(const EdramDumpShaderPitches& other_pitches) const {
    return pitches == other_pitches.pitches;
  }
  bool operator!=(const EdramDumpShaderPitches& other_pitches) const {
    return !(*this == other_pitches);
  }
};

union EdramDumpShaderOffsets {
  uint32_t offsets;
  struct {
    // May be beyond the EDRAM tile count in case of EDRAM addressing wrapping,
    // thus + 1 bit.
    uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 1;
    uint32_t source_base_tiles : xenos::kEdramBaseTilesBits;
  };
  EdramDumpShaderOffsets() : offsets(0) {
    static_assert_size(*this, sizeof(offsets));
  }
  bool operator==(const EdramDumpShaderOffsets& other_offsets) const {
    return offsets == other_offsets.offsets;
  }
  bool operator!=(const EdramDumpShaderOffsets& other_offsets) const {
    return !(*this == other_offsets);
  }
};

// What the emitter can't derive from the key: the binding model to declare the
// resources in, and the host properties the guest layout is resolved against.
struct EdramDumpShaderOptions {
  // SPIR-V version to emit, as glslang's SpirvBuilder takes it.
  uint32_t spirv_version = 0x00010000;

  // Descriptor sets of the destination EDRAM buffer and of the source render
  // target. The source's stencil, when the key is a depth one, is binding 1 of
  // the source set; everything else is binding 0 of its set.
  uint32_t descriptor_set_edram = 0;
  uint32_t descriptor_set_source = 1;

  // Guest-to-host resolution scale, which the tile addressing is computed
  // against.
  uint32_t resolution_scale_x = 1;
  uint32_t resolution_scale_y = 1;

  // Whether the host can give 2x MSAA render targets real 2-sample attachments
  // rather than emulating them on 4-sample ones, which decides where guest 2x
  // samples live.
  bool msaa_2x_attachments_supported = false;

  // Whether the backend stores this color format's ownership-transfer view as
  // an integer texture, so the source is sampled as uint rather than float.
  // Ignored for depth keys. The backends own their format policy, so this
  // comes in rather than being derived here.
  bool source_is_uint = false;

  // float24 depth handling, from the cvars of the same name as the backend
  // resolved them: whether guest depth was already rounded to float24 by the
  // pixel shader, and whether rounding is to nearest even rather than toward
  // zero.
  bool depth_float24_round = false;
  bool depth_float24_convert_in_pixel_shader = false;
};

// Returns the SPIR-V words of the dump shader for one key, or an empty vector
// if the key's format is not dumpable.
std::vector<uint32_t> BuildEdramDumpShaderSpirv(
    EdramDumpShaderKey key, const EdramDumpShaderOptions& options);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_EDRAM_DUMP_SHADER_H_
