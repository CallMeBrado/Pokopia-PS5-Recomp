/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vk_ledger.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

enum { CAPACITY = 8192, MEMORY_TYPES = VK_MAX_MEMORY_TYPES };
typedef struct {
    VkDevice device;
    VkDeviceMemory memory;
    uint64_t bytes;
    uint32_t type;
    PokopiaVkLedgerClass category;
    bool used;
} Entry;

typedef struct {
    uint64_t live, peak, allocated, released;
    uint64_t live_count, alloc_count, free_count, failed_count;
} Count;

static atomic_flag ledger_lock = ATOMIC_FLAG_INIT;
static Entry entries[CAPACITY];
static Count all, classes[VK_LEDGER_CLASS_COUNT], types[MEMORY_TYPES];
static uint64_t untracked_allocations, untracked_releases;
static uint32_t wsi_images, wsi_peak;
static VkExtent2D wsi_extent;
static VkFormat wsi_format;

static void lock(void)
{
    while (atomic_flag_test_and_set_explicit(&ledger_lock, memory_order_acquire)) {}
}

static void unlock(void)
{
    atomic_flag_clear_explicit(&ledger_lock, memory_order_release);
}

static Entry *find(VkDevice device, VkDeviceMemory memory, bool insert)
{
    /* Vulkan's 64-bit non-dispatchable handles may be aligned pointers. */
    uint64_t key = (uint64_t)(uintptr_t)device ^ (uint64_t)(uintptr_t)memory;
    key = (key ^ (key >> 16)) * UINT64_C(0x9e3779b97f4a7c15);
    size_t index = (size_t)(key >> 51);
    Entry *free_slot = NULL;
    for (size_t probe = 0; probe < CAPACITY; ++probe) {
        Entry *entry = &entries[(index + probe) % CAPACITY];
        if (entry->memory == memory && entry->device == device)
            return entry;
        if (!entry->memory && !free_slot)
            free_slot = entry;
        if (!entry->used)
            break;
    }
    return insert ? free_slot : NULL;
}

static void account_allocate(Count *count, uint64_t bytes)
{
    count->allocated += bytes;
    count->live += bytes;
    ++count->alloc_count;
    ++count->live_count;
    if (count->peak < count->live)
        count->peak = count->live;
}

static void account_free(Count *count, uint64_t bytes)
{
    count->released += bytes;
    count->live -= bytes;
    ++count->free_count;
    --count->live_count;
}

VkResult pokopia_vk_ledger_allocate(PFN_vkAllocateMemory allocate, VkDevice device,
                                  const VkMemoryAllocateInfo *request, VkDeviceMemory *memory,
                                  PokopiaVkLedgerClass category)
{
    const VkResult result = allocate(device, request, NULL, memory);
    lock();
    Count *type = request->memoryTypeIndex < MEMORY_TYPES ? &types[request->memoryTypeIndex] : NULL;
    if (result == VK_SUCCESS) {
        Entry *entry = *memory ? find(device, *memory, true) : NULL;
        if (!entry || entry->memory) {
            ++untracked_allocations;
        } else {
            *entry = (Entry){.device = device, .memory = *memory, .bytes = request->allocationSize,
                            .type = request->memoryTypeIndex, .category = category, .used = true};
            account_allocate(&all, entry->bytes);
            account_allocate(&classes[category], entry->bytes);
            if (type)
                account_allocate(type, entry->bytes);
        }
    } else {
        ++all.failed_count;
        ++classes[category].failed_count;
        if (type)
            ++type->failed_count;
    }
    unlock();
    return result;
}

void pokopia_vk_ledger_free(PFN_vkFreeMemory release, VkDevice device, VkDeviceMemory memory)
{
    if (memory) {
        lock();
        Entry *entry = find(device, memory, false);
        if (entry) {
            account_free(&all, entry->bytes);
            account_free(&classes[entry->category], entry->bytes);
            if (entry->type < MEMORY_TYPES)
                account_free(&types[entry->type], entry->bytes);
            *entry = (Entry){.used = true};
        } else {
            ++untracked_releases;
        }
        unlock();
    }
    release(device, memory, NULL);
}

void pokopia_vk_ledger_swapchain_add(uint32_t images, VkExtent2D extent, VkFormat format)
{
    lock();
    wsi_images += images;
    if (wsi_peak < wsi_images)
        wsi_peak = wsi_images;
    wsi_extent = extent;
    wsi_format = format;
    unlock();
}

void pokopia_vk_ledger_swapchain_remove(uint32_t images)
{
    lock();
    wsi_images -= images;
    unlock();
}

static void report_count(FILE *log, const char *label, const Count *count)
{
    fprintf(log, "  %s live=%" PRIu64 " peak=%" PRIu64 " allocated=%" PRIu64 " released=%" PRIu64
                 " objects=%" PRIu64 " allocs=%" PRIu64 " frees=%" PRIu64 " failed=%" PRIu64 "\n",
            label, count->live, count->peak, count->allocated, count->released,
            count->live_count, count->alloc_count, count->free_count, count->failed_count);
}

void pokopia_vk_ledger_report(FILE *log, const char *phase)
{
    static const char *const labels[] = {
        "nvn_image", "desktop_output_image", "texel_buffer", "upload_staging", "readback_staging",
        "copy_staging", "draw_scratch", "compute_scratch", "presentation_conversion", "capture_staging",
    };
    _Static_assert(sizeof labels / sizeof labels[0] == VK_LEDGER_CLASS_COUNT, "ledger class labels");
    if (!log)
        return;
    lock();
    if (!all.alloc_count && !all.failed_count && !wsi_peak) {
        unlock();
        return;
    }
    const Count total = all;
    Count class_counts[VK_LEDGER_CLASS_COUNT], type_counts[MEMORY_TYPES];
    memcpy(class_counts, classes, sizeof class_counts);
    memcpy(type_counts, types, sizeof type_counts);
    const uint32_t swapchain_live = wsi_images, swapchain_peak = wsi_peak;
    const VkExtent2D swapchain_extent = wsi_extent;
    const VkFormat swapchain_format = wsi_format;
    const uint64_t untracked_allocs = untracked_allocations, untracked_frees = untracked_releases;
    unlock();

    fprintf(log, "Vulkan logical memory [%s] bytes, excluding driver and WSI memory\n", phase);
    report_count(log, "all", &total);
    for (size_t i = 0; i < VK_LEDGER_CLASS_COUNT; ++i)
        if (class_counts[i].alloc_count || class_counts[i].failed_count)
            report_count(log, labels[i], &class_counts[i]);
    for (size_t i = 0; i < MEMORY_TYPES; ++i) {
        if (!type_counts[i].alloc_count && !type_counts[i].failed_count)
            continue;
        char name[24];
        snprintf(name, sizeof name, "memory_type_%zu", i);
        report_count(log, name, &type_counts[i]);
    }
    fprintf(log, "  driver WSI images live=%u peak=%u last_extent=%ux%u format=%d; "
                 "untracked allocs=%" PRIu64 " frees=%" PRIu64 "\n",
            swapchain_live, swapchain_peak, swapchain_extent.width, swapchain_extent.height, (int)swapchain_format,
            untracked_allocs, untracked_frees);
    fflush(log);
}
