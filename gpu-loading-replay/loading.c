/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gfx-bridge/texture_upload.h"
#include "loading-data.h"

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#include "../gfx-bridge/game_capture.h"
#define INPUT_FILE "/app0/assets/images/loading-draws.bin"
#else
#define INPUT_FILE "generated/package/assets/images/loading-draws.bin"
#endif

enum { WIDTH = 1920, HEIGHT = 1080, IMAGE_BYTES = WIDTH * HEIGHT * 4, DRAW_COUNT = 2 };
static const VkFormat color_format = VK_FORMAT_R8G8B8A8_SRGB;
static const VkImageAspectFlags depth_aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
static const uint64_t timeout_ns = UINT64_C(5000000000);
static FILE *log_file;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;

typedef struct {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
} Image;

typedef struct {
    VkDeviceSize vs, fs, index;
    uint32_t index_count;
    VkExtent2D texture_extent;
    VkFilter filter;
    size_t texture_offset, texture_bytes;
} Draw;

static void report(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char line[384];
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    puts(line);
    fflush(stdout);
    if (log_file) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);
    }
}

static _Noreturn void fail(const char *where, int error)
{
    report("FAIL %s (%d)", where, error);
#ifdef __ORBIS__
    for (;;)
        sceKernelUsleep(1000000);
#else
    exit(EXIT_FAILURE);
#endif
}

static void checked(VkResult result, const char *where)
{
    if (result != VK_SUCCESS)
        fail(where, result);
}

#define VK(call) checked((call), #call)

static unsigned char *load_inputs(void)
{
    FILE *file = fopen(INPUT_FILE, "rb");
    if (!file)
        fail("captured draw data file", 0);
    unsigned char *payload = malloc(PAYLOAD_SIZE);
    if (!payload)
        fail("captured draw data allocation", VK_ERROR_OUT_OF_HOST_MEMORY);
    const bool valid = fread(payload, 1, PAYLOAD_SIZE, file) == PAYLOAD_SIZE &&
                       fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!valid)
        fail("captured draw data length", 0);
    return payload;
}

static VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags required,
                               bool *coherent)
{
    uint32_t chosen = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) || (flags & required) != required)
            continue;
        if (chosen == UINT32_MAX)
            chosen = i;
        if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
            chosen = i;
            break;
        }
    }
    if (chosen == UINT32_MAX)
        fail("Vulkan memory type", VK_ERROR_FEATURE_NOT_PRESENT);
    if (coherent)
        *coherent = (memory.memoryTypes[chosen].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    const VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = chosen,
    };
    VkDeviceMemory allocated = VK_NULL_HANDLE;
    VK(vkAllocateMemory(device, &info, NULL, &allocated));
    return allocated;
}

static Image create_image(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
                          VkImageAspectFlags aspects)
{
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {extent.width, extent.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    Image image = {0};
    VK(vkCreateImage(device, &info, NULL, &image.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    image.memory = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    VK(vkBindImageMemory(device, image.image, image.memory, 0));
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = {aspects, 0, 1, 0, 1},
    };
    VK(vkCreateImageView(device, &view, NULL, &image.view));
    return image;
}

static void destroy_image(Image image)
{
    vkDestroyImageView(device, image.view, NULL);
    vkDestroyImage(device, image.image, NULL);
    vkFreeMemory(device, image.memory, NULL);
}

static VkSampler create_sampler(VkFilter filter)
{
    const VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = filter, .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .maxAnisotropy = 1,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkSampler sampler = VK_NULL_HANDLE;
    VK(vkCreateSampler(device, &info, NULL, &sampler));
    return sampler;
}

static VkShaderModule shader(const unsigned char *code, size_t bytes)
{
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
        .pCode = (const uint32_t *)code,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VK(vkCreateShaderModule(device, &info, NULL, &module));
    return module;
}

static VkPipeline create_pipeline(VkPipelineLayout layout, VkFormat depth, const unsigned char *data)
{
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = shader(data + VERTEX_SPV_OFFSET, VERTEX_SPV_SIZE), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = shader(data + FRAGMENT_SPV_OFFSET, FRAGMENT_SPV_SIZE), .pName = "main"},
    };
    const VkVertexInputBindingDescription binding = {.binding = 0, .stride = 44};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32_SFLOAT, 40}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 36}, {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
    };
    const VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 4, .pVertexAttributeDescriptions = attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkViewport viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1};
    const VkRect2D scissor = {{0, 0}, {WIDTH, HEIGHT}};
    const VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthCompareOp = VK_COMPARE_OP_ALWAYS,
    };
    const VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
    };
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &color_format,
        .depthAttachmentFormat = depth, .stencilAttachmentFormat = depth,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pDepthStencilState = &depth_state, .pColorBlendState = &blend, .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline));
    for (size_t i = 0; i < 2; ++i)
        vkDestroyShaderModule(device, stages[i].module, NULL);
    return pipeline;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspects,
                    VkImageLayout old, VkImageLayout next, VkPipelineStageFlags source, VkAccessFlags write,
                    VkPipelineStageFlags destination, VkAccessFlags access)
{
    const VkImageMemoryBarrier info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = old, .newLayout = next,
        .srcAccessMask = write, .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {aspects, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, source, destination, 0, 0, NULL, 0, NULL, 1, &info);
}

static void begin(VkCommandBuffer cmd)
{
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(vkBeginCommandBuffer(cmd, &info));
}

static void render(VkCommandBuffer cmd, Image color, Image depth, VkPipeline pipeline, VkPipelineLayout layout,
                   const VkDescriptorSet sets[4], const Draw draws[DRAW_COUNT], VkBuffer buffer,
                   VkDeviceSize vertices, VkDeviceSize readback)
{
    barrier(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(cmd, depth.image, depth_aspects, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo color_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = color.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = {0, 0, 0, 1},
    };
    const VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.depthStencil = {0, 0},
    };
    const VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {WIDTH, HEIGHT}}, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &color_attachment,
        .pDepthAttachment = &depth_attachment, .pStencilAttachment = &depth_attachment,
    };
    vkCmdBeginRendering(cmd, &info);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vertices);
    for (uint32_t i = 0; i < DRAW_COUNT; ++i) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[i * 2], 0, NULL);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &sets[i * 2 + 1], 0, NULL);
        vkCmdBindIndexBuffer(cmd, buffer, draws[i].index, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, draws[i].index_count, 1, 0, 0, 0);
    }
    vkCmdEndRendering(cmd);
    barrier(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy copy = {
        .bufferOffset = readback, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1},
    };
    vkCmdCopyImageToBuffer(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    const VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .buffer = buffer, .offset = readback, .size = IMAGE_BYTES,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host, 0, NULL);
}

static VkPhysicalDevice graphics_device(VkInstance instance, uint32_t *family)
{
    VkPhysicalDevice physical[16];
    uint32_t count = sizeof physical / sizeof physical[0];
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, physical);
    if (result != VK_INCOMPLETE)
        VK(result);
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3 || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;
        VkQueueFamilyProperties queues[32];
        uint32_t length = sizeof queues / sizeof queues[0];
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i], &length, queues);
        for (uint32_t j = 0; j < length; ++j) {
            if ((queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queues[j].queueCount) {
                *family = j;
                return physical[i];
            }
        }
    }
    fail("Vulkan 1.3 graphics queue", VK_ERROR_FEATURE_NOT_PRESENT);
}

static VkFormat choose_depth(VkPhysicalDevice physical)
{
    const VkFormat formats[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (size_t i = 0; i < 2; ++i) {
        VkImageFormatProperties properties;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(physical, formats[i], VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &properties);
        if (result == VK_SUCCESS && properties.maxExtent.width >= WIDTH && properties.maxExtent.height >= HEIGHT)
            return formats[i];
        if (result != VK_SUCCESS && result != VK_ERROR_FORMAT_NOT_SUPPORTED)
            VK(result);
    }
    fail("depth/stencil attachment format", VK_ERROR_FORMAT_NOT_SUPPORTED);
}

static VkDescriptorSetLayout set_layout(const VkDescriptorSetLayoutBinding *bindings, uint32_t count)
{
    const VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = count, .pBindings = bindings,
    };
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VK(vkCreateDescriptorSetLayout(device, &info, NULL, &layout));
    return layout;
}

static VkDeviceSize aligned(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static void check_readback(const unsigned char *pixels)
{
    unsigned low_x = WIDTH, low_y = HEIGHT, high_x = 0, high_y = 0;
    uint64_t colored = 0, fingerprint = UINT64_C(14695981039346656037);
    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            const unsigned char *rgb = pixels + ((size_t)y * WIDTH + x) * 4;
            if (rgb[0] || rgb[1] || rgb[2]) {
                ++colored;
                if (x < low_x) low_x = x;
                if (y < low_y) low_y = y;
                if (x > high_x) high_x = x;
                if (y > high_y) high_y = y;
            }
            for (size_t c = 0; c < 3; ++c)
                fingerprint = (fingerprint ^ rgb[c]) * UINT64_C(1099511628211);
        }
    }
    report("Loading frame 540: %" PRIu64 " nonblack pixels, bounds %u,%u to %u,%u, RGB FNV-1a %016" PRIx64,
           colored, low_x, low_y, high_x, high_y, fingerprint);
    if (!colored || colored == WIDTH * HEIGHT)
        fail("captured loading indicator did not render", 0);
#ifndef __ORBIS__
    FILE *file = fopen("generated/desktop-loading.rgba", "wb");
    if (!file)
        fail("desktop readback file", 0);
    const bool written = fwrite(pixels, 1, IMAGE_BYTES, file) == IMAGE_BYTES;
    if (fclose(file) != 0 || !written)
        fail("desktop readback write", 0);
#endif
}

#ifdef __ORBIS__
static void preview(PokopiaPresent *present, PokopiaGameCapture *capture, VkPhysicalDevice physical,
                    VkCommandBuffer cmd, VkImage rendered)
{
    PokopiaPresentTarget target;
    VkResult result = pokopia_present_acquire(present, timeout_ns, &target);
    if (result != VK_SUBOPTIMAL_KHR)
        VK(result);
    VK(vkResetCommandBuffer(cmd, 0));
    begin(cmd);
    barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkImageCopy copy = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {WIDTH, HEIGHT, 1},
    };
    vkCmdCopyImage(cmd, rendered, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const VkResult recorded = pokopia_game_capture_record(capture, physical, present, cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 540);
    if (recorded != VK_SUCCESS) {
        if (recorded != VK_NOT_READY)
            report("Capture record result %d", (int)recorded);
        barrier(cmd, target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    VK(vkEndCommandBuffer(cmd));
    result = pokopia_present_frame(present, cmd, timeout_ns);
    if (result != VK_SUBOPTIMAL_KHR)
        VK(result);
    if (recorded == VK_SUCCESS) {
        VK(pokopia_game_capture_finish(capture, present, 540, result));
        report("Captured loading replay frame %" PRIu64 " request %016" PRIx64,
               capture->captured_render_serial, capture->captured_request_seq);
    }
}
#endif

int main(void)
{
#ifdef __ORBIS__
    log_file = fopen("/data/pokopia-ps4-loading-replay.txt", "w");
#endif
    report("Pokopia Loading Replay | update 655360 frame 540 draws 3 and 4");
    unsigned char *input = load_inputs();
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Pokopia Loading Replay",
        .apiVersion = VK_API_VERSION_1_3,
    };
#ifdef __ORBIS__
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#endif
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
#ifdef __ORBIS__
        .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_extensions,
#endif
    };
    VkInstance instance = VK_NULL_HANDLE;
    VK(vkCreateInstance(&instance_info, NULL, &instance));
    uint32_t family = 0;
    VkPhysicalDevice physical = graphics_device(instance, &family);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const VkFormat depth_format = choose_depth(physical);
    report("Vulkan device %s, depth/stencil format %u", properties.deviceName, (unsigned)depth_format);
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f13,
    };
    VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    if (!f13.dynamicRendering || !f11.shaderDrawParameters || !supported.features.shaderFloat64 ||
        !supported.features.shaderImageGatherExtended || !supported.features.textureCompressionBC)
        fail("Vulkan shader and BC7 features", VK_ERROR_FEATURE_NOT_PRESENT);
    f13 = (VkPhysicalDeviceVulkan13Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE,
    };
    f11 = (VkPhysicalDeviceVulkan11Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f13,
        .shaderDrawParameters = VK_TRUE,
    };
    const VkPhysicalDeviceFeatures features = {
        .shaderFloat64 = VK_TRUE, .shaderImageGatherExtended = VK_TRUE, .textureCompressionBC = VK_TRUE,
    };
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
        .queueCount = 1, .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f11, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info, .pEnabledFeatures = &features,
#ifdef __ORBIS__
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_extensions,
#endif
    };
    VK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    const VkDeviceSize align = properties.limits.minUniformBufferOffsetAlignment;
    Draw draws[DRAW_COUNT] = {
        {.vs = aligned(SUPPORT_UBO_SIZE, align), .index_count = 6, .texture_extent = {4, 16},
         .filter = VK_FILTER_NEAREST, .texture_offset = BC7_3_OFFSET, .texture_bytes = BC7_3_SIZE},
        {.index_count = 12, .texture_extent = {1024, 2048},
         .filter = VK_FILTER_LINEAR, .texture_offset = BC7_4_OFFSET, .texture_bytes = BC7_4_SIZE},
    };
    draws[0].fs = aligned(draws[0].vs + VS3_SIZE, align);
    draws[1].vs = aligned(draws[0].fs + FS3_SIZE, align);
    draws[1].fs = aligned(draws[1].vs + VS4_SIZE, align);
    const VkDeviceSize vertices = aligned(draws[1].fs + FS4_SIZE, 4);
    draws[0].index = vertices + VERTICES_SIZE;
    draws[1].index = draws[0].index + INDEX3_SIZE;
    const VkDeviceSize readback = aligned(draws[1].index + INDEX4_SIZE, 4);
    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = readback + IMAGE_BYTES,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer = VK_NULL_HANDLE;
    VK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    bool coherent = false;
    VkDeviceMemory backing = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &coherent);
    VK(vkBindBufferMemory(device, buffer, backing, 0));
    void *host = NULL;
    VK(vkMapMemory(device, backing, 0, VK_WHOLE_SIZE, 0, &host));
    unsigned char *mapped = host;
    memcpy(mapped, input + SUPPORT_UBO_OFFSET, SUPPORT_UBO_SIZE);
    memcpy(mapped + vertices, input + VERTICES_OFFSET, VERTICES_SIZE);
    memcpy(mapped + draws[0].vs, input + VS3_OFFSET, VS3_SIZE);
    memcpy(mapped + draws[0].fs, input + FS3_OFFSET, FS3_SIZE);
    memcpy(mapped + draws[0].index, input + INDEX3_OFFSET, INDEX3_SIZE);
    memcpy(mapped + draws[1].vs, input + VS4_OFFSET, VS4_SIZE);
    memcpy(mapped + draws[1].fs, input + FS4_OFFSET, FS4_SIZE);
    memcpy(mapped + draws[1].index, input + INDEX4_OFFSET, INDEX4_SIZE);
    const VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = backing, .size = VK_WHOLE_SIZE,
    };
    if (!coherent)
        VK(vkFlushMappedMemoryRanges(device, 1, &range));

    NvnTextureUpload *uploader = NULL;
    VK(nvn_texture_upload_open(&uploader, device, queue, family, &memory, vkGetDeviceProcAddr, BC7_4_SIZE));
    Image textures[DRAW_COUNT];
    VkSampler samplers[DRAW_COUNT];
    for (size_t i = 0; i < DRAW_COUNT; ++i) {
        const VkExtent2D extent = draws[i].texture_extent;
        textures[i] = create_image(extent, VK_FORMAT_BC7_SRGB_BLOCK,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        samplers[i] = create_sampler(draws[i].filter);
        const NvnTextureUploadRegion upload = {
            .image = textures[i].image, .format = VK_FORMAT_BC7_SRGB_BLOCK, .mip_extent = extent, .extent = extent,
            .before = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0},
            .after = {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT},
        };
        VK(nvn_texture_upload_submit(uploader, &upload, input + draws[i].texture_offset, draws[i].texture_bytes, 0));
        VK(nvn_texture_upload_wait(uploader, timeout_ns));
    }
    VK(nvn_texture_upload_close(&uploader));
    report("Uploaded original BC7 textures 4x16 and 1024x2048");

    const VkDescriptorSetLayoutBinding ubo[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {22, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    const VkDescriptorSetLayoutBinding texture = {
        128, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL,
    };
    VkDescriptorSetLayout layouts[] = {set_layout(ubo, 3), set_layout(NULL, 0), set_layout(&texture, 1)};
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3, .pSetLayouts = layouts,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetLayout allocate_layouts[] = {layouts[0], layouts[2], layouts[0], layouts[2]};
    const VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
        .descriptorSetCount = 4, .pSetLayouts = allocate_layouts,
    };
    VkDescriptorSet sets[4];
    VK(vkAllocateDescriptorSets(device, &set_info, sets));
    for (size_t i = 0; i < DRAW_COUNT; ++i) {
        const VkDescriptorBufferInfo buffers[] = {
            {buffer, 0, SUPPORT_UBO_SIZE}, {buffer, draws[i].vs, VS3_SIZE}, {buffer, draws[i].fs, FS3_SIZE},
        };
        const VkDescriptorImageInfo image = {
            samplers[i], textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet writes[4];
        for (size_t j = 0; j < 3; ++j) {
            writes[j] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i * 2], .dstBinding = ubo[j].binding,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &buffers[j],
            };
        }
        writes[3] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i * 2 + 1], .dstBinding = texture.binding,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image,
        };
        vkUpdateDescriptorSets(device, 4, writes, 0, NULL);
    }
    VkPipeline pipeline = create_pipeline(layout, depth_format, input);
    free(input);
    const VkExtent2D extent = {WIDTH, HEIGHT};
    Image color = create_image(extent, color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = create_image(extent, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth_aspects);
    const VkCommandPoolCreateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VK(vkCreateCommandPool(device, &commands, NULL, &command_pool));
    const VkCommandBufferAllocateInfo command = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK(vkAllocateCommandBuffers(device, &command, &cmd));
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK(vkCreateFence(device, &fence_info, NULL, &fence));
    begin(cmd);
    render(cmd, color, depth, pipeline, layout, sets, draws, buffer, vertices, readback);
    VK(vkEndCommandBuffer(cmd));
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    VK(vkQueueSubmit(queue, 1, &submit, fence));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_ns));
    if (!coherent)
        VK(vkInvalidateMappedMemoryRanges(device, 1, &range));
    check_readback(mapped + readback);
    report("PASS two exact indexed loading-indicator draws");
    vkDestroyFence(device, fence, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    for (size_t i = 0; i < 3; ++i)
        vkDestroyDescriptorSetLayout(device, layouts[i], NULL);
    for (size_t i = 0; i < DRAW_COUNT; ++i) {
        vkDestroySampler(device, samplers[i], NULL);
        destroy_image(textures[i]);
    }
    destroy_image(depth);
    vkUnmapMemory(device, backing);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, backing, NULL);

#ifdef __ORBIS__
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VK(pokopia_present_surface(instance, &surface));
    PokopiaPresent present = {0};
    VK(pokopia_present_open(&present, physical, device, family, queue, surface, color_format, extent,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    if (present.extent.width != WIDTH || present.extent.height != HEIGHT)
        fail("VideoOut extent differs from the captured render target", VK_ERROR_FORMAT_NOT_SUPPORTED);
    PokopiaGameCapture capture = {0};
    if (!pokopia_game_capture_init(&capture, POKOPIA_DEBUG_DIRECTORY))
        fail("debug capture directory", 0);
    preview(&present, &capture, physical, cmd, color.image);
    report("PASS VideoOut accepted loading replay frame 540 at 1920x1080");
    report("DEBUG READY loading replay screenshot");
    for (;;) {
        if (pokopia_game_capture_requested(&capture))
            preview(&present, &capture, physical, cmd, color.image);
        sceKernelUsleep(12000);
    }
#else
    vkDestroyCommandPool(device, command_pool, NULL);
    destroy_image(color);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
#endif
}
