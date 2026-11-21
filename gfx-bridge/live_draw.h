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

/* R61 general draw: any translated vertex/fragment pair, described by its
 * own resource bindings (Ryujinx's Vulkan scheme: set 0 uniform buffers with
 * binding 0 the support buffer, set 2 combined image samplers) and the
 * recorded NVN state already converted to Vulkan terms. */
enum {
    POKOPIA_GENERIC_UNIFORMS = 16, POKOPIA_GENERIC_TEXTURES = 40, /* R73: lit shaders bind up to 29 */
    POKOPIA_GENERIC_STREAMS = 8, POKOPIA_GENERIC_ATTRIBUTES = 16,
};

typedef struct {
    uint32_t binding;
    PokopiaLiveDrawSpan data;
} PokopiaGenericUniform;

typedef struct {
    uint32_t binding;
    PokopiaVkTexture *texture;
    const NvnFrontTextureInfo *guest_info;
    VkFormat view_format;
    NvnFrontSamplerInfo sampler;
    uint8_t swizzle[4]; /* R66: NVN TextureSwizzle per component; all zero = identity */
    /* R73: a texTexture*|Shadow binding samples with depth comparison. */
    bool shadow;
    /* R73: a 2D texture bound where the shader declares a 2D array. */
    bool as_array;
    /* R73: a texTextureBuffer binding is a uniform texel buffer; texture is
     * the NVN buffer texture and the image fields are unused. */
    PokopiaVkTexel *texel;
} PokopiaGenericTexture;

typedef struct {
    PokopiaVkTexture *target, *depth_target;
    /* R63: color(i+1) for multiple render targets; NULL slots are unused.
     * color_count 0 or 1 means only target. color_write_masks packs 4 bits per
     * target; blend_mask has one bit per target. */
    PokopiaVkTexture *more_targets[7];
    uint32_t color_count, color_write_masks, blend_mask;
    uint32_t color_layers[8]; /* R64: array layer rendered per colour target */
    uint32_t color_levels[8]; /* R73: mip level rendered per colour target */
    VkExtent2D extent;
    PokopiaLiveDrawShader vertex_shader, fragment_shader;
    PokopiaGenericUniform uniforms[POKOPIA_GENERIC_UNIFORMS];
    uint32_t uniform_count;
    PokopiaGenericTexture textures[POKOPIA_GENERIC_TEXTURES];
    uint32_t texture_count;
    VkVertexInputBindingDescription bindings[POKOPIA_GENERIC_STREAMS];
    PokopiaLiveDrawSpan streams[POKOPIA_GENERIC_STREAMS]; /* indexed by binding description order */
    uint32_t binding_count;
    VkVertexInputAttributeDescription attributes[POKOPIA_GENERIC_ATTRIBUTES];
    uint32_t attribute_count;
    PokopiaLiveDrawSpan indices;
    VkIndexType index_type;
    VkPrimitiveTopology topology;
    VkViewport viewport;
    VkRect2D scissor;
    VkCullModeFlags cull_mode;
    VkFrontFace front_face;
    VkColorComponentFlags color_write_mask;
    bool blend_enable, depth_test, depth_write, stencil_test;
    VkBlendFactor src_rgb, dst_rgb, src_alpha, dst_alpha;
    VkBlendOp rgb_op, alpha_op;
    VkCompareOp depth_compare;
    VkStencilOpState stencil[2];
    uint32_t vertex_count, first_vertex, index_count, first_index;
    int32_t base_vertex;
    uint32_t instance_count, first_instance; /* R65; 0 instances means 1 */
    /* R73: NVN fill polygon offset as Vulkan depth bias (factor, units). */
    bool depth_bias;
    float depth_bias_factor, depth_bias_units;
} PokopiaGenericDraw;

/* R73: general compute dispatch (see live_draw_generic.inc). */
enum { POKOPIA_COMPUTE_UNIFORMS = 8, POKOPIA_COMPUTE_STORAGE = 8, POKOPIA_COMPUTE_TEXTURES = 8,
       POKOPIA_COMPUTE_IMAGES = 8 };

typedef struct {
    uint32_t binding;
    void *data; /* canonical guest memory; written back after the dispatch */
    size_t bytes;
} PokopiaGenericStorage;

typedef struct {
    uint32_t binding;
    PokopiaVkTexture *texture;
} PokopiaGenericImage;

typedef struct {
    PokopiaLiveDrawShader shader;
    uint32_t groups[3];
    PokopiaGenericUniform uniforms[POKOPIA_COMPUTE_UNIFORMS];
    uint32_t uniform_count;
    PokopiaGenericStorage storage[POKOPIA_COMPUTE_STORAGE];
    uint32_t storage_count;
    PokopiaGenericTexture textures[POKOPIA_COMPUTE_TEXTURES];
    uint32_t texture_count;
    PokopiaGenericImage images[POKOPIA_COMPUTE_IMAGES];
    uint32_t image_count;
} PokopiaGenericCompute;

VkResult pokopia_live_compute_generic(PokopiaVkAdapter *adapter, const PokopiaGenericCompute *compute);

/* Same contract as pokopia_live_draw. VK_ERROR_FORMAT_NOT_SUPPORTED means the
 * draw was not attempted and changed nothing. */
VkResult pokopia_live_draw_generic(PokopiaVkAdapter *adapter, const PokopiaGenericDraw *draw,
                                   uint64_t *completed_render_serial);

#endif
