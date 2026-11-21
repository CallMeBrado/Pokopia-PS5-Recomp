#include "live_draw.h"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#include "../gfx-bridge/game_capture.h"
#endif

#include "draw-data.h"

enum { WIDTH = 256, HEIGHT = 256, INDEX_COUNT = 6, PIXEL_BYTES = 4 };
static const VkFormat draw_format = VK_FORMAT_R16G16_SFLOAT;
static const uint64_t timeout_ns = UINT64_C(5000000000);
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;
static FILE *log_file;

typedef struct {
    VkBuffer handle;
    VkDeviceMemory backing;
    unsigned char *mapped;
    bool coherent;
} Buffer;

typedef struct {
    VkImage image;
    VkImageView view;
    VkDeviceMemory backing;
} Image;

static void report(const char *format, ...)
{
    char line[384];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    puts(line);
    fflush(stdout);
    if (log_file) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);
    }
}

static _Noreturn void fail(const char *operation, int result)
{
    report("FAIL %s (%d)", operation, result);
#ifdef __ORBIS__
    for (;;)
        sceKernelUsleep(1000000);
#else
    exit(EXIT_FAILURE);
#endif
}

static void checked(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS)
        fail(operation, result);
}

#define VK(call) checked((call), #call)

static VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, bool *coherent)
{
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (UINT32_C(1) << i)) || (flags & required) != required)
            continue;
        if (type == UINT32_MAX)
            type = i;
        if ((flags & preferred) == preferred) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX)
        fail("no compatible Vulkan memory type", VK_ERROR_FEATURE_NOT_PRESENT);
    if (coherent)
        *coherent = (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    const VkMemoryAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    VkDeviceMemory result = VK_NULL_HANDLE;
    VK(vkAllocateMemory(device, &info, NULL, &result));
    return result;
}

static Buffer make_buffer(VkDeviceSize size)
{
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    Buffer buffer = {0};
    VK(vkCreateBuffer(device, &info, NULL, &buffer.handle));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
    buffer.backing = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &buffer.coherent);
    VK(vkBindBufferMemory(device, buffer.handle, buffer.backing, 0));
    void *mapped = NULL;
    VK(vkMapMemory(device, buffer.backing, 0, VK_WHOLE_SIZE, 0, &mapped));
    buffer.mapped = mapped;
    return buffer;
}

static Image make_image(void)
{
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = draw_format, .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    Image result = {0};
    VK(vkCreateImage(device, &info, NULL, &result.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, result.image, &requirements);
    result.backing = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, NULL);
    VK(vkBindImageMemory(device, result.image, result.backing, 0));
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = info.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VK(vkCreateImageView(device, &view, NULL, &result.view));
    return result;
}

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    VK(vkCreateShaderModule(device, &info, NULL, &module));
    return module;
}

static VkPipeline make_pipeline(VkPipelineLayout layout)
{
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = shader(draw_vertex_shader, sizeof draw_vertex_shader), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = shader(draw_fragment_shader, sizeof draw_fragment_shader), .pName = "main"},
    };
    const VkVertexInputBindingDescription binding = {.binding = 0, .stride = 24};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 16},
    };
    const VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = attributes,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    /* The capture's upper-left NVN origin maps to a negative Vulkan height. */
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
    const VkPipelineColorBlendAttachmentState attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
    };
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &draw_format,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pColorBlendState = &blend, .layout = layout,
    };
    VkPipeline result = VK_NULL_HANDLE;
    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &result));
    for (size_t i = 0; i < 2; ++i)
        vkDestroyShaderModule(device, stages[i].module, NULL);
    return result;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old, VkImageLayout next,
                          VkPipelineStageFlags source, VkAccessFlags write,
                          VkPipelineStageFlags destination, VkAccessFlags access)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = old, .newLayout = next,
        .srcAccessMask = write, .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(cmd, source, destination, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void begin(VkCommandBuffer cmd)
{
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(vkBeginCommandBuffer(cmd, &info));
}

static void draw_first(VkCommandBuffer cmd, Image image, Buffer buffer, VkPipeline pipeline,
                       VkPipelineLayout layout, VkDescriptorSet descriptors, VkDeviceSize vertices,
                       VkDeviceSize indices, VkDeviceSize readback)
{
    image_barrier(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = image.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = {0, 0, 0, 1},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &color,
    };
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptors, 0, NULL);
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer.handle, &vertices);
    vkCmdBindIndexBuffer(cmd, buffer.handle, indices, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, INDEX_COUNT, 1, 0, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy copy = {
        .bufferOffset = readback, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1},
    };
    vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.handle, 1, &copy);
    const VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buffer.handle, .offset = readback,
        .size = WIDTH * HEIGHT * PIXEL_BYTES,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host, 0, NULL);
}

static float half_value(uint16_t value)
{
    const int exponent = (value >> 10) & 31;
    const int fraction = value & 1023;
    const float magnitude = exponent ? ldexpf((float)(1024 + fraction), exponent - 25)
                                     : ldexpf((float)fraction, -24);
    return value & 0x8000 ? -magnitude : magnitude;
}

static void verify_readback(const unsigned char *pixels)
{
    uint64_t fingerprint = UINT64_C(14695981039346656037);
    size_t changed = 0, nonzero = 0;
    float low[2] = {INFINITY, INFINITY}, high[2] = {-INFINITY, -INFINITY};
    for (size_t pixel = 0; pixel < WIDTH * HEIGHT; ++pixel) {
        const unsigned char *raw = pixels + pixel * PIXEL_BYTES;
        const uint16_t bits[2] = {(uint16_t)(raw[0] | raw[1] << 8), (uint16_t)(raw[2] | raw[3] << 8)};
        changed += memcmp(raw, pixels, PIXEL_BYTES) != 0;
        nonzero += ((bits[0] | bits[1]) & 0x7fff) != 0;
        for (size_t channel = 0; channel < 2; ++channel) {
            if ((bits[channel] & 0x7c00) == 0x7c00)
                fail("non-finite RG16F result", (int)pixel);
            const float value = half_value(bits[channel]);
            if (value < low[channel])
                low[channel] = value;
            if (value > high[channel])
                high[channel] = value;
        }
        for (size_t byte = 0; byte < PIXEL_BYTES; ++byte) {
            fingerprint ^= raw[byte];
            fingerprint *= UINT64_C(1099511628211);
        }
    }
    report("Draw 1 RG16F readback: %zu nonzero, %zu differ from first, R [%g, %g], G [%g, %g], FNV-1a %016" PRIx64,
           nonzero, changed, (double)low[0], (double)high[0], (double)low[1], (double)high[1], fingerprint);
    if (!nonzero || !changed)
        fail("expected nonuniform output from the captured indexed draw", 0);
}

static VkPhysicalDevice find_device(VkInstance instance, uint32_t *family)
{
    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count)
        fail("no Vulkan physical device", 0);
    VkPhysicalDevice *physical = calloc(count, sizeof *physical);
    if (!physical)
        fail("physical device list", VK_ERROR_OUT_OF_HOST_MEMORY);
    VK(vkEnumeratePhysicalDevices(instance, &count, physical));
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count && !selected; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3 || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i], &queue_count, NULL);
        if (!queue_count)
            continue;
        VkQueueFamilyProperties *queues = calloc(queue_count, sizeof *queues);
        if (!queues)
            fail("queue family list", VK_ERROR_OUT_OF_HOST_MEMORY);
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i], &queue_count, queues);
        for (uint32_t j = 0; j < queue_count; ++j) {
            if ((queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queues[j].queueCount) {
                selected = physical[i];
                *family = j;
                break;
            }
        }
        free(queues);
    }
    free(physical);
    if (!selected)
        fail("no Vulkan 1.3 graphics queue", VK_ERROR_FEATURE_NOT_PRESENT);
    return selected;
}

static VkDeviceSize aligned(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

#ifdef __ORBIS__
static void preview(PokopiaPresent *present, PokopiaGameCapture *capture, VkPhysicalDevice physical,
                    VkCommandBuffer cmd, VkImage offscreen)
{
    PokopiaPresentTarget target;
    VkResult result = pokopia_present_acquire(present, timeout_ns, &target);
    if (result != VK_SUBOPTIMAL_KHR)
        VK(result);
    VK(vkResetCommandBuffer(cmd, 0));
    begin(cmd);
    image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
    const VkImageSubresourceRange subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &subresource);
    image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const int32_t w = (int32_t)present->extent.width, h = (int32_t)present->extent.height;
    const int32_t side = w < h ? w : h;
    const int32_t x = (w - side) / 2, y = (h - side) / 2;
    const VkImageBlit blit = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {WIDTH, HEIGHT, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{x, y, 0}, {x + side, y + side, 1}},
    };
    vkCmdBlitImage(cmd, offscreen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

    VkResult captured = pokopia_game_capture_record(capture, physical, present, cmd,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, 1);
    if (captured != VK_SUCCESS) {
        if (captured != VK_NOT_READY)
            report("Capture record result %d", (int)captured);
        image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    }
    VK(vkEndCommandBuffer(cmd));
    result = pokopia_present_frame(present, cmd, timeout_ns);
    if (result != VK_SUBOPTIMAL_KHR)
        VK(result);
    if (captured == VK_SUCCESS) {
        VK(pokopia_game_capture_finish(capture, present, 1, result));
        report("Captured first-draw replay serial %" PRIu64 " request %016" PRIx64,
               capture->captured_render_serial, capture->captured_request_seq);
    }
}
#endif

int main(void)
{
#ifdef __ORBIS__
    log_file = fopen("/data/pokopia-ps4-first-draw.txt", "w");
#endif
    report("Pokopia First Draw Replay | update 655360 | VS 821538a4398f85a7 FS 7587f31f1945afaf");
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Pokopia First Draw Replay",
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
    VkPhysicalDevice physical = find_device(instance, &family);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    report("Vulkan device %s", props.deviceName);
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f13,
    };
    VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(physical, &supported);
    if (!f13.dynamicRendering || !f11.shaderDrawParameters || !supported.features.shaderFloat64 ||
        !supported.features.shaderImageGatherExtended)
        fail("Vulkan features declared by captured SPIR-V", VK_ERROR_FEATURE_NOT_PRESENT);
    f13 = (VkPhysicalDeviceVulkan13Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE,
    };
    f11 = (VkPhysicalDeviceVulkan11Features){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f13,
        .shaderDrawParameters = VK_TRUE,
    };
    const VkPhysicalDeviceFeatures features = {.shaderFloat64 = VK_TRUE, .shaderImageGatherExtended = VK_TRUE};
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

    PokopiaVkAdapter adapter = {
        .device = device, .queue = queue, .family = family, .memory = memory,
        .properties = props, .features = features, .get_device_proc = vkGetDeviceProcAddr,
        .wait_idle = vkDeviceWaitIdle, .create_command_pool = vkCreateCommandPool,
        .destroy_command_pool = vkDestroyCommandPool,
    };
    const Image output = make_image();
    PokopiaVkTexture target = {
        .backing.image = output.image, .view = output.view,
        .format = draw_format, .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
        .layout = VK_IMAGE_LAYOUT_UNDEFINED, .stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
    };
    const float clear[4] = {0, 0, 0, 1};
    const VkShaderModule vs = shader(draw_vertex_shader, sizeof draw_vertex_shader);
    const VkShaderModule fs = shader(draw_fragment_shader, sizeof draw_fragment_shader);
    PokopiaLiveDraw draw = {
        .target = &target, .extent = {WIDTH, HEIGHT},
        .vertex_shader = {vs, "main", "821538a4398f85a7"},
        .fragment_shader = {fs, "main", "7587f31f1945afaf"},
        .vertices = {draw_vertices, sizeof draw_vertices},
        .indices = {draw_indices, sizeof draw_indices},
        .vertex_uniform = {draw_vertex_ubo, sizeof draw_vertex_ubo},
        .fragment_uniform = {draw_fragment_ubo, sizeof draw_fragment_ubo},
        .vertex_stride = 24,
        .attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                       {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                       {2, 0, VK_FORMAT_R32G32_SFLOAT, 16}},
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .index_type = VK_INDEX_TYPE_UINT16,
        .viewport = {0, HEIGHT, WIDTH, -HEIGHT, 0, 1}, .scissor = {{0, 0}, {WIDTH, HEIGHT}},
        .color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
        .polygon_mode = VK_POLYGON_MODE_FILL, .cull_mode = VK_CULL_MODE_NONE,
        .index_count = INDEX_COUNT, .instance_count = 1,
    };
    uint64_t serial = UINT64_MAX;
    if (pokopia_live_draw(&adapter, &draw, &serial) != VK_ERROR_FORMAT_NOT_SUPPORTED ||
        serial != UINT64_MAX || adapter.render_serial != 0)
        fail("draw before initialized target", 0);
    VK(pokopia_vk_clear_color(&adapter, &target, clear));
    draw.vertices.bytes = draw.vertex_stride;
    if (pokopia_live_draw(&adapter, &draw, &serial) != VK_ERROR_FORMAT_NOT_SUPPORTED ||
        serial != UINT64_MAX || adapter.render_serial != 1 || target.render_serial != 1)
        fail("out-of-range guest index", 0);
    draw.vertices.bytes = sizeof draw_vertices;
    Buffer readback = make_buffer(WIDTH * HEIGHT * PIXEL_BYTES);
    unsigned char degenerate[sizeof draw_vertices] = {0};
    for (unsigned run = 0; run < 2; ++run) {
        if (run) {
            const float marker[4] = {0.5f, 0.25f, 0, 1};
            VK(pokopia_vk_clear_color(&adapter, &target, marker));
            draw.vertices.data = degenerate;
        }
        VK(pokopia_live_draw(&adapter, &draw, &serial));
        if (serial != 2 + run * 2 || target.render_serial != serial || adapter.render_serial != serial ||
            target.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            fail("completed render serial", 0);
        VkCommandBuffer cmd;
        VK(pokopia_vk_command_buffer_open(&adapter, &cmd));
        begin(cmd);
        image_barrier(cmd, output.image, target.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            target.stages, target.access, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferImageCopy copy = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {WIDTH, HEIGHT, 1},
        };
        vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
        const VkBufferMemoryBarrier host = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = readback.handle,
            .size = VK_WHOLE_SIZE,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &host, 0, NULL);
        VK(vkEndCommandBuffer(cmd));
        bool retired;
        VK(pokopia_vk_submit_wait(&adapter, cmd, &retired));
        pokopia_vk_command_buffer_close(&adapter, cmd);
        target.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        target.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        target.access = VK_ACCESS_TRANSFER_READ_BIT;
        const VkMappedMemoryRange mapped = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = readback.backing, .size = VK_WHOLE_SIZE,
        };
        if (!readback.coherent)
            VK(vkInvalidateMappedMemoryRanges(device, 1, &mapped));
        if (!run) {
            verify_readback(readback.mapped);
        } else {
            const unsigned char marker[] = {0x00, 0x38, 0x00, 0x34};
            for (size_t i = 0; i < WIDTH * HEIGHT; ++i)
                if (memcmp(readback.mapped + i * PIXEL_BYTES, marker, sizeof marker))
                    fail("changed vertex bytes should degenerate all triangles", (int)i);
            report("PASS replacing caller-provided vertices preserves every prior-clear pixel");
        }
    }
    report("PASS live_draw backend verified inputs, actual indexed submit, and retirement");
    vkDestroyShaderModule(device, fs, NULL);
    vkDestroyShaderModule(device, vs, NULL);
    vkUnmapMemory(device, readback.backing);
    vkDestroyBuffer(device, readback.handle, NULL);
    vkFreeMemory(device, readback.backing, NULL);
    vkDestroyCommandPool(device, adapter.guest_commands, NULL);
    vkDestroyImageView(device, output.view, NULL);
    vkDestroyImage(device, output.image, NULL);
    vkFreeMemory(device, output.backing, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return 0;
}
VkFormat nvn_guest_texture_vk_format(uint32_t format) { (void)format; abort(); }
