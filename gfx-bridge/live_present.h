/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_LIVE_PRESENT_H
#define POKOPIA_LIVE_PRESENT_H

#include "game_capture.h"
#include "debug_overlay_pixels.h"

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
    VkCommandBuffer active_command; /* R81: the open frame's command buffer */
    VkImage conversion;
    VkDeviceMemory conversion_memory;
    VkExtent2D conversion_extent;
    VkFormat conversion_format;
    VkBuffer overlay_buffer;
    VkDeviceMemory overlay_memory;
    void *overlay_pixels;
    bool overlay_coherent, overlay_ready;
    /* R75: frame-rate panel in the top-right corner. */
    VkBuffer fps_buffer;
    VkDeviceMemory fps_memory;
    void *fps_pixels;
    bool fps_coherent, fps_ready;
    /* R84: AMD FSR 1 (EASU upscale, then RCAS sharpen; compute) in place of the
     * linear blit to the scanout size. Built on first use; the blit remains the
     * fallback. One descriptor-set pair per frame in flight. */
    bool fsr_tried, fsr_ready;
    VkSampler fsr_sampler;
    VkDescriptorSetLayout fsr_set_layout;
    VkPipelineLayout fsr_layouts[2];
    VkPipeline fsr_pipelines[2];
    VkDescriptorPool fsr_pool;
    VkDescriptorSet fsr_sets[3][2];
    VkImage fsr_images[2];
    VkDeviceMemory fsr_memory[2];
    VkImageView fsr_sample_view, fsr_storage_views[2];
    VkFormat fsr_out_format;
    struct { VkImage image; VkFormat format; VkImageView view; } fsr_inputs[8];
    unsigned fsr_input_count;
    /* R86: the on-screen keyboard's two panels (a band across the top and the
     * keyboard across the bottom), one buffer, composited while visible. */
    VkBuffer keyboard_buffer;
    VkDeviceMemory keyboard_memory;
    void *keyboard_pixels;
    bool keyboard_coherent, keyboard_visible;
    uint32_t keyboard_width, keyboard_top, keyboard_bottom;
} PokopiaLivePresent;

/* Start with a zeroed object. Borrow an open native swapchain and a dedicated
 * resettable primary command buffer on its graphics queue. The swapchain needs
 * TRANSFER_DST usage and, with capture, TRANSFER_SRC usage. The caller retains
 * both objects and serializes their use with the live game's render queue. */
VkResult pokopia_live_present_init(PokopiaLivePresent *live, VkPhysicalDevice physical,
    PokopiaPresent *present, VkCommandBuffer command, PokopiaGameCapture *capture);

/* Update the optional panel between submissions, then composite it on every
 * game flip. Show it over a blank image before the first game flip. */
VkResult pokopia_live_present_overlay_update(PokopiaLivePresent *live,
    const char *const lines[POKOPIA_OVERLAY_LINES]);
VkResult pokopia_live_present_show_overlay(PokopiaLivePresent *live, uint64_t timeout_ns);
/* R75: one line of text composited at the top-right of every game flip.
 * Same update rules as the panel above. */
VkResult pokopia_live_present_fps_update(PokopiaLivePresent *live, const char *text);

/* R86: the keyboard panels' pixels (4 bytes each; B,G,R,A when *bgra), made on
 * first use at the scanout width: top rows, then bottom rows. Flush after
 * drawing; set keyboard_visible to composite them on every frame. */
VkResult pokopia_live_present_keyboard_pixels(PokopiaLivePresent *live, uint32_t top, uint32_t bottom,
    uint8_t **pixels, uint32_t *width, bool *bgra);
VkResult pokopia_live_present_keyboard_flush(PokopiaLivePresent *live);

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
