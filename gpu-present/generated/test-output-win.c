#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "present.h"

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

#include "fixture-data.h"

enum { WIDTH = 32, HEIGHT = 32, PIXEL_BYTES = WIDTH * HEIGHT * 4 };
enum { BANK_STRIDE = 65536, UPLOAD = 4 * BANK_STRIDE, READBACK = UPLOAD + PIXEL_BYTES };
enum { SAMPLE_COUNT = 5, SAMPLE_STRIDE = 256, BUFFER_BYTES = READBACK + SAMPLE_COUNT * SAMPLE_STRIDE };
static const uint64_t timeout_ns = UINT64_C(5000000000);
static FILE *log_file;
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;

static void report(const char *fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    puts(line);
    fflush(stdout);
    if (log_file) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);
    }
}

static void finish(int status)
{
    if (log_file)
        fclose(log_file);
#ifdef __ORBIS__
    (void)status;
    for (;;)
        sceKernelUsleep(1000000);
#else
    exit(status);
#endif
}

static void need(int condition, const char *message)
{
    if (!condition) {
        report("FAIL %s", message);
        finish(1);
    }
}

static void checked(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS) {
        report("FAIL %s: VkResult %d", operation, (int)result);
        finish(1);
    }
}

#define VK(call) checked((call), #call)

static VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & flags) == flags) {
            VkMemoryAllocateInfo info = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex = i,
            };
            VkDeviceMemory result;
            VK(vkAllocateMemory(device, &info, NULL, &result));
            return result;
        }
    }
    report("FAIL no memory type for flags 0x%x, bits 0x%x", flags, requirements.memoryTypeBits);
    finish(1);
    return VK_NULL_HANDLE;
}

typedef struct {
    VkImage image;
    VkImageView view;
} Image;

static Image make_image(VkImageUsageFlags usage, VkExtent2D extent)
{
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    Image result;
    VK(vkCreateImage(device, &info, NULL, &result.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, result.image, &requirements);
    VkDeviceMemory backing = allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(vkBindImageMemory(device, result.image, backing, 0));

    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = info.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    VK(vkCreateImageView(device, &view, NULL, &result.view));
    return result;
}

static VkShaderModule shader(const uint32_t *words, size_t size)
{
    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = words,
    };
    VkShaderModule result;
    VK(vkCreateShaderModule(device, &info, NULL, &result));
    return result;
}

static VkDescriptorSetLayout descriptor_layout(const VkDescriptorSetLayoutBinding *bindings, uint32_t count)
{
    const VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout result;
    VK(vkCreateDescriptorSetLayout(device, &info, NULL, &result));
    return result;
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                          VkImageLayout new_layout, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst)
{
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage, .srcAccessMask = src,
        .dstStageMask = dst_stage, .dstAccessMask = dst,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

static VkPipeline make_pipeline(VkPipelineLayout layout, VkExtent2D extent, VkFormat format)
{
    const VkPipelineShaderStageCreateInfo stages[] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shader(composite_vert, sizeof(composite_vert)), .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = shader(composite_frag, sizeof(composite_frag)), .pName = "main" },
    };
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkViewport vp = {0, 0, (float)extent.width, (float)extent.height, 0, 1};
    const VkRect2D scissor = {{0, 0}, extent};
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount = 1, .pScissors = &scissor,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
    };
    const VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &format,
    };
    const VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &input,
        .pInputAssemblyState = &assembly, .pViewportState = &viewport,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = layout,
    };
    VkPipeline result;
    VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &result));
    vkDestroyShaderModule(device, stages[0].module, NULL);
    vkDestroyShaderModule(device, stages[1].module, NULL);
    return result;
}

static void fill_fixture(uint8_t *data, VkExtent2D extent)
{
    memset(data, 0, BUFFER_BYTES);
    float *support = (float *)data;
    support[36] = 2.0f / extent.width;
    support[37] = 2.0f / extent.height;
    support[38] = 1.0f;
    support[40] = (float)extent.width;
    support[41] = (float)extent.height;
    support[42] = 1.0f;
    for (unsigned i = 0; i < 73; ++i)
        support[48 + 4 * i] = 1.0f;

    memcpy(data + BANK_STRIDE, vertex_c1, sizeof(vertex_c1));
    float *fragment_c5 = (float *)(data + 3 * BANK_STRIDE);
    /* c5[6].w and c5[9].x zero disable spatial effects. These two ones disable gamma
       correction and keep RGB unscaled. The composite then copies each texel. */
    fragment_c5[7 * 4 + 1] = 1.0f;
    fragment_c5[8 * 4 + 3] = 1.0f;

    for (unsigned y = 0; y < HEIGHT; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            uint8_t *pixel = data + UPLOAD + 4 * (y * WIDTH + x);
            pixel[0] = (uint8_t)(3 + 7 * x);
            pixel[1] = (uint8_t)(5 + 5 * y);
            pixel[2] = (uint8_t)(13 + (x / 4 + y / 8) % 2 * 179);
            pixel[3] = 255;
        }
    }
}

static void upload_texture(VkCommandBuffer cmd, VkBuffer buffer, VkImage image)
{
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy = {
        .bufferOffset = UPLOAD, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {WIDTH, HEIGHT, 1},
    };
    vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

static void draw_composite(VkCommandBuffer cmd, PokopiaPresentTarget target, VkExtent2D extent,
                           VkPipeline pipeline, VkPipelineLayout layout, const VkDescriptorSet sets[3])
{
    image_barrier(cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  0, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target.view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color.float32 = {1.0f, 0.0f, 1.0f, 1.0f}},
    };
    const VkRenderingInfo render = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, extent},
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment,
    };
    vkCmdBeginRendering(cmd, &render);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

static void copy_samples(VkCommandBuffer cmd, VkImage image, VkBuffer buffer,
                         const VkOffset3D points[SAMPLE_COUNT])
{
    image_barrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[SAMPLE_COUNT];
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
        copies[i] = (VkBufferImageCopy){
            .bufferOffset = READBACK + i * SAMPLE_STRIDE,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = points[i], .imageExtent = {1, 1, 1},
        };
    }
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, SAMPLE_COUNT, copies);

    const VkMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);
}

static void check_samples(const uint8_t *mapped, VkExtent2D extent, const VkOffset3D points[SAMPLE_COUNT])
{
    bool matches = true;
    for (unsigned i = 0; i < SAMPLE_COUNT; ++i) {
        unsigned x = (2 * (unsigned)points[i].x + 1) * WIDTH / (2 * extent.width);
        unsigned y = (2 * (unsigned)points[i].y + 1) * HEIGHT / (2 * extent.height);
        const uint8_t *expected = mapped + UPLOAD + 4 * (y * WIDTH + x);
        const uint8_t *actual = mapped + READBACK + i * SAMPLE_STRIDE;
        report("swapchain sample %d,%d: RGBA %u,%u,%u,%u expected %u,%u,%u,%u",
               points[i].x, points[i].y, actual[0], actual[1], actual[2], actual[3],
               expected[0], expected[1], expected[2], expected[3]);
        if (memcmp(actual, expected, 4) != 0)
            matches = false;
    }
    need(matches, "swapchain samples differ from the composite input");
}

int main(void)
{
#ifdef __ORBIS__
    log_file = fopen("/data/pokopia-ps4-present-probe.txt", "w");
#endif
    report("Pokopia composite fixture: Vulkan to PS4 VideoOut");
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Pokopia PS4 GPU Probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
    };
    VkInstance instance;
    VK(vkCreateInstance(&instance_info, NULL, &instance));


    VkPhysicalDevice physical;
    uint32_t count = 1;
    const VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count, &physical);
    need((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) && count,
         "no Vulkan physical device");
    VkPhysicalDeviceVulkan11Properties p11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11,
    };
    vkGetPhysicalDeviceProperties2(physical, &props);
    const uint32_t api = props.properties.apiVersion;
    report("GPU %s, Vulkan %u.%u.%u, subgroup %u",
           props.properties.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
           VK_API_VERSION_PATCH(api), p11.subgroupSize);
    need(api >= VK_API_VERSION_1_3 && props.properties.limits.minUniformBufferOffsetAlignment <= BANK_STRIDE,
         "Vulkan 1.3 or fixture uniform alignment unavailable");

    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f13,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11,
    };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    need(f13.dynamicRendering && f13.synchronization2, "dynamic rendering or synchronization2 unavailable");
    need(f11.shaderDrawParameters && features.features.shaderFloat64 &&
         features.features.shaderImageGatherExtended, "declared SPIR-V capabilities unavailable");
    const VkPhysicalDeviceFeatures enabled = {
        .shaderFloat64 = VK_TRUE, .shaderImageGatherExtended = VK_TRUE,
    };
    const VkPhysicalDeviceVulkan13Features enable13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
    };
    const VkPhysicalDeviceVulkan11Features enable11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = (void *)&enable13, .shaderDrawParameters = VK_TRUE,
    };

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    need(families != NULL, "queue family allocation failed");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    free(families);
    need(family < count, "no combined graphics and presentation queue");
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable11,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info, .pEnabledFeatures = &enabled,
    };
    VK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    report("graphics device ready");
    const PokopiaPresent present = {.extent = {1920, 1080}, .format = VK_FORMAT_R8G8B8A8_UNORM};

    const VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUFFER_BYTES,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    VK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkDeviceMemory backing = allocate(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkBindBufferMemory(device, buffer, backing, 0));
    void *mapped;
    VK(vkMapMemory(device, backing, 0, VK_WHOLE_SIZE, 0, &mapped));
    fill_fixture(mapped, present.extent);

    const Image input = make_image(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, (VkExtent2D){WIDTH, HEIGHT});
    const VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkSampler sampler;
    VK(vkCreateSampler(device, &sampler_info, NULL, &sampler));

    const VkDescriptorSetLayoutBinding banks[] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {24, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    const VkDescriptorSetLayoutBinding texture = {
        128, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL,
    };
    VkDescriptorSetLayout layouts[] = {
        descriptor_layout(banks, 4), descriptor_layout(NULL, 0), descriptor_layout(&texture, 1),
    };
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
    };
    const VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 3, .poolSizeCount = 2, .pPoolSizes = pool_sizes,
    };
    VkDescriptorPool pool;
    VK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    const VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 3, .pSetLayouts = layouts,
    };
    VkDescriptorSet sets[3];
    VK(vkAllocateDescriptorSets(device, &set_info, sets));
    VkDescriptorBufferInfo bank_info[4];
    VkWriteDescriptorSet writes[5];
    for (unsigned i = 0; i < 4; ++i) {
        bank_info[i] = (VkDescriptorBufferInfo){buffer, i * BANK_STRIDE, BANK_STRIDE};
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0],
            .dstBinding = banks[i].binding, .descriptorCount = 1, .descriptorType = banks[i].descriptorType,
            .pBufferInfo = &bank_info[i],
        };
    }
    const VkDescriptorImageInfo texture_info = {sampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[4] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2],
        .dstBinding = texture.binding, .descriptorCount = 1, .descriptorType = texture.descriptorType,
        .pImageInfo = &texture_info,
    };
    vkUpdateDescriptorSets(device, 5, writes, 0, NULL);

    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3, .pSetLayouts = layouts,
    };
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &layout));
    const VkPipeline pipeline = make_pipeline(layout, present.extent, present.format);
    report("Pokopia composite shaders compiled on GPU");

    const VkCommandPoolCreateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool command_pool;
    VK(vkCreateCommandPool(device, &commands, NULL, &command_pool));
    const VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(device, &command_info, &cmd));
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    const int32_t w = (int32_t)present.extent.width;
    const int32_t h = (int32_t)present.extent.height;
    const VkOffset3D points[SAMPLE_COUNT] = {
        {1, 1, 0}, {w - 2, 1, 0}, {1, h - 2, 0}, {w - 2, h - 2, 0}, {w / 2 + 3, h / 2 + 3, 0},
    };
    need(w >= 32 && h >= 32, "swapchain extent too small for the fixture");
    const Image output = make_image(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    present.extent);
    const PokopiaPresentTarget target = {output.image, output.view};
    VK(vkBeginCommandBuffer(cmd, &begin));
    upload_texture(cmd, buffer, input.image);
    draw_composite(cmd, target, present.extent, pipeline, layout, sets);
    copy_samples(cmd, output.image, buffer, points);
    VK(vkEndCommandBuffer(cmd));
    VkFence fence;
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device, &fence_info, NULL, &fence));
    const VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    VK(vkQueueSubmit(queue, 1, &submit, fence));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, timeout_ns));
    check_samples(mapped, present.extent, points);
    report("PASS Pokopia composite at 1920x1080 on Windows, five exact offscreen samples");
    return 0;
}
