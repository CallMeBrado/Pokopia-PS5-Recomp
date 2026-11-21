/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_LIVE_PRESENT_H
#define POKOPIA_LIVE_PRESENT_H

#include "game_capture.h"

typedef struct {
    VkImage image;
    VkFormat format;
    VkExtent2D extent;
    VkImageLayout layout;
    VkPipelineStageFlags stages;
    VkAccessFlags access;
    VkRect2D crop;
    bool flip_y;
    uint64_t render_serial, completed_render_serial;
} PokopiaLiveFrame;

typedef struct {
    uint64_t presented_render_serial;

    VkPhysicalDevice physical;
    PokopiaPresent *present;
    PokopiaGameCapture *capture;
    VkCommandBuffer command;
    VkImage conversion;
    VkDeviceMemory conversion_memory;
    VkExtent2D conversion_extent;
    VkFormat conversion_format;
} PokopiaLivePresent;

/* Start with a zeroed object. Borrow an open native swapchain and a dedicated
 * resettable primary command buffer on its graphics queue. The swapchain needs
 * TRANSFER_DST usage and, with capture, TRANSFER_SRC usage. The caller retains
 * both objects and serializes their use with the live game's render queue. */
VkResult pokopia_live_present_init(PokopiaLivePresent *live, VkPhysicalDevice physical,
    PokopiaPresent *present, VkCommandBuffer command, PokopiaGameCapture *capture);

/* The caller supplies a completed render serial belonging to this image, an
 * exclusive single-sample 2D color image with TRANSFER_SRC usage. The stages
 * and access describe its current layout and last access. The layout is
 * restored before presentation completes. A zero crop selects the full image.
 * Capture errors are reported independently of successful WSI presentation. */
VkResult pokopia_live_present_frame(PokopiaLivePresent *live, const PokopiaLiveFrame *frame,
    uint64_t timeout_ns, VkResult *capture_result);

/* Retire any pending queue work before close, including when a submission
 * failed. The caller then closes the borrowed capture, command and swapchain. */
void pokopia_live_present_close(PokopiaLivePresent *live);

#endif
