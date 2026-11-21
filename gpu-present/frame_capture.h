/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_PS4_FRAME_CAPTURE_H
#define POKOPIA_PS4_FRAME_CAPTURE_H

#include <stdbool.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkDevice device;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    VkExtent2D extent;
    bool bgra, coherent, recorded;
} PokopiaFrameCapture;

/* Start with a zeroed object. The format and extent must match the swapchain.
 * The caller serializes capture with the presenter on its render thread. */
VkResult pokopia_frame_capture_open(PokopiaFrameCapture *capture, VkPhysicalDevice physical,
                                    VkDevice device, VkFormat format, VkExtent2D extent);

/* Call outside a render pass, after drawing and before the usual PRESENT
 * barrier. The current swapchain image needs TRANSFER_SRC usage. This records
 * the copy and transitions the image to PRESENT_SRC_KHR. The stage and access
 * flags describe preceding writes to that image. */
VkResult pokopia_frame_capture_record(PokopiaFrameCapture *capture, VkCommandBuffer command,
                                      VkImage image, VkImageLayout layout,
                                      VkPipelineStageFlags stages, VkAccessFlags access);

/* Pass the fence for the submission containing the recorded copy, after the
 * frame wait and before resetting the fence. VK_NOT_READY permits a retry.
 * Publishes an opaque RGB PNG using an atomic rename. File errors return
 * VK_ERROR_UNKNOWN with errno set. */
VkResult pokopia_frame_capture_save(PokopiaFrameCapture *capture, VkFence fence, const char *path);

/* Retire any submitted copy and discard any unsubmitted recording first. */
void pokopia_frame_capture_close(PokopiaFrameCapture *capture);

#ifdef __cplusplus
}
#endif
#endif
