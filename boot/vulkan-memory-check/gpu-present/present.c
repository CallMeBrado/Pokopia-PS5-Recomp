/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "present.h"
#include "vk_ledger.h"

#include <stdlib.h>

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
    if (result == VK_SUCCESS)
        pokopia_vk_ledger_swapchain_add(present->image_count, present->extent, present->format);
    else
        pokopia_present_close(present);
    return result;
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
