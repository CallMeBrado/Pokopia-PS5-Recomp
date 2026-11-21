/* GENERATED - do not edit. Regenerate with:
 *
 *     python3 <orbis-compat>/vkloader/gen.py --objs <this project's object dirs> --out <this file>
 *
 * One forwarding thunk per Vulkan entry point THIS project references, over the RADV ICD. There is no Vulkan
 * loader on this console and no shared object, so something has to define the vk* symbols a program calls and
 * forward them through vkGetInstanceProcAddr; vkloader.c is that something and this is the generated half.
 *
 * The list is read from this project's own objects, so it cannot rot into a link error in a file nobody edited.
 * Re-running the command above must leave `git diff` empty.
 *
 * THE POINTERS ARE RESOLVED ON FIRST CALL, per thunk. A race between two threads calling the same entry point
 * for the first time resolves it twice and stores the same value twice, which is benign - the alternative is a
 * mutex on every Vulkan call in the program, to protect a write that cannot tear.
 *
 * vkloader.h is orbis-compat's, reached through the include directory the build adds for it -
 * ORBIS_VKLOADER_DIR in a CMake consumer. It is not a file of this repository.
 *
 * SPDX-License-Identifier: MIT
 */

#include "vkloader.h"

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
{
  static PFN_vkAcquireNextImageKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkAcquireNextImageKHR)vkl_resolve("vkAcquireNextImageKHR");
    if(pfn == NULL)
      vkl_missing("vkAcquireNextImageKHR");
    }
  return pfn(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory)
{
  static PFN_vkAllocateMemory pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkAllocateMemory)vkl_resolve("vkAllocateMemory");
    if(pfn == NULL)
      vkl_missing("vkAllocateMemory");
    }
  return pfn(device, pAllocateInfo, pAllocator, pMemory);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo)
{
  static PFN_vkBeginCommandBuffer pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkBeginCommandBuffer)vkl_resolve("vkBeginCommandBuffer");
    if(pfn == NULL)
      vkl_missing("vkBeginCommandBuffer");
    }
  return pfn(commandBuffer, pBeginInfo);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
  static PFN_vkBindBufferMemory pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkBindBufferMemory)vkl_resolve("vkBindBufferMemory");
    if(pfn == NULL)
      vkl_missing("vkBindBufferMemory");
    }
  return pfn(device, buffer, memory, memoryOffset);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
  static PFN_vkBindImageMemory pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkBindImageMemory)vkl_resolve("vkBindImageMemory");
    if(pfn == NULL)
      vkl_missing("vkBindImageMemory");
    }
  return pfn(device, image, memory, memoryOffset);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter)
{
  static PFN_vkCmdBlitImage pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdBlitImage)vkl_resolve("vkCmdBlitImage");
    if(pfn == NULL)
      vkl_missing("vkCmdBlitImage");
    }
  pfn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges)
{
  static PFN_vkCmdClearColorImage pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdClearColorImage)vkl_resolve("vkCmdClearColorImage");
    if(pfn == NULL)
      vkl_missing("vkCmdClearColorImage");
    }
  pfn(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions)
{
  static PFN_vkCmdCopyBufferToImage pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdCopyBufferToImage)vkl_resolve("vkCmdCopyBufferToImage");
    if(pfn == NULL)
      vkl_missing("vkCmdCopyBufferToImage");
    }
  pfn(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions)
{
  static PFN_vkCmdCopyImage pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdCopyImage)vkl_resolve("vkCmdCopyImage");
    if(pfn == NULL)
      vkl_missing("vkCmdCopyImage");
    }
  pfn(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy* pRegions)
{
  static PFN_vkCmdCopyImageToBuffer pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdCopyImageToBuffer)vkl_resolve("vkCmdCopyImageToBuffer");
    if(pfn == NULL)
      vkl_missing("vkCmdCopyImageToBuffer");
    }
  pfn(commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers)
{
  static PFN_vkCmdPipelineBarrier pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdPipelineBarrier)vkl_resolve("vkCmdPipelineBarrier");
    if(pfn == NULL)
      vkl_missing("vkCmdPipelineBarrier");
    }
  pfn(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer)
{
  static PFN_vkCreateBuffer pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateBuffer)vkl_resolve("vkCreateBuffer");
    if(pfn == NULL)
      vkl_missing("vkCreateBuffer");
    }
  return pfn(device, pCreateInfo, pAllocator, pBuffer);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkFence* pFence)
{
  static PFN_vkCreateFence pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateFence)vkl_resolve("vkCreateFence");
    if(pfn == NULL)
      vkl_missing("vkCreateFence");
    }
  return pfn(device, pCreateInfo, pAllocator, pFence);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage)
{
  static PFN_vkCreateImage pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateImage)vkl_resolve("vkCreateImage");
    if(pfn == NULL)
      vkl_missing("vkCreateImage");
    }
  return pfn(device, pCreateInfo, pAllocator, pImage);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView)
{
  static PFN_vkCreateImageView pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateImageView)vkl_resolve("vkCreateImageView");
    if(pfn == NULL)
      vkl_missing("vkCreateImageView");
    }
  return pfn(device, pCreateInfo, pAllocator, pView);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
  static PFN_vkCreateSwapchainKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateSwapchainKHR)vkl_resolve("vkCreateSwapchainKHR");
    if(pfn == NULL)
      vkl_missing("vkCreateSwapchainKHR");
    }
  return pfn(device, pCreateInfo, pAllocator, pSwapchain);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroyBuffer pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroyBuffer)vkl_resolve("vkDestroyBuffer");
    if(pfn == NULL)
      vkl_missing("vkDestroyBuffer");
    }
  pfn(device, buffer, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroyFence pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroyFence)vkl_resolve("vkDestroyFence");
    if(pfn == NULL)
      vkl_missing("vkDestroyFence");
    }
  pfn(device, fence, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroyImage pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroyImage)vkl_resolve("vkDestroyImage");
    if(pfn == NULL)
      vkl_missing("vkDestroyImage");
    }
  pfn(device, image, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroyImageView pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroyImageView)vkl_resolve("vkDestroyImageView");
    if(pfn == NULL)
      vkl_missing("vkDestroyImageView");
    }
  pfn(device, imageView, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroySurfaceKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroySurfaceKHR)vkl_resolve("vkDestroySurfaceKHR");
    if(pfn == NULL)
      vkl_missing("vkDestroySurfaceKHR");
    }
  pfn(instance, surface, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroySwapchainKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroySwapchainKHR)vkl_resolve("vkDestroySwapchainKHR");
    if(pfn == NULL)
      vkl_missing("vkDestroySwapchainKHR");
    }
  pfn(device, swapchain, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
  static PFN_vkEndCommandBuffer pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkEndCommandBuffer)vkl_resolve("vkEndCommandBuffer");
    if(pfn == NULL)
      vkl_missing("vkEndCommandBuffer");
    }
  return pfn(commandBuffer);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkFlushMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
  static PFN_vkFlushMappedMemoryRanges pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkFlushMappedMemoryRanges)vkl_resolve("vkFlushMappedMemoryRanges");
    if(pfn == NULL)
      vkl_missing("vkFlushMappedMemoryRanges");
    }
  return pfn(device, memoryRangeCount, pMemoryRanges);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkFreeMemory pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkFreeMemory)vkl_resolve("vkFreeMemory");
    if(pfn == NULL)
      vkl_missing("vkFreeMemory");
    }
  pfn(device, memory, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemoryRequirements)
{
  static PFN_vkGetBufferMemoryRequirements pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetBufferMemoryRequirements)vkl_resolve("vkGetBufferMemoryRequirements");
    if(pfn == NULL)
      vkl_missing("vkGetBufferMemoryRequirements");
    }
  pfn(device, buffer, pMemoryRequirements);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkGetFenceStatus(VkDevice device, VkFence fence)
{
  static PFN_vkGetFenceStatus pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetFenceStatus)vkl_resolve("vkGetFenceStatus");
    if(pfn == NULL)
      vkl_missing("vkGetFenceStatus");
    }
  return pfn(device, fence);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pMemoryRequirements)
{
  static PFN_vkGetImageMemoryRequirements pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetImageMemoryRequirements)vkl_resolve("vkGetImageMemoryRequirements");
    if(pfn == NULL)
      vkl_missing("vkGetImageMemoryRequirements");
    }
  pfn(device, image, pMemoryRequirements);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pFormatProperties)
{
  static PFN_vkGetPhysicalDeviceFormatProperties pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetPhysicalDeviceFormatProperties)vkl_resolve("vkGetPhysicalDeviceFormatProperties");
    if(pfn == NULL)
      vkl_missing("vkGetPhysicalDeviceFormatProperties");
    }
  pfn(physicalDevice, format, pFormatProperties);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
  static PFN_vkGetPhysicalDeviceMemoryProperties pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetPhysicalDeviceMemoryProperties)vkl_resolve("vkGetPhysicalDeviceMemoryProperties");
    if(pfn == NULL)
      vkl_missing("vkGetPhysicalDeviceMemoryProperties");
    }
  pfn(physicalDevice, pMemoryProperties);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
  static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkl_resolve("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if(pfn == NULL)
      vkl_missing("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    }
  return pfn(physicalDevice, surface, pSurfaceCapabilities);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats)
{
  static PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkl_resolve("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if(pfn == NULL)
      vkl_missing("vkGetPhysicalDeviceSurfaceFormatsKHR");
    }
  return pfn(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported)
{
  static PFN_vkGetPhysicalDeviceSurfaceSupportKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkl_resolve("vkGetPhysicalDeviceSurfaceSupportKHR");
    if(pfn == NULL)
      vkl_missing("vkGetPhysicalDeviceSurfaceSupportKHR");
    }
  return pfn(physicalDevice, queueFamilyIndex, surface, pSupported);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages)
{
  static PFN_vkGetSwapchainImagesKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkGetSwapchainImagesKHR)vkl_resolve("vkGetSwapchainImagesKHR");
    if(pfn == NULL)
      vkl_missing("vkGetSwapchainImagesKHR");
    }
  return pfn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange* pMemoryRanges)
{
  static PFN_vkInvalidateMappedMemoryRanges pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkInvalidateMappedMemoryRanges)vkl_resolve("vkInvalidateMappedMemoryRanges");
    if(pfn == NULL)
      vkl_missing("vkInvalidateMappedMemoryRanges");
    }
  return pfn(device, memoryRangeCount, pMemoryRanges);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** ppData)
{
  static PFN_vkMapMemory pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkMapMemory)vkl_resolve("vkMapMemory");
    if(pfn == NULL)
      vkl_missing("vkMapMemory");
    }
  return pfn(device, memory, offset, size, flags, ppData);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
  static PFN_vkQueuePresentKHR pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkQueuePresentKHR)vkl_resolve("vkQueuePresentKHR");
    if(pfn == NULL)
      vkl_missing("vkQueuePresentKHR");
    }
  return pfn(queue, pPresentInfo);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
  static PFN_vkQueueSubmit pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkQueueSubmit)vkl_resolve("vkQueueSubmit");
    if(pfn == NULL)
      vkl_missing("vkQueueSubmit");
    }
  return pfn(queue, submitCount, pSubmits, fence);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
  static PFN_vkResetCommandBuffer pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkResetCommandBuffer)vkl_resolve("vkResetCommandBuffer");
    if(pfn == NULL)
      vkl_missing("vkResetCommandBuffer");
    }
  return pfn(commandBuffer, flags);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences)
{
  static PFN_vkResetFences pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkResetFences)vkl_resolve("vkResetFences");
    if(pfn == NULL)
      vkl_missing("vkResetFences");
    }
  return pfn(device, fenceCount, pFences);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
  static PFN_vkUnmapMemory pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkUnmapMemory)vkl_resolve("vkUnmapMemory");
    if(pfn == NULL)
      vkl_missing("vkUnmapMemory");
    }
  pfn(device, memory);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout)
{
  static PFN_vkWaitForFences pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkWaitForFences)vkl_resolve("vkWaitForFences");
    if(pfn == NULL)
      vkl_missing("vkWaitForFences");
    }
  return pfn(device, fenceCount, pFences, waitAll, timeout);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers)
{
  static PFN_vkAllocateCommandBuffers pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkAllocateCommandBuffers)vkl_resolve("vkAllocateCommandBuffers");
    if(pfn == NULL)
      vkl_missing("vkAllocateCommandBuffers");
    }
  return pfn(device, pAllocateInfo, pCommandBuffers);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool)
{
  static PFN_vkCreateCommandPool pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateCommandPool)vkl_resolve("vkCreateCommandPool");
    if(pfn == NULL)
      vkl_missing("vkCreateCommandPool");
    }
  return pfn(device, pCreateInfo, pAllocator, pCommandPool);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroyCommandPool pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroyCommandPool)vkl_resolve("vkDestroyCommandPool");
    if(pfn == NULL)
      vkl_missing("vkDestroyCommandPool");
    }
  pfn(device, commandPool, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSemaphore* pSemaphore)
{
  static PFN_vkCreateSemaphore pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateSemaphore)vkl_resolve("vkCreateSemaphore");
    if(pfn == NULL)
      vkl_missing("vkCreateSemaphore");
    }
  return pfn(device, pCreateInfo, pAllocator, pSemaphore);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroySemaphore pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroySemaphore)vkl_resolve("vkDestroySemaphore");
    if(pfn == NULL)
      vkl_missing("vkDestroySemaphore");
    }
  pfn(device, semaphore, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkQueueWaitIdle(VkQueue queue)
{
  static PFN_vkQueueWaitIdle pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkQueueWaitIdle)vkl_resolve("vkQueueWaitIdle");
    if(pfn == NULL)
      vkl_missing("vkQueueWaitIdle");
    }
  return pfn(queue);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule)
{
  static PFN_vkCreateShaderModule pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateShaderModule)vkl_resolve("vkCreateShaderModule");
    if(pfn == NULL)
      vkl_missing("vkCreateShaderModule");
    }
  return pfn(device, pCreateInfo, pAllocator, pShaderModule);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator)
{
  static PFN_vkDestroyShaderModule pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkDestroyShaderModule)vkl_resolve("vkDestroyShaderModule");
    if(pfn == NULL)
      vkl_missing("vkDestroyShaderModule");
    }
  pfn(device, shaderModule, pAllocator);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
  static PFN_vkCreateComputePipelines pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateComputePipelines)vkl_resolve("vkCreateComputePipelines");
    if(pfn == NULL)
      vkl_missing("vkCreateComputePipelines");
    }
  return pfn(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout)
{
  static PFN_vkCreatePipelineLayout pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreatePipelineLayout)vkl_resolve("vkCreatePipelineLayout");
    if(pfn == NULL)
      vkl_missing("vkCreatePipelineLayout");
    }
  return pfn(device, pCreateInfo, pAllocator, pPipelineLayout);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout)
{
  static PFN_vkCreateDescriptorSetLayout pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateDescriptorSetLayout)vkl_resolve("vkCreateDescriptorSetLayout");
    if(pfn == NULL)
      vkl_missing("vkCreateDescriptorSetLayout");
    }
  return pfn(device, pCreateInfo, pAllocator, pSetLayout);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool)
{
  static PFN_vkCreateDescriptorPool pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateDescriptorPool)vkl_resolve("vkCreateDescriptorPool");
    if(pfn == NULL)
      vkl_missing("vkCreateDescriptorPool");
    }
  return pfn(device, pCreateInfo, pAllocator, pDescriptorPool);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets)
{
  static PFN_vkAllocateDescriptorSets pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkAllocateDescriptorSets)vkl_resolve("vkAllocateDescriptorSets");
    if(pfn == NULL)
      vkl_missing("vkAllocateDescriptorSets");
    }
  return pfn(device, pAllocateInfo, pDescriptorSets);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies)
{
  static PFN_vkUpdateDescriptorSets pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkUpdateDescriptorSets)vkl_resolve("vkUpdateDescriptorSets");
    if(pfn == NULL)
      vkl_missing("vkUpdateDescriptorSets");
    }
  pfn(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

__attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler)
{
  static PFN_vkCreateSampler pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCreateSampler)vkl_resolve("vkCreateSampler");
    if(pfn == NULL)
      vkl_missing("vkCreateSampler");
    }
  return pfn(device, pCreateInfo, pAllocator, pSampler);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
  static PFN_vkCmdBindPipeline pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdBindPipeline)vkl_resolve("vkCmdBindPipeline");
    if(pfn == NULL)
      vkl_missing("vkCmdBindPipeline");
    }
  pfn(commandBuffer, pipelineBindPoint, pipeline);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets)
{
  static PFN_vkCmdBindDescriptorSets pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdBindDescriptorSets)vkl_resolve("vkCmdBindDescriptorSets");
    if(pfn == NULL)
      vkl_missing("vkCmdBindDescriptorSets");
    }
  pfn(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues)
{
  static PFN_vkCmdPushConstants pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdPushConstants)vkl_resolve("vkCmdPushConstants");
    if(pfn == NULL)
      vkl_missing("vkCmdPushConstants");
    }
  pfn(commandBuffer, layout, stageFlags, offset, size, pValues);
}

__attribute__((weak)) VKAPI_ATTR void VKAPI_CALL
vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
  static PFN_vkCmdDispatch pfn;
  if(pfn == NULL) {
    pfn = (PFN_vkCmdDispatch)vkl_resolve("vkCmdDispatch");
    if(pfn == NULL)
      vkl_missing("vkCmdDispatch");
    }
  pfn(commandBuffer, groupCountX, groupCountY, groupCountZ);
}
