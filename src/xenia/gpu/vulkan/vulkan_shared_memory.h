/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
#define XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, Memory& memory,
                     TraceWriter& trace_writer,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});

  VkBuffer buffer() const { return buffer_; }

  // True when the buffer aliases guest RAM directly
  // (VK_EXT_external_memory_host import). Uploads and memexport readback copies
  // are then unnecessary.
  bool is_zero_copy() const { return zero_copy_; }

  // Returns true if any downloads were submitted to the command processor.
  bool InitializeTraceSubmitDownloads();
  void InitializeTraceCompleteDownloads();

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_ranges) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  // Attempts to alias guest RAM as the buffer via VK_EXT_external_memory_host.
  // Returns true on success (buffer_ and buffer_memory_ populated, zero_copy_
  // set); false to fall back to the normal device-local path.
  bool TryInitializeZeroCopy();

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;
  // Buffer memory is imported guest RAM - no uploads or readback copies needed.
  bool zero_copy_ = false;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;

  // Created temporarily, only for downloading.
  VkBuffer trace_download_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory trace_download_buffer_memory_ = VK_NULL_HANDLE;
  void ResetTraceDownload();
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
