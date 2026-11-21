#include "nvn-staging.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

enum { POOL_BYTES = 0x1000, COPY_BYTES = 0x400, SOURCE_OFFSET = 0x210, DESTINATION_OFFSET = 0x7f0 };
static const uint64_t source_address = UINT64_C(0x4400100000);
static const uint64_t destination_address = UINT64_C(0x4400200000);
static FILE *log_file;

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

static void need(bool condition, const char *message)
{
    if (!condition) {
        report("FAIL %s", message);
        finish(1);
    }
}

static void expect(VkResult observed, VkResult expected, const char *operation)
{
    if (observed != expected) {
        report("FAIL %s: VkResult %d, expected %d", operation, (int)observed, (int)expected);
        finish(1);
    }
}

#define VK(call) expect((call), VK_SUCCESS, #call)

static void check_bytes(const uint8_t *bytes, uint8_t fill, size_t size, const char *message)
{
    for (size_t i = 0; i < size; ++i)
        need(bytes[i] == fill, message);
}

static void run_copy(NvnStaging *staging, const NvnStagedCopy *copy, unsigned sequence, uint8_t sentinel)
{
    NvnStagedPool *source = copy->source;
    NvnStagedPool *destination = copy->destination;
    uint8_t expected[COPY_BYTES];
    for (size_t i = 0; i < COPY_BYTES; ++i)
        expected[i] = (uint8_t)((i * 29u + sequence * 73u) ^ (i >> 3));
    memcpy(source->guest_data + SOURCE_OFFSET, expected, sizeof(expected));

    /* Submission reads current guest bytes after the command has been recorded. */
    VK(nvn_staging_submit(staging, copy));
    expect(nvn_staging_invalidate(staging, destination, destination_address + DESTINATION_OFFSET, COPY_BYTES),
           VK_NOT_READY, "invalidate before waiting");
    expect(nvn_staging_pool_close(staging, destination), VK_NOT_READY, "destroy while destination is pending");
    expect(nvn_staging_submit(staging, copy), VK_NOT_READY, "submit while a destination is pending");
    VK(nvn_staging_wait(staging, UINT64_C(5000000000)));
    check_bytes(destination->guest_data, sentinel, POOL_BYTES, "guest destination changed before invalidate");
    expect(nvn_staging_invalidate(staging, destination, destination_address + DESTINATION_OFFSET, COPY_BYTES - 1),
           VK_ERROR_INITIALIZATION_FAILED, "invalidate a range other than the completed copy");
    VK(nvn_staging_invalidate(staging, destination, destination_address + DESTINATION_OFFSET, COPY_BYTES));

    need(!memcmp(destination->guest_data + DESTINATION_OFFSET, expected, sizeof(expected)),
         "destination does not match the submitted bytes");
    check_bytes(destination->guest_data, sentinel, DESTINATION_OFFSET, "destination prefix changed");
    check_bytes(destination->guest_data + DESTINATION_OFFSET + COPY_BYTES, sentinel,
                POOL_BYTES - DESTINATION_OFFSET - COPY_BYTES, "destination suffix changed");
    need(!memcmp(source->guest_data + SOURCE_OFFSET, expected, sizeof(expected)), "source data changed");
    check_bytes(source->guest_data, 0x91, SOURCE_OFFSET, "source prefix changed");
    check_bytes(source->guest_data + SOURCE_OFFSET + COPY_BYTES, 0x91,
                POOL_BYTES - SOURCE_OFFSET - COPY_BYTES, "source suffix changed");

    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < COPY_BYTES; ++i)
        hash = (hash ^ destination->guest_data[DESTINATION_OFFSET + i]) * UINT32_C(16777619);
    report("copy %u: %u bytes from +%03x to +%03x, FNV-1a=%08x, sentinels preserved",
           sequence, COPY_BYTES, SOURCE_OFFSET, DESTINATION_OFFSET, hash);
}

static void run_pool_fixture(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family)
{
    uint8_t source_bytes[POOL_BYTES], old_destination[POOL_BYTES], new_destination[POOL_BYTES];
    uint8_t saved_destination[POOL_BYTES];
    memset(source_bytes, 0x91, sizeof(source_bytes));
    memset(old_destination, 0xc3, sizeof(old_destination));
    memset(new_destination, 0x53, sizeof(new_destination));

    NvnStaging staging = {0};
    NvnStagedPool source = {0}, destination = {0};
    VK(nvn_staging_open(&staging, physical, device, queue, family, POOL_BYTES));
    VK(nvn_staging_pool_open(&staging, &source, source_address, source_bytes, POOL_BYTES));
    VK(nvn_staging_pool_open(&staging, &destination, destination_address, old_destination, POOL_BYTES));
    report("two distinct guest pools resolved to Vulkan-owned coherent mapped buffers");
    expect(nvn_staging_close(&staging), VK_NOT_READY, "close context with registered pools");

    NvnStagedCopy copy = {0};
    expect(nvn_staging_record_copy(&copy, &source, source_address + POOL_BYTES - 4, &destination,
                                   destination_address + DESTINATION_OFFSET, COPY_BYTES),
           VK_ERROR_INITIALIZATION_FAILED, "record an out-of-range source");
    VK(nvn_staging_record_copy(&copy, &source, source_address + SOURCE_OFFSET, &destination,
                               destination_address + DESTINATION_OFFSET, COPY_BYTES));
    run_copy(&staging, &copy, 1, 0xc3);
    memcpy(saved_destination, old_destination, sizeof(saved_destination));

    VK(nvn_staging_pool_close(&staging, &destination));
    VK(nvn_staging_pool_open(&staging, &destination, destination_address, new_destination, POOL_BYTES));
    expect(nvn_staging_submit(&staging, &copy), VK_ERROR_INITIALIZATION_FAILED,
           "submit a command recorded for a finalized pool");
    report("stale command rejected after re-registering the same guest address");
    VK(nvn_staging_record_copy(&copy, &source, source_address + SOURCE_OFFSET, &destination,
                               destination_address + DESTINATION_OFFSET, COPY_BYTES));
    run_copy(&staging, &copy, 2, 0x53);
    need(!memcmp(old_destination, saved_destination, sizeof(saved_destination)),
         "copy after reinitialization wrote into finalized guest storage");

    VK(nvn_staging_pool_close(&staging, &destination));
    VK(nvn_staging_pool_close(&staging, &source));
    VK(nvn_staging_close(&staging));
}

int main(void)
{
#ifdef __ORBIS__
    log_file = fopen("/data/pokopia-ps4-pool-probe.txt", "w");
#endif
    report("Pokopia NVN buffer-pool fixture: Vulkan staging and guest readback");
    const VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Pokopia PS4 Pool Probe",
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
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    const uint32_t api = properties.apiVersion;
    report("GPU %s, Vulkan %u.%u.%u", properties.deviceName,
           VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    need(families != NULL, "queue family allocation failed");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        ++family;
    free(families);
    need(family < count, "no graphics queue");

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    VK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    run_pool_fixture(physical, device, queue, family);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    report("PASS NVN pool staging, range sentinels, and reinitialization");
    finish(0);
    return 0;
}
