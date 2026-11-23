/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_PS4_PRESENT_H
#define POKOPIA_PS4_PRESENT_H

#include <stdbool.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;
    VkImageView view;
} PokopiaPresentTarget;

typedef struct {
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;

    /* Private swapchain ownership and frame state. */
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    PokopiaPresentTarget *targets;
    VkFence fence;
    uint32_t image_index;
    bool frame_open, fence_pending, faulted;
    /* R81: frames in flight. The acquire and the blit are no longer waited for
     * on the CPU (the replay worker waited out the GPU's whole frame at every
     * present): semaphores order acquire -> blit -> flip, and each of three
     * command buffers is reused after its own fence. `fence` is the open
     * frame's fence (game captures wait on it). */
    bool async;
    VkFence base_fence;
    VkCommandPool pool;
    VkCommandBuffer commands[3];
    VkFence ring_fences[3];
    VkSemaphore acquired[3], rendered[3];
    bool ring_used[3];
    uint32_t ring_at;
} PokopiaPresent;

/* Enable VK_KHR_surface and VK_EXT_headless_surface on the instance, and
 * VK_KHR_swapchain on the device. On Orbis this surface names the TV. */
/* R96: the display mode's refresh in millihertz (119880 at 120 Hz), 0 when unknown. */
extern uint32_t pokopia_present_refresh_mhz;
VkResult pokopia_present_surface(VkInstance instance, VkSurfaceKHR *surface);

/* Start with a zeroed object. The caller retains the device and surface and
 * serializes access to this object and its graphics/present queue. The driver
 * owns image storage, VideoOut registration, and the scanout row pitch. */
VkResult pokopia_present_open(PokopiaPresent *present, VkPhysicalDevice physical,
                              VkDevice device, uint32_t family, VkQueue queue,
                              VkSurfaceKHR surface, VkFormat format, VkExtent2D extent,
                              VkImageUsageFlags extra_usage);

/* A successful acquire (including SUBOPTIMAL) opens one frame. The target's
 * contents may be discarded. Record commands against it, ending in
 * PRESENT_SRC_KHR. If acquiring an image times out, it can be retried. */
VkResult pokopia_present_acquire(PokopiaPresent *present, uint64_t timeout,
                                 PokopiaPresentTarget *target);

/* Submit, wait for rendering, and enqueue the VideoOut presentation. Fence
 * wait makes command-buffer and mapped readback reuse safe on success. A
 * failed submit or fence wait requires closing/reopening the presenter. */
VkResult pokopia_present_frame(PokopiaPresent *present, VkCommandBuffer command,
                               uint64_t timeout);

/* R81: the open frame's own command buffer (free to reset), or VK_NULL_HANDLE
 * when frames are not pipelined. */
VkCommandBuffer pokopia_present_command(const PokopiaPresent *present);

/* Finish outstanding rendering before releasing owned objects. The caller
 * must first stop recording/submitting references to the targets. */
VkResult pokopia_present_close(PokopiaPresent *present);

#endif
