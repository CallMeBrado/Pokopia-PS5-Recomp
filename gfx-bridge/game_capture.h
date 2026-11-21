/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_GAME_CAPTURE_H
#define POKOPIA_GAME_CAPTURE_H

#include "../gpu-present/debug_mailbox.h"
#include "../gpu-present/frame_capture.h"
#include "../gpu-present/present.h"

typedef struct {
    /* Updated only after a game frame was accepted for present and saved. */
    uint64_t captured_render_serial, captured_request_seq;

    PokopiaDebugMailbox mailbox;
    PokopiaFrameCapture readback;
    VkFormat format;
    const PokopiaPresent *pending_present;
    VkFence pending_fence;
    uint64_t pending_render_serial;
    uint32_t pending_image_index;
    char path[POKOPIA_DEBUG_PATH_BYTES];
} PokopiaGameCapture;

/* Start with a zeroed object. Keep directory alive until close. Calls are
 * serialized on the presentation thread. */
bool pokopia_game_capture_init(PokopiaGameCapture *capture, const char *directory);

/* Claim a mailbox request once a game-produced image is available. Keeps the
 * request pending until it can be recorded and published. */
bool pokopia_game_capture_requested(PokopiaGameCapture *capture);

/* Call outside rendering, after copying/drawing the game's final image to
 * the acquired swapchain target. The swapchain must have TRANSFER_SRC usage.
 * Supply a nonzero serial only for a frame produced by actual game rendering.
 * On VK_SUCCESS the command includes the final PRESENT layout transition.
 * VK_NOT_READY means there is no capture for this frame. */
VkResult pokopia_game_capture_record(PokopiaGameCapture *capture, VkPhysicalDevice physical,
                                    const PokopiaPresent *present, VkCommandBuffer command,
                                    VkImageLayout layout, VkPipelineStageFlags stages,
                                    VkAccessFlags access, uint64_t guest_render_serial);

/* Call with the result of presenting the exact command recorded above,
 * before the next acquire resets the fence. VK_NOT_READY allows a retry.
 * A failed presentation must be retired and closed before reuse. */
VkResult pokopia_game_capture_finish(PokopiaGameCapture *capture, const PokopiaPresent *present,
                                    uint64_t guest_render_serial, VkResult present_result);

/* Retire any submitted copy and discard any unsubmitted command first. */
void pokopia_game_capture_close(PokopiaGameCapture *capture);

#endif
