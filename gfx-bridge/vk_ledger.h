/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_VK_LEDGER_H
#define POKOPIA_VK_LEDGER_H

#include <stdio.h>
#include <vulkan/vulkan.h>

typedef enum {
    VK_LEDGER_NV_TEXTURE, VK_LEDGER_HOST_DISPLAY, VK_LEDGER_TEXEL,
    VK_LEDGER_UPLOAD, VK_LEDGER_DOWNLOAD, VK_LEDGER_COPY,
    VK_LEDGER_DRAW, VK_LEDGER_COMPUTE, VK_LEDGER_CONVERSION, VK_LEDGER_CAPTURE,
    VK_LEDGER_CLASS_COUNT,
} PokopiaVkLedgerClass;

#ifdef POKOPIA_VK_MEMORY_TRACE
VkResult pokopia_vk_ledger_allocate(PFN_vkAllocateMemory allocate, VkDevice device,
                                  const VkMemoryAllocateInfo *request, VkDeviceMemory *memory,
                                  PokopiaVkLedgerClass category);
void pokopia_vk_ledger_free(PFN_vkFreeMemory release, VkDevice device, VkDeviceMemory memory);
void pokopia_vk_ledger_swapchain_add(uint32_t images, VkExtent2D extent, VkFormat format);
void pokopia_vk_ledger_swapchain_remove(uint32_t images);
void pokopia_vk_ledger_report(FILE *log, const char *phase);
#else
static inline VkResult pokopia_vk_ledger_allocate(PFN_vkAllocateMemory allocate, VkDevice device,
                                                   const VkMemoryAllocateInfo *request, VkDeviceMemory *memory,
                                                   PokopiaVkLedgerClass category)
{
    (void)category;
    return allocate(device, request, NULL, memory);
}
static inline void pokopia_vk_ledger_free(PFN_vkFreeMemory release, VkDevice device, VkDeviceMemory memory)
{
    release(device, memory, NULL);
}
static inline void pokopia_vk_ledger_swapchain_add(uint32_t images, VkExtent2D extent, VkFormat format)
{
    (void)images;
    (void)extent;
    (void)format;
}
static inline void pokopia_vk_ledger_swapchain_remove(uint32_t images) { (void)images; }
static inline void pokopia_vk_ledger_report(FILE *log, const char *phase) { (void)log; (void)phase; }
#endif

#endif
