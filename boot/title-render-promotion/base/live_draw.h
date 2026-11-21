/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_LIVE_DRAW_H
#define POKOPIA_LIVE_DRAW_H

#include "vk_adapter.h"

typedef struct {
    const void *data;
    size_t bytes;
} PokopiaLiveDrawSpan;

typedef struct {
    VkShaderModule module;
    const char *entry;
    const char *hash;
} PokopiaLiveDrawShader;

typedef struct {
    PokopiaVkTexture *texture;
    const NvnFrontTextureInfo *guest_info;
    NvnFrontSamplerInfo sampler;
    VkFormat view_format;
} PokopiaLiveDrawSample;

enum { POKOPIA_LIVE_DRAW_SAMPLE_COUNT = 2 };

typedef struct {
    PokopiaVkTexture *target, *depth_target;
    uint32_t target_layer;
    VkExtent2D extent;
    PokopiaLiveDrawShader vertex_shader, fragment_shader;
    PokopiaLiveDrawSpan vertices, indices, vertex_uniform, fragment_uniform;
    PokopiaLiveDrawSample fragment_samples[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
    uint32_t vertex_stride;
    VkVertexInputAttributeDescription attributes[4];
    VkPrimitiveTopology topology;
    VkIndexType index_type;
    VkViewport viewport;
    VkRect2D scissor;
    VkColorComponentFlags color_write_mask;
    VkPolygonMode polygon_mode;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkStencilOpState stencil[2];
    NvnFrontBlendInfo blend;
    bool blend_enable, depth_test, depth_write, stencil_test;
    uint32_t vertex_count, first_vertex;
    uint32_t index_count, instance_count, first_index, first_instance;
    int32_t base_vertex;
} PokopiaLiveDraw;

/* Submit a supported draw against a single mip-0 layer. The caller validates guest memory, attachment shape and all remaining
 * NVN state, and serializes the adapter queue, resources and command pool.
 * The spans and linked shaders are borrowed for the duration of the call.
 * A successful return commits the texture state and a completed render serial.
 * Any error other than VK_ERROR_FORMAT_NOT_SUPPORTED is terminal to replay. */
VkResult pokopia_live_draw(PokopiaVkAdapter *adapter, const PokopiaLiveDraw *draw,
                                  uint64_t *completed_render_serial);

#endif
