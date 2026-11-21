from pathlib import Path

def edit(name, pairs):
    p=Path(name)
    s=p.read_text()
    for a,b in pairs:
        if s.count(a)!=1:
            raise ValueError((name,a,s.count(a)))
        s=s.replace(a,b)
    p.write_text(s)

edit('vk_adapter.h', [
    ('    VkFormat format;\n', '    VkFormat format;\n    VkExtent2D extent;\n    uint32_t array_layers;\n'),
    ('void pokopia_vk_close(', '''VkResult pokopia_vk_clear_depth_stencil(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, VkImageAspectFlags aspects, VkClearDepthStencilValue value);
void pokopia_vk_close('''),
])
edit('live_draw.h', [
    ('    PokopiaVkTexture *target;\n', '    PokopiaVkTexture *target, *depth_target;\n    uint32_t target_layer;\n'),
    ('against a 2D mip-0 target.', 'against a single mip-0 layer.'),
])
edit('nvn_replay.h', [
    ('    bool (*draw)', '''    bool (*clear_depth_stencil)(void *opaque, void *texture, VkRect2D area,
                                VkImageAspectFlags aspects, VkClearDepthStencilValue value);
    bool (*draw)'''),
])
edit('vk_adapter.c', [
    ('        .format = format, .aspects = depth ? depth : VK_IMAGE_ASPECT_COLOR_BIT,\n', '''        .format = format, .aspects = depth ? depth : VK_IMAGE_ASPECT_COLOR_BIT,
        .extent = {image.extent.width, image.extent.height}, .array_layers = image.arrayLayers,
'''),
    ('static PFN_vkGetInstanceProcAddr open_loader', '''VkResult pokopia_vk_clear_depth_stencil(PokopiaVkAdapter *adapter, PokopiaVkTexture *texture,
                                      VkRect2D area, VkImageAspectFlags aspects, VkClearDepthStencilValue value)
{
    const VkImageAspectFlags depth_stencil = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!texture || !texture->backing.image || !texture->view || !texture->stages ||
        texture->aspects != depth_stencil || texture->array_layers != 1 || !aspects || (aspects & ~depth_stencil) ||
        area.offset.x < 0 || area.offset.y < 0 || !area.extent.width || !area.extent.height ||
        (uint64_t)area.offset.x + area.extent.width > texture->extent.width ||
        (uint64_t)area.offset.y + area.extent.height > texture->extent.height ||
        ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && !(value.depth >= 0 && value.depth <= 1)) ||
        value.stencil > 0xff)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    const VkDevice device = adapter->device;
    PFN_vkGetDeviceProcAddr proc = adapter->get_device_proc;
    PFN_vkBeginCommandBuffer begin = (PFN_vkBeginCommandBuffer)proc(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer end = (PFN_vkEndCommandBuffer)proc(device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier barrier = (PFN_vkCmdPipelineBarrier)proc(device, "vkCmdPipelineBarrier");
    PFN_vkCmdBeginRendering begin_rendering = (PFN_vkCmdBeginRendering)proc(device, "vkCmdBeginRendering");
    PFN_vkCmdEndRendering end_rendering = (PFN_vkCmdEndRendering)proc(device, "vkCmdEndRendering");
    if (!begin || !end || !barrier || !begin_rendering || !end_rendering)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkCommandBuffer command;
    VkResult result = pokopia_vk_command_buffer_open(adapter, &command);
    if (result != VK_SUCCESS)
        return result;
    const VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkImageMemoryBarrier transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = texture->access,
        .dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = texture->layout, .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = texture->backing.image, .subresourceRange = {depth_stencil, 0, 1, 0, 1},
    };
    result = begin(command, &info);
    if (result == VK_SUCCESS) {
        barrier(command, texture->stages, stages, 0, 0, NULL, 0, NULL, 1, &transition);
        const VkAttachmentLoadOp preserve = texture->layout == VK_IMAGE_LAYOUT_UNDEFINED ?
            VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        VkRenderingAttachmentInfo depth = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = texture->view,
            .imageLayout = transition.newLayout, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .loadOp = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_ATTACHMENT_LOAD_OP_CLEAR : preserve,
            .clearValue = {.depthStencil = value},
        };
        VkRenderingAttachmentInfo stencil = depth;
        stencil.loadOp = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_ATTACHMENT_LOAD_OP_CLEAR : preserve;
        const VkRenderingInfo rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1,
            .pDepthAttachment = &depth, .pStencilAttachment = &stencil,
        };
        begin_rendering(command, &rendering);
        end_rendering(command);
        result = end(command);
    }
    bool retired = true;
    if (result == VK_SUCCESS)
        result = pokopia_vk_submit_wait(adapter, command, &retired);
    if (retired)
        pokopia_vk_command_buffer_close(adapter, command);
    if (result == VK_SUCCESS) {
        texture->layout = transition.newLayout;
        texture->stages = stages;
        texture->access = transition.dstAccessMask;
    }
    return result;
}

static PFN_vkGetInstanceProcAddr open_loader'''),
])
