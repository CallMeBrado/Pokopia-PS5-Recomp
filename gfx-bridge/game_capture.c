/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_capture.h"

bool pokopia_game_capture_init(PokopiaGameCapture *capture, const char *directory)
{
    return capture && !capture->mailbox.directory &&
           pokopia_debug_mailbox_init(&capture->mailbox, directory);
}

bool pokopia_game_capture_requested(PokopiaGameCapture *capture)
{
    return capture && capture->mailbox.directory &&
           (capture->path[0] || pokopia_debug_capture(&capture->mailbox, capture->path));
}

VkResult pokopia_game_capture_record(PokopiaGameCapture *capture, VkPhysicalDevice physical,
                                    const PokopiaPresent *present, VkCommandBuffer command,
                                    VkImageLayout layout, VkPipelineStageFlags stages,
                                    VkAccessFlags access, uint64_t guest_render_serial)
{
    if (!capture || !capture->mailbox.directory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!guest_render_serial || capture->pending_render_serial)
        return VK_NOT_READY;
    if (!physical || !present || !present->device || !present->swapchain || !present->fence ||
        !present->frame_open || present->faulted || present->fence_pending || !present->targets ||
        present->image_index >= present->image_count || !present->targets[present->image_index].image)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!pokopia_game_capture_requested(capture))
        return VK_NOT_READY;

    PokopiaFrameCapture *readback = &capture->readback;
    if (readback->device && (readback->device != present->device || capture->format != present->format ||
        readback->extent.width != present->extent.width || readback->extent.height != present->extent.height))
        pokopia_frame_capture_close(readback);
    if (!readback->device) {
        VkResult result = pokopia_frame_capture_open(readback, physical, present->device, present->format,
                                                      present->extent);
        if (result != VK_SUCCESS)
            return result;
        capture->format = present->format;
    }

    VkResult result = pokopia_frame_capture_record(readback, command,
        present->targets[present->image_index].image, layout, stages, access);
    if (result == VK_SUCCESS) {
        capture->pending_present = present;
        capture->pending_fence = present->fence;
        capture->pending_image_index = present->image_index;
        capture->pending_render_serial = guest_render_serial;
    }
    return result;
}

VkResult pokopia_game_capture_finish(PokopiaGameCapture *capture, const PokopiaPresent *present,
                                    uint64_t guest_render_serial, VkResult present_result)
{
    if (!capture || !capture->mailbox.directory)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!capture->pending_render_serial)
        return VK_NOT_READY;
    if (!present || present != capture->pending_present || guest_render_serial != capture->pending_render_serial ||
        present->fence != capture->pending_fence || present->image_index != capture->pending_image_index)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (present_result != VK_SUCCESS && present_result != VK_SUBOPTIMAL_KHR)
        return present_result;
    if (present->faulted)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (present->frame_open || present->fence_pending)
        return VK_NOT_READY;

    VkResult result = pokopia_frame_capture_save(&capture->readback, capture->pending_fence, capture->path);
    if (!capture->readback.recorded) {
        capture->pending_present = NULL;
        capture->pending_fence = VK_NULL_HANDLE;
        capture->pending_render_serial = 0;
    }
    if (result == VK_SUCCESS) {
        capture->captured_render_serial = guest_render_serial;
        capture->captured_request_seq = capture->mailbox.capture_seq;
        capture->path[0] = '\0';
    }
    return result;
}

void pokopia_game_capture_close(PokopiaGameCapture *capture)
{
    if (!capture)
        return;
    pokopia_frame_capture_close(&capture->readback);
    *capture = (PokopiaGameCapture){0};
}
