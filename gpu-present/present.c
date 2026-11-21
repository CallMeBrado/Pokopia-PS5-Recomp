/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "present.h"
#include "../gfx-bridge/vk_ledger.h"

#include <stdlib.h>
#include <time.h>

#if defined(__PROSPERO__)
/* PS5 RADV presents through VideoOut behind VK_KHR_display: the first display's
 * first mode, a full-screen opaque plane (probe-verified at 3840x2160). */
VkResult pokopia_present_surface(VkInstance instance, VkSurfaceKHR *surface)
{
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceDisplayPropertiesKHR displays = (PFN_vkGetPhysicalDeviceDisplayPropertiesKHR)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR");
    PFN_vkGetDisplayModePropertiesKHR modes =
        (PFN_vkGetDisplayModePropertiesKHR)vkGetInstanceProcAddr(instance, "vkGetDisplayModePropertiesKHR");
    PFN_vkCreateDisplayPlaneSurfaceKHR create_surface =
        (PFN_vkCreateDisplayPlaneSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateDisplayPlaneSurfaceKHR");
    if (!enumerate || !displays || !modes || !create_surface)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    uint32_t count = 1;
    VkPhysicalDevice physical;
    VkResult result = enumerate(instance, &count, &physical);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
        return result != VK_SUCCESS && result != VK_INCOMPLETE ? result : VK_ERROR_INITIALIZATION_FAILED;
    VkDisplayPropertiesKHR display;
    count = 1;
    result = displays(physical, &count, &display);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
        return result != VK_SUCCESS && result != VK_INCOMPLETE ? result : VK_ERROR_INITIALIZATION_FAILED;
    VkDisplayModePropertiesKHR mode;
    count = 1;
    result = modes(physical, display.display, &count, &mode);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
        return result != VK_SUCCESS && result != VK_INCOMPLETE ? result : VK_ERROR_INITIALIZATION_FAILED;
    const VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mode.displayMode,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = mode.parameters.visibleRegion,
    };
    return create_surface(instance, &info, NULL, surface);
}
#else
VkResult pokopia_present_surface(VkInstance instance, VkSurfaceKHR *surface)
{
    PFN_vkCreateHeadlessSurfaceEXT create_surface =
        (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
    if (!create_surface)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    const VkHeadlessSurfaceCreateInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
    };
    return create_surface(instance, &info, NULL, surface);
}
#endif

static VkResult check_format(VkPhysicalDevice physical, VkSurfaceKHR surface, VkFormat format)
{
    uint32_t count = 0;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, NULL);
    if (result != VK_SUCCESS || !count)
        return result != VK_SUCCESS ? result : VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkSurfaceFormatKHR *formats = calloc(count, sizeof *formats);
    if (!formats)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats);
    if (result == VK_SUCCESS) {
        result = VK_ERROR_FORMAT_NOT_SUPPORTED;
        for (uint32_t i = 0; i < count; ++i) {
            if (formats[i].format == format && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                result = VK_SUCCESS;
                break;
            }
        }
    }
    free(formats);
    return result;
}

VkResult pokopia_present_close(PokopiaPresent *present)
{
    if (!present)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (present->fence_pending) {
        VkResult result = vkWaitForFences(present->device, 1, &present->fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
            return result;
    }
    if (present->async) {
        vkQueueWaitIdle(present->queue);
        for (uint32_t i = 0; i < 3; ++i) {
            vkDestroyFence(present->device, present->ring_fences[i], NULL);
            vkDestroySemaphore(present->device, present->acquired[i], NULL);
            vkDestroySemaphore(present->device, present->rendered[i], NULL);
        }
    }
    if (present->pool)
        vkDestroyCommandPool(present->device, present->pool, NULL);
    if (present->base_fence)
        present->fence = present->base_fence;
    if (present->fence) {
        pokopia_vk_ledger_swapchain_remove(present->image_count);
        vkDestroyFence(present->device, present->fence, NULL);
    }
    if (present->targets) {
        for (uint32_t i = 0; i < present->image_count; ++i) {
            if (present->targets[i].view)
                vkDestroyImageView(present->device, present->targets[i].view, NULL);
        }
    }
    if (present->swapchain)
        vkDestroySwapchainKHR(present->device, present->swapchain, NULL);
    free(present->targets);
    *present = (PokopiaPresent){0};
    return VK_SUCCESS;
}

static VkResult make_targets(PokopiaPresent *present)
{
    VkResult result = vkGetSwapchainImagesKHR(present->device, present->swapchain, &present->image_count, NULL);
    if (result != VK_SUCCESS || !present->image_count)
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;
    VkImage *images = calloc(present->image_count, sizeof *images);
    present->targets = calloc(present->image_count, sizeof *present->targets);
    if (!images || !present->targets) {
        free(images);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    result = vkGetSwapchainImagesKHR(present->device, present->swapchain, &present->image_count, images);
    for (uint32_t i = 0; result == VK_SUCCESS && i < present->image_count; ++i) {
        present->targets[i].image = images[i];
        const VkImageViewCreateInfo view = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = present->format,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        result = vkCreateImageView(present->device, &view, NULL, &present->targets[i].view);
    }
    free(images);
    return result;
}

VkResult pokopia_present_open(PokopiaPresent *present, VkPhysicalDevice physical,
                              VkDevice device, uint32_t family, VkQueue queue,
                              VkSurfaceKHR surface, VkFormat format, VkExtent2D extent,
                              VkImageUsageFlags extra_usage)
{
    if (!present || present->device || !physical || !device || !queue || !surface)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkBool32 supported = VK_FALSE;
    VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported);
    if (result != VK_SUCCESS || !supported)
        return result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED;

    result = check_format(physical, surface, format);
#if defined(__PROSPERO__)
    /* PS5 VideoOut offers BGRA scanout; live_present blits RGBA frames into it. */
    static const VkFormat fallbacks[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
                                          VK_FORMAT_R8G8B8A8_SRGB };
    for (size_t i = 0; result == VK_ERROR_FORMAT_NOT_SUPPORTED && i < sizeof fallbacks / sizeof fallbacks[0]; ++i) {
        result = check_format(physical, surface, fallbacks[i]);
        if (result == VK_SUCCESS)
            format = fallbacks[i];
    }
#endif
    if (result != VK_SUCCESS)
        return result;
    VkSurfaceCapabilitiesKHR caps;
    result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    if (result != VK_SUCCESS)
        return result;

    if (caps.currentExtent.width != UINT32_MAX)
        extent = caps.currentExtent;
    if (extent.width < caps.minImageExtent.width || extent.width > caps.maxImageExtent.width ||
        extent.height < caps.minImageExtent.height || extent.height > caps.maxImageExtent.height)
        return VK_ERROR_INITIALIZATION_FAILED;

    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | extra_usage;
    if ((caps.supportedUsageFlags & usage) != usage ||
        !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface, .minImageCount = caps.minImageCount,
        .imageFormat = format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    *present = (PokopiaPresent){ .device = device, .queue = queue, .format = format, .extent = extent };
    result = vkCreateSwapchainKHR(device, &info, NULL, &present->swapchain);
    if (result == VK_SUCCESS)
        result = make_targets(present);
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (result == VK_SUCCESS)
        result = vkCreateFence(device, &fence, NULL, &present->fence);
    if (result == VK_SUCCESS) {
        present->base_fence = present->fence;
        pokopia_vk_ledger_swapchain_add(present->image_count, present->extent, present->format);
        /* R81: pipelined frames when every object is made; else the host-fenced path. */
        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family,
        };
        bool ok = vkCreateCommandPool(device, &pool_info, NULL, &present->pool) == VK_SUCCESS;
        const VkCommandBufferAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = present->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3,
        };
        ok = ok && vkAllocateCommandBuffers(device, &allocate, present->commands) == VK_SUCCESS;
        const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (uint32_t i = 0; i < 3 && ok; ++i)
            ok = vkCreateFence(device, &fence, NULL, &present->ring_fences[i]) == VK_SUCCESS &&
                 vkCreateSemaphore(device, &semaphore, NULL, &present->acquired[i]) == VK_SUCCESS &&
                 vkCreateSemaphore(device, &semaphore, NULL, &present->rendered[i]) == VK_SUCCESS;
        present->async = ok;
    } else {
        pokopia_present_close(present);
    }
    return result;
}

VkCommandBuffer pokopia_present_command(const PokopiaPresent *present)
{
    return present && present->async && present->frame_open ? present->commands[present->ring_at] : VK_NULL_HANDLE;
}

/* R89: time the async ring waits for a frame three presents back (the GPU
 * is behind), and time in vkAcquireNextImageKHR. */
uint64_t pokopia_present_ring_waits, pokopia_present_ring_wait_ns, pokopia_present_acquire_ns;
static uint64_t present_now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static VkResult wait_fence(PokopiaPresent *present, uint64_t timeout)
{
    present->fence_pending = true;
    VkResult result = vkWaitForFences(present->device, 1, &present->fence, VK_TRUE, timeout);
    if (result == VK_SUCCESS)
        present->fence_pending = false;
    return result;
}

VkResult pokopia_present_acquire(PokopiaPresent *present, uint64_t timeout,
                                 PokopiaPresentTarget *target)
{
    if (!present || !target || !present->swapchain || present->frame_open || present->faulted)
        return VK_ERROR_INITIALIZATION_FAILED;
    *target = (PokopiaPresentTarget){0};

    if (present->async) {
        const uint32_t slot = present->ring_at;
        if (present->ring_used[slot]) {
            const uint64_t wait_start = present_now_ns();
            const VkResult done = vkWaitForFences(present->device, 1, &present->ring_fences[slot], VK_TRUE, timeout);
            const uint64_t waited = present_now_ns() - wait_start;
            if (waited > UINT64_C(50000)) {
                ++pokopia_present_ring_waits;
                pokopia_present_ring_wait_ns += waited;
            }
            if (done != VK_SUCCESS) {
                if (done != VK_TIMEOUT)
                    present->faulted = true;
                return done;
            }
            present->ring_used[slot] = false;
        }
        VkResult acquired = vkResetFences(present->device, 1, &present->ring_fences[slot]);
        const uint64_t acquire_start = present_now_ns();
        if (acquired == VK_SUCCESS)
            acquired = vkAcquireNextImageKHR(present->device, present->swapchain, timeout, present->acquired[slot],
                                             VK_NULL_HANDLE, &present->image_index);
        pokopia_present_acquire_ns += present_now_ns() - acquire_start;
        if (acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR) {
            present->fence = present->ring_fences[slot];
            present->frame_open = true;
            *target = present->targets[present->image_index];
        } else if (acquired != VK_TIMEOUT && acquired != VK_NOT_READY) {
            present->faulted = true;
        }
        return acquired;
    }

    VkResult result = vkResetFences(present->device, 1, &present->fence);
    if (result != VK_SUCCESS) {
        present->faulted = true;
        return result;
    }
    result = vkAcquireNextImageKHR(present->device, present->swapchain, timeout,
                                  VK_NULL_HANDLE, present->fence, &present->image_index);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        const VkResult completed = wait_fence(present, timeout);
        if (completed != VK_SUCCESS) {
            present->faulted = true;
            return completed;
        }
        present->frame_open = true;
        *target = present->targets[present->image_index];
    } else if (result != VK_TIMEOUT && result != VK_NOT_READY) {
        present->faulted = true;
    }
    return result;
}

VkResult pokopia_present_frame(PokopiaPresent *present, VkCommandBuffer command,
                               uint64_t timeout)
{
    if (!present || !present->frame_open || present->faulted || !command)
        return VK_ERROR_INITIALIZATION_FAILED;

    if (present->async) {
        const uint32_t slot = present->ring_at;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        const VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &present->acquired[slot], .pWaitDstStageMask = &stage,
            .commandBufferCount = 1, .pCommandBuffers = &command,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &present->rendered[slot],
        };
        VkResult result = vkQueueSubmit(present->queue, 1, &submit, present->ring_fences[slot]);
        if (result == VK_SUCCESS) {
            present->ring_used[slot] = true;
            const VkPresentInfoKHR info = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1, .pWaitSemaphores = &present->rendered[slot],
                .swapchainCount = 1, .pSwapchains = &present->swapchain,
                .pImageIndices = &present->image_index,
            };
            result = vkQueuePresentKHR(present->queue, &info);
        }
        present->ring_at = (slot + 1) % 3;
        present->frame_open = false;
        present->faulted = result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR;
        return result;
    }

    VkResult result = vkResetFences(present->device, 1, &present->fence);
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &command,
    };
    if (result == VK_SUCCESS)
        result = vkQueueSubmit(present->queue, 1, &submit, present->fence);
    if (result == VK_SUCCESS)
        result = wait_fence(present, timeout);

    /* Acquire and render are host-fenced. The WSI owns image reuse,
     * including the image VideoOut is currently scanning. */
    if (result == VK_SUCCESS) {
        const VkPresentInfoKHR info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1, .pSwapchains = &present->swapchain,
            .pImageIndices = &present->image_index,
        };
        result = vkQueuePresentKHR(present->queue, &info);
    }
    present->frame_open = false;
    present->faulted = result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR;
    return result;
}
