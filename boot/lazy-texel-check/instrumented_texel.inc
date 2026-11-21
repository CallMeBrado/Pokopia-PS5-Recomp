static PFN_vkGetDeviceProcAddr original_proc;
static unsigned buffers_created, buffers_destroyed, allocations, frees, views_created;
static bool fail_view;

static VKAPI_ATTR VkResult VKAPI_CALL count_buffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
    PFN_vkCreateBuffer create = (PFN_vkCreateBuffer)original_proc(device, "vkCreateBuffer");
    const VkResult result = create(device, info, allocator, buffer);
    buffers_created += result == VK_SUCCESS;
    return result;
}

static VKAPI_ATTR void VKAPI_CALL count_buffer_destroy(VkDevice device, VkBuffer buffer,
    const VkAllocationCallbacks *allocator)
{
    PFN_vkDestroyBuffer destroy = (PFN_vkDestroyBuffer)original_proc(device, "vkDestroyBuffer");
    destroy(device, buffer, allocator);
    ++buffers_destroyed;
}

static VKAPI_ATTR VkResult VKAPI_CALL count_alloc(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory)
{
    PFN_vkAllocateMemory allocate = (PFN_vkAllocateMemory)original_proc(device, "vkAllocateMemory");
    const VkResult result = allocate(device, info, allocator, memory);
    allocations += result == VK_SUCCESS;
    return result;
}

static VKAPI_ATTR void VKAPI_CALL count_free(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator)
{
    PFN_vkFreeMemory free_memory = (PFN_vkFreeMemory)original_proc(device, "vkFreeMemory");
    free_memory(device, memory, allocator);
    ++frees;
}

static VKAPI_ATTR VkResult VKAPI_CALL count_view(VkDevice device, const VkBufferViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBufferView *view)
{
    if (fail_view)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    PFN_vkCreateBufferView create = (PFN_vkCreateBufferView)original_proc(device, "vkCreateBufferView");
    const VkResult result = create(device, info, allocator, view);
    views_created += result == VK_SUCCESS;
    return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL count_proc(VkDevice device, const char *name)
{
    if (strcmp(name, "vkCreateBuffer") == 0)
        return (PFN_vkVoidFunction)count_buffer;
    if (strcmp(name, "vkDestroyBuffer") == 0)
        return (PFN_vkVoidFunction)count_buffer_destroy;
    if (strcmp(name, "vkAllocateMemory") == 0)
        return (PFN_vkVoidFunction)count_alloc;
    if (strcmp(name, "vkFreeMemory") == 0)
        return (PFN_vkVoidFunction)count_free;
    if (strcmp(name, "vkCreateBufferView") == 0)
        return (PFN_vkVoidFunction)count_view;
    return original_proc(device, name);
}

static void assert_unbacked(const PokopiaVkTexel *texel)
{
    assert(!texel->buffer && !texel->view && !texel->memory && !texel->mapped);
}

static void open_texel(PokopiaVkAdapter *adapter, PokopiaVkTexel *out, uint32_t count)
{
    VkFormatProperties caps;
    const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    adapter->format_properties(adapter->physical, format, &caps);
    original_proc = adapter->get_device_proc;
    const uint32_t limit = adapter->properties.limits.maxTexelBufferElements;
    const unsigned prior_buffers = buffers_created, prior_allocs = allocations;
    const unsigned prior_destroyed = buffers_destroyed, prior_frees = frees, prior_views = views_created;
    const size_t size = (size_t)count * 16;
    void *zeros = calloc(count, 16);
    assert(zeros);
    if (count == 1024) {
        assert(pokopia_vk_texel_configure(out, adapter->device, count_proc, &adapter->memory,
            0, format, count, 16, limit) == VK_ERROR_FORMAT_NOT_SUPPORTED);
        CHECK(pokopia_vk_texel_configure(out, adapter->device, count_proc, &adapter->memory,
            caps.bufferFeatures, format, count, 16, limit));
        assert(out->device == adapter->device && out->bytes == size && out->format == format);
        assert_unbacked(out);
        assert(allocations == prior_allocs && buffers_created == prior_buffers && views_created == prior_views);
        pokopia_vk_texel_close(out);
        pokopia_vk_texel_close(out);
        assert(!out->device && allocations == prior_allocs && frees == prior_frees &&
               buffers_created == prior_buffers && buffers_destroyed == prior_destroyed);
        CHECK(pokopia_vk_texel_configure(out, adapter->device, count_proc, &adapter->memory,
            caps.bufferFeatures, format, count, 16, limit));
        assert(pokopia_vk_texel_configure(out, adapter->device, count_proc, &adapter->memory,
            caps.bufferFeatures, format, count, 16, limit) == VK_ERROR_INITIALIZATION_FAILED);
        assert(pokopia_vk_texel_refresh(out, zeros, size - 1) == VK_ERROR_INITIALIZATION_FAILED);
        assert(allocations == prior_allocs && buffers_created == prior_buffers);
        fail_view = true;
        assert(pokopia_vk_texel_refresh(out, zeros, size) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fail_view = false;
        assert_unbacked(out);
        assert(out->device == adapter->device && out->bytes == size && out->format == format);
        assert(allocations == prior_allocs + 1 && frees == prior_frees + 1 &&
               buffers_created == prior_buffers + 1 && buffers_destroyed == prior_destroyed + 1 &&
               views_created == prior_views);
        CHECK(pokopia_vk_texel_refresh(out, zeros, size));
        assert(allocations == prior_allocs + 2 && views_created == prior_views + 1);
        const VkDeviceMemory stable_memory = out->memory;
        const VkBufferView stable_view = out->view;
        CHECK(pokopia_vk_texel_refresh(out, zeros, size));
        assert(out->memory == stable_memory && out->view == stable_view);
        assert(allocations == prior_allocs + 2 && views_created == prior_views + 1);
        puts("PASS configured texel allocates nothing until use, validates sizes, rolls back and retries");
    } else {
        fail_view = true;
        assert(pokopia_vk_texel_open(out, adapter->device, count_proc, &adapter->memory,
            caps.bufferFeatures, format, count, 16, limit) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        fail_view = false;
        assert_unbacked(out);
        assert(!out->device && allocations == prior_allocs + 1 && frees == prior_frees + 1);
        CHECK(pokopia_vk_texel_open(out, adapter->device, count_proc, &adapter->memory,
            caps.bufferFeatures, format, count, 16, limit));
        assert(out->buffer && out->memory && out->view && out->mapped);
        assert(allocations == prior_allocs + 2 && views_created == prior_views + 1);
        CHECK(pokopia_vk_texel_refresh(out, zeros, size));
        assert(allocations == prior_allocs + 2);
        puts("PASS legacy eager open still materializes a Vulkan view and clears a failed object");
    }
    free(zeros);
}
