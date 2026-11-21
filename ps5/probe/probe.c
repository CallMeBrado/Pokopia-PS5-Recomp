/*
 * Pokopia PS5 phase-0 probe (native PPSA title on the PS5 RADV driver).
 *
 * 1. Memory: direct memory size, and whether the recompiled guest's fixed
 *    regions (main 0x71.., sdk 0x75.., subsdk0 0x76.., heap 0x77..) can be
 *    mapped at their exact addresses.
 * 2. Large allocation: how much the title can actually map (1 GiB steps).
 * 3. Vulkan: RADV instance/device, VK_KHR_display swapchain, clears that cycle
 *    colour; Cross changes the colour, Options quits. Runs at most 60 s.
 *
 * Results go to klog and to /data/pokopia/ps5-probe.txt.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

/* Console imports (resolved against the payload SDK's stub libraries). */
int sceKernelDebugOutText(int channel, const char *text);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type,
                                      int64_t *phys);
int32_t sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int32_t sceKernelReleaseDirectMemory(int64_t phys, size_t len);
int32_t sceKernelMunmap(void *addr, size_t len);

int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user);
int scePadInit(void);
int scePadOpen(int32_t user, int32_t type, int32_t index, const void *param);
int scePadReadState(int32_t handle, void *data);

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#define PAD_CROSS 0x00004000u
#define PAD_OPTIONS 0x00000008u
#define SCE_KERNEL_WB_ONION 0
#define SCE_KERNEL_PROT_CPU_RW 0x3
#define SCE_KERNEL_MAP_FIXED 0x10

static FILE *results;

static void report(const char *format, ...)
{
   char line[512];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   char klog[540];
   snprintf(klog, sizeof(klog), "[pokopia-probe] %s\n", line);
   sceKernelDebugOutText(0, klog);
   if (results) {
      fprintf(results, "%s\n", line);
      fflush(results);
   }
}

/* ---------------------------------------------------------------- memory */

static void probe_fixed_regions(void)
{
   static const struct {
      const char *name;
      uintptr_t base;
      size_t size;
   } regions[] = {
      {"main", 0x7100000000ull, 256ull << 20},
      {"sdk", 0x7500000000ull, 64ull << 20},
      {"subsdk0", 0x7600000000ull, 16ull << 20},
      {"heap", 0x7700000000ull, 1ull << 30},
   };
   for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
      void *want = (void *)regions[i].base;
      void *got = mmap(want, regions[i].size, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
      if (got == MAP_FAILED) {
         /* Retry without MAP_FIXED to see where the kernel would put it. */
         void *hint = mmap(want, regions[i].size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
         report("fixed %-8s 0x%010lx size=%zu MiB: FAILED (hint landed at %p)", regions[i].name,
                (unsigned long)regions[i].base, regions[i].size >> 20, hint);
         if (hint != MAP_FAILED)
            munmap(hint, regions[i].size);
         continue;
      }
      volatile uint8_t *p = got;
      p[0] = 0x5a;
      p[regions[i].size - 1] = 0xa5;
      int ok = got == want && p[0] == 0x5a && p[regions[i].size - 1] == 0xa5;
      report("fixed %-8s 0x%010lx size=%zu MiB: %s (at %p)", regions[i].name, (unsigned long)regions[i].base,
             regions[i].size >> 20, ok ? "OK" : "WRONG ADDRESS", got);
      munmap(got, regions[i].size);
   }

   /* The guest heap the way the PS4 port backs it: direct memory mapped at a
    * fixed address (anonymous/flexible memory is a small pool). */
   static const size_t heap_sizes[] = {1ull << 30, 4ull << 30, 8ull << 30};
   for (size_t i = 0; i < sizeof(heap_sizes) / sizeof(heap_sizes[0]); i++) {
      const size_t size = heap_sizes[i];
      int64_t phys = 0;
      int32_t rc = sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), size, 2u << 20,
                                                 SCE_KERNEL_WB_ONION, &phys);
      if (rc != 0) {
         report("direct heap %zu GiB at 0x7700000000: alloc failed rc=0x%08x", size >> 30, (unsigned)rc);
         continue;
      }
      void *addr = (void *)0x7700000000ull;
      rc = sceKernelMapDirectMemory(&addr, size, SCE_KERNEL_PROT_CPU_RW, SCE_KERNEL_MAP_FIXED, phys, 2u << 20);
      if (rc == 0) {
         volatile uint8_t *p = addr;
         p[0] = 1;
         p[size - 1] = 2;
      }
      report("direct heap %zu GiB at 0x7700000000: %s rc=0x%08x (at %p)", size >> 30,
             rc == 0 && addr == (void *)0x7700000000ull ? "OK" : "FAILED", (unsigned)rc, addr);
      if (rc == 0)
         sceKernelMunmap(addr, size);
      sceKernelReleaseDirectMemory(phys, size);
   }
}

static void probe_direct_memory(void)
{
   const size_t total = sceKernelGetDirectMemorySize();
   report("direct memory size: %zu MiB", total >> 20);

   enum { MAX_CHUNKS = 16 };
   const size_t chunk = 1ull << 30;
   int64_t phys[MAX_CHUNKS];
   void *virt[MAX_CHUNKS];
   int n = 0;
   for (; n < MAX_CHUNKS; n++) {
      int32_t rc = sceKernelAllocateDirectMemory(0, (int64_t)total, chunk, 2u << 20, SCE_KERNEL_WB_ONION, &phys[n]);
      if (rc != 0) {
         report("direct alloc #%d (1 GiB) failed rc=0x%08x", n, (unsigned)rc);
         break;
      }
      virt[n] = NULL;
      rc = sceKernelMapDirectMemory(&virt[n], chunk, SCE_KERNEL_PROT_CPU_RW, 0, phys[n], 2u << 20);
      if (rc != 0) {
         report("direct map #%d failed rc=0x%08x", n, (unsigned)rc);
         sceKernelReleaseDirectMemory(phys[n], chunk);
         break;
      }
      memset(virt[n], 0xcd, 4096);
   }
   report("direct memory mapped: %d GiB", n);
   for (int i = 0; i < n; i++) {
      sceKernelMunmap(virt[i], chunk);
      sceKernelReleaseDirectMemory(phys[i], chunk);
   }
}

/* ---------------------------------------------------------------- vulkan */

#define VK_FUNCS(X)                                                                                \
   X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
   X(CreateDevice) X(GetDeviceQueue) X(GetPhysicalDeviceDisplayPropertiesKHR)                      \
   X(GetDisplayModePropertiesKHR) X(CreateDisplayPlaneSurfaceKHR) X(CreateSwapchainKHR)            \
   X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateCommandPool)         \
   X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier)       \
   X(CmdClearColorImage) X(QueueSubmit) X(CreateSemaphore) X(CreateFence) X(WaitForFences)         \
   X(ResetFences) X(ResetCommandBuffer) X(DeviceWaitIdle) X(GetDeviceProcAddr)

#define DECLARE(name) static PFN_vk##name vk##name;
VK_FUNCS(DECLARE)
static PFN_vkCreateInstance vkCreateInstance_;

static int load_instance_funcs(VkInstance instance)
{
#define LOAD(name)                                                                                 \
   vk##name = (PFN_vk##name)vk_icdGetInstanceProcAddr(instance, "vk" #name);                       \
   if (!vk##name) {                                                                                \
      report("vulkan: missing vk" #name);                                                          \
      return 0;                                                                                    \
   }
   VK_FUNCS(LOAD)
#undef LOAD
   return 1;
}

#define VK_CHECK(call)                                                                             \
   do {                                                                                            \
      VkResult r_ = (call);                                                                        \
      if (r_ != VK_SUCCESS) {                                                                      \
         report("vulkan: %s -> %d", #call, (int)r_);                                               \
         return 0;                                                                                 \
      }                                                                                            \
   } while (0)

static uint32_t read_buttons(int32_t pad)
{
   uint8_t data[256];
   memset(data, 0, sizeof(data));
   if (pad < 0 || scePadReadState(pad, data) != 0)
      return 0;
   uint32_t buttons;
   memcpy(&buttons, data, sizeof(buttons));
   /* Log the raw report whenever the first 16 bytes change, to pin down the layout. */
   static uint8_t last[16];
   if (memcmp(last, data, sizeof(last)) != 0) {
      memcpy(last, data, sizeof(last));
      report("pad raw: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", data[0], data[1],
             data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12],
             data[13], data[14], data[15]);
   }
   return buttons;
}

static int run_vulkan(int32_t pad)
{
   vkCreateInstance_ = (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
   if (!vkCreateInstance_) {
      report("vulkan: no vkCreateInstance");
      return 0;
   }
   const char *instance_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};
   const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  .pApplicationName = "pokopia-probe",
                                  .apiVersion = VK_API_VERSION_1_3};
   const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                               .pApplicationInfo = &app,
                                               .enabledExtensionCount = 2,
                                               .ppEnabledExtensionNames = instance_ext};
   VkInstance instance;
   VK_CHECK(vkCreateInstance_(&instance_info, NULL, &instance));
   if (!load_instance_funcs(instance))
      return 0;

   uint32_t count = 1;
   VkPhysicalDevice physical;
   VkResult er = vkEnumeratePhysicalDevices(instance, &count, &physical);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || count == 0) {
      report("vulkan: no physical device (%d)", (int)er);
      return 0;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(physical, &props);
   report("vulkan: %s API %u.%u.%u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
          VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                               .queueFamilyIndex = 0,
                                               .queueCount = 1,
                                               .pQueuePriorities = &priority};
   const char *device_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
   const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                           .queueCreateInfoCount = 1,
                                           .pQueueCreateInfos = &queue_info,
                                           .enabledExtensionCount = 1,
                                           .ppEnabledExtensionNames = device_ext};
   VkDevice device;
   VK_CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* Display surface: first display, first mode. */
   VkDisplayPropertiesKHR display;
   count = 1;
   er = vkGetPhysicalDeviceDisplayPropertiesKHR(physical, &count, &display);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || count == 0) {
      report("vulkan: no display (%d)", (int)er);
      return 0;
   }
   VkDisplayModePropertiesKHR mode;
   count = 1;
   er = vkGetDisplayModePropertiesKHR(physical, display.display, &count, &mode);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || count == 0) {
      report("vulkan: no display mode (%d)", (int)er);
      return 0;
   }
   const VkExtent2D extent = mode.parameters.visibleRegion;
   report("display: %ux%u @ %u mHz", extent.width, extent.height, mode.parameters.refreshRate);
   const VkDisplaySurfaceCreateInfoKHR surface_info = {.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
                                                       .displayMode = mode.displayMode,
                                                       .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                                       .globalAlpha = 1.0f,
                                                       .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
                                                       .imageExtent = extent};
   VkSurfaceKHR surface;
   VK_CHECK(vkCreateDisplayPlaneSurfaceKHR(instance, &surface_info, NULL, &surface));

   const VkSwapchainCreateInfoKHR swap_info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                               .surface = surface,
                                               .minImageCount = 3,
                                               .imageFormat = VK_FORMAT_B8G8R8A8_SRGB,
                                               .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                               .imageExtent = extent,
                                               .imageArrayLayers = 1,
                                               .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                               .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                               .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                               .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                               .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                               .clipped = VK_TRUE};
   VkSwapchainKHR swapchain;
   VK_CHECK(vkCreateSwapchainKHR(device, &swap_info, NULL, &swapchain));
   VkImage images[8];
   count = 8;
   VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, images));
   report("swapchain: %u images", count);

   const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                              .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                              .queueFamilyIndex = 0};
   VkCommandPool pool;
   VK_CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
   const VkCommandBufferAllocateInfo cb_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                .commandPool = pool,
                                                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                .commandBufferCount = 1};
   VkCommandBuffer cb;
   VK_CHECK(vkAllocateCommandBuffers(device, &cb_info, &cb));
   const VkSemaphoreCreateInfo sem_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
   VkSemaphore acquired, rendered;
   VK_CHECK(vkCreateSemaphore(device, &sem_info, NULL, &acquired));
   VK_CHECK(vkCreateSemaphore(device, &sem_info, NULL, &rendered));
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                         .flags = VK_FENCE_CREATE_SIGNALED_BIT};
   VkFence fence;
   VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

   static const float palette[][4] = {
      {0.90f, 0.20f, 0.25f, 1}, {0.15f, 0.65f, 0.30f, 1}, {0.20f, 0.40f, 0.95f, 1}, {0.95f, 0.80f, 0.15f, 1}};
   unsigned colour = 0, frames = 0, presses = 0;
   uint32_t previous = 0;
   const time_t start = time(NULL);
   while (time(NULL) - start < 60) {
      const uint32_t buttons = read_buttons(pad);
      if ((buttons & PAD_CROSS) && !(previous & PAD_CROSS)) {
         colour = (colour + 1) % 4;
         presses++;
         report("pad: Cross -> colour %u", colour);
      }
      if (buttons & PAD_OPTIONS) {
         report("pad: Options -> quit");
         break;
      }
      previous = buttons;

      VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
      VK_CHECK(vkResetFences(device, 1, &fence));
      uint32_t index;
      VK_CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));

      vkResetCommandBuffer(cb, 0);
      const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
      VK_CHECK(vkBeginCommandBuffer(cb, &begin));
      const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image = images[index],
                                      .subresourceRange = range};
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                           NULL, 1, &barrier);
      /* Pulse brightness so a frozen frame is obvious. */
      const float pulse = 0.6f + 0.4f * (float)((frames % 120) < 60 ? (frames % 60) : 60 - (frames % 60)) / 60.0f;
      const VkClearColorValue clear = {{palette[colour][0] * pulse, palette[colour][1] * pulse,
                                        palette[colour][2] * pulse, 1.0f}};
      vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = 0;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                           0, NULL, 1, &barrier);
      VK_CHECK(vkEndCommandBuffer(cb));

      const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                   .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &acquired,
                                   .pWaitDstStageMask = &wait_stage,
                                   .commandBufferCount = 1,
                                   .pCommandBuffers = &cb,
                                   .signalSemaphoreCount = 1,
                                   .pSignalSemaphores = &rendered};
      VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
      const VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                        .waitSemaphoreCount = 1,
                                        .pWaitSemaphores = &rendered,
                                        .swapchainCount = 1,
                                        .pSwapchains = &swapchain,
                                        .pImageIndices = &index};
      VK_CHECK(vkQueuePresentKHR(queue, &present));
      frames++;
      if (frames % 600 == 0)
         report("frames presented: %u", frames);
   }
   vkDeviceWaitIdle(device);
   const long seconds = (long)(time(NULL) - start);
   report("vulkan: %u frames in %ld s (~%ld fps), %u Cross presses", frames, seconds,
          seconds ? (long)frames / seconds : 0, presses);
   return 1;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
   mkdir("/data/pokopia", 0777);
   results = fopen("/data/pokopia/ps5-probe.txt", "w");
   report("Pokopia PS5 probe start");

   probe_fixed_regions();
   probe_direct_memory();

   int32_t pad = -1;
   int32_t user = -1;
   const int us = sceUserServiceInitialize(NULL); /* already-initialized is fine */
   sceUserServiceGetInitialUser(&user);
   scePadInit();
   pad = scePadOpen(user, 0, 0, NULL);
   report("pad: userservice=0x%08x user=%d handle=%d", (unsigned)us, (int)user, (int)pad);

   const int ok = run_vulkan(pad);
   report("Pokopia PS5 probe %s", ok ? "PASS" : "FAIL");
   if (results)
      fclose(results);
   return ok ? 0 : 1;
}
