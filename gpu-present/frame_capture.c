/* SPDX-License-Identifier: GPL-3.0-or-later */
#if !defined(_WIN32) && !defined(__ORBIS__)
#define _POSIX_C_SOURCE 200809L
#endif
#include "frame_capture.h"
#include "../gfx-bridge/vk_ledger.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

void pokopia_frame_capture_close(PokopiaFrameCapture *capture)
{
    if (!capture || !capture->device)
        return;
    if (capture->mapped)
        vkUnmapMemory(capture->device, capture->memory);
    if (capture->buffer)
        vkDestroyBuffer(capture->device, capture->buffer, NULL);
    if (capture->memory)
        pokopia_vk_ledger_free(vkFreeMemory, capture->device, capture->memory);
    *capture = (PokopiaFrameCapture){0};
}

static VkResult allocate_readback(PokopiaFrameCapture *capture, VkPhysicalDevice physical,
                                  VkDeviceSize bytes)
{
    const VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vkCreateBuffer(capture->device, &info, NULL, &capture->buffer);
    if (result != VK_SUCCESS)
        return result;

    VkMemoryRequirements requirements;
    VkPhysicalDeviceMemoryProperties properties;
    vkGetBufferMemoryRequirements(capture->device, capture->buffer, &requirements);
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = UINT32_MAX;
    int best = -1;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        const bool coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        const int rank = ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 0) + coherent;
        if (rank > best) {
            best = rank;
            type = i;
            capture->coherent = coherent;
        }
    }
    if (type == UINT32_MAX)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    };
    result = pokopia_vk_ledger_allocate(vkAllocateMemory, capture->device, &allocation, &capture->memory,
                                       VK_LEDGER_CAPTURE);
    if (result == VK_SUCCESS)
        result = vkBindBufferMemory(capture->device, capture->buffer, capture->memory, 0);
    if (result == VK_SUCCESS)
        result = vkMapMemory(capture->device, capture->memory, 0, VK_WHOLE_SIZE, 0, &capture->mapped);
    return result;
}

VkResult pokopia_frame_capture_open(PokopiaFrameCapture *capture, VkPhysicalDevice physical,
                                    VkDevice device, VkFormat format, VkExtent2D extent)
{
    if (!capture || capture->device || !physical || !device || !extent.width || !extent.height ||
        extent.width > INT32_MAX || extent.height > INT32_MAX ||
        extent.width > SIZE_MAX / 4 / extent.height)
        return VK_ERROR_INITIALIZATION_FAILED;
    const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    *capture = (PokopiaFrameCapture){.device = device, .extent = extent, .bgra = bgra};
    const VkDeviceSize bytes = (VkDeviceSize)extent.width * extent.height * 4;
    VkResult result = allocate_readback(capture, physical, bytes);
    if (result != VK_SUCCESS)
        pokopia_frame_capture_close(capture);
    return result;
}

VkResult pokopia_frame_capture_record(PokopiaFrameCapture *capture, VkCommandBuffer command,
                                      VkImage image, VkImageLayout layout,
                                      VkPipelineStageFlags stages, VkAccessFlags access)
{
    if (!capture || !capture->buffer || !command || !image || !stages ||
        layout == VK_IMAGE_LAYOUT_UNDEFINED || layout == VK_IMAGE_LAYOUT_PREINITIALIZED ||
        layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (capture->recorded)
        return VK_NOT_READY;

    VkImageMemoryBarrier image_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = access, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = layout, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &image_barrier);

    const VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {capture->extent.width, capture->extent.height, 1},
    };
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           capture->buffer, 1, &region);

    const VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = capture->buffer, .size = VK_WHOLE_SIZE,
    };
    image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.dstAccessMask = 0;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 1, &host_barrier, 1, &image_barrier);
    capture->recorded = true;
    return VK_SUCCESS;
}

enum { PNG_BLOCK_BYTES = 65535, PNG_ADLER_MODULUS = 65521 };

typedef struct {
    uint32_t crc_table[256];
    uint32_t adler_a, adler_b;
    size_t used;
    unsigned char block[5 + PNG_BLOCK_BYTES];
} PngStream;

static void big_endian(unsigned char out[4], uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static void init_png(PngStream *stream)
{
    stream->adler_a = 1;
    stream->adler_b = 0;
    stream->used = 0;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        stream->crc_table[i] = crc;
    }
}

static bool png_chunk(FILE *file, const PngStream *stream, const char name[4],
                      const unsigned char *data, uint32_t size)
{
    unsigned char length[4], crc_bytes[4];
    big_endian(length, size);
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < 4; ++i)
        crc = (crc >> 8) ^ stream->crc_table[(crc ^ (unsigned char)name[i]) & 0xff];
    for (size_t i = 0; i < size; ++i)
        crc = (crc >> 8) ^ stream->crc_table[(crc ^ data[i]) & 0xff];
    big_endian(crc_bytes, ~crc);
    return fwrite(length, 1, sizeof length, file) == sizeof length &&
           fwrite(name, 1, 4, file) == 4 &&
           (!size || fwrite(data, 1, size, file) == size) &&
           fwrite(crc_bytes, 1, sizeof crc_bytes, file) == sizeof crc_bytes;
}

/* Stream one stored DEFLATE block at a time. The PNG decoder combines the
 * IDAT chunks into one zlib stream, even when a block splits a scanline. */
static bool png_block(FILE *file, PngStream *stream, bool final)
{
    unsigned char *block = stream->block;
    const uint16_t length = (uint16_t)stream->used;
    const uint16_t inverse = (uint16_t)~length;
    block[0] = final;
    block[1] = (unsigned char)length;
    block[2] = (unsigned char)(length >> 8);
    block[3] = (unsigned char)inverse;
    block[4] = (unsigned char)(inverse >> 8);
    for (size_t i = 0; i < length;) {
        /* Bound the sums before reducing them modulo 65521. */
        const size_t end = length - i > 5552 ? i + 5552 : length;
        for (; i < end; ++i) {
            stream->adler_a += block[5 + i];
            stream->adler_b += stream->adler_a;
        }
        stream->adler_a %= PNG_ADLER_MODULUS;
        stream->adler_b %= PNG_ADLER_MODULUS;
    }
    stream->used = 0;
    return png_chunk(file, stream, "IDAT", block, 5u + length);
}

static bool png_append(FILE *file, PngStream *stream, const unsigned char *bytes, size_t size)
{
    while (size) {
        if (stream->used == PNG_BLOCK_BYTES && !png_block(file, stream, false))
            return false;
        const size_t room = PNG_BLOCK_BYTES - stream->used;
        const size_t take = size < room ? size : room;
        memcpy(stream->block + 5 + stream->used, bytes, take);
        stream->used += take;
        bytes += take;
        size -= take;
    }
    return true;
}

static bool write_png(FILE *file, const PokopiaFrameCapture *capture)
{
    const size_t row_size = (size_t)capture->extent.width * 3 + 1;
    PngStream *stream = malloc(sizeof *stream);
    unsigned char *row = malloc(row_size);
    if (!stream || !row) {
        free(stream);
        free(row);
        errno = ENOMEM;
        return false;
    }
    init_png(stream);
    static const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const unsigned char zlib_header[] = {0x78, 0x01};
    static const unsigned char intent[] = {0};
    unsigned char header[13] = {0};
    big_endian(header, capture->extent.width);
    big_endian(header + 4, capture->extent.height);
    header[8] = 8;
    header[9] = 2; /* RGB matches opaque scanout regardless of the swapchain's alpha bytes. */
    bool ok = fwrite(signature, 1, sizeof signature, file) == sizeof signature &&
              png_chunk(file, stream, "IHDR", header, sizeof header) &&
              png_chunk(file, stream, "sRGB", intent, sizeof intent) &&
              png_chunk(file, stream, "IDAT", zlib_header, sizeof zlib_header);
    const unsigned char *pixel = capture->mapped;
    const size_t red = capture->bgra ? 2 : 0;
    const size_t blue = capture->bgra ? 0 : 2;
    row[0] = 0;
    for (uint32_t y = 0; ok && y < capture->extent.height; ++y) {
        unsigned char *rgb = row + 1;
        for (uint32_t x = 0; x < capture->extent.width; ++x) {
            rgb[0] = pixel[red];
            rgb[1] = pixel[1];
            rgb[2] = pixel[blue];
            rgb += 3;
            pixel += 4;
        }
        ok = png_append(file, stream, row, row_size);
    }
    if (ok && png_block(file, stream, true)) {
        unsigned char checksum[4];
        big_endian(checksum, (stream->adler_b << 16) | stream->adler_a);
        ok = png_chunk(file, stream, "IDAT", checksum, sizeof checksum) &&
             png_chunk(file, stream, "IEND", NULL, 0);
    } else {
        ok = false;
    }
    free(stream);
    free(row);
    return ok;
}

static bool publish_png(const char *path, const PokopiaFrameCapture *capture)
{
#if defined(__PROSPERO__)
    /* R72: the PS5 payload libc call path through mkstemp/fchmod/fdopen
     * jumped to address 0, and 0640 would hide the PNG from FTP. Write a
     * fixed temporary name (one capture at a time) and rename it into place. */
    {
        char temporary[1024];
        if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) {
            errno = ENAMETOOLONG;
            return false;
        }
        FILE *file = fopen(temporary, "wb");
        if (!file)
            return false;
        bool ok = write_png(file, capture);
        if (fclose(file) != 0)
            ok = false;
        if (ok && rename(temporary, path) == 0)
            return true;
        remove(temporary);
        return false;
    }
#endif
    static const char suffix[] = ".tmp-XXXXXX";
    const size_t length = strlen(path);
    if (length > SIZE_MAX - sizeof suffix) {
        errno = ENAMETOOLONG;
        return false;
    }
    char *temporary = malloc(length + sizeof suffix);
    if (!temporary) {
        errno = ENOMEM;
        return false;
    }
    memcpy(temporary, path, length);
    memcpy(temporary + length, suffix, sizeof suffix);

#ifdef _WIN32
    const errno_t temp_error = _mktemp_s(temporary, length + sizeof suffix);
    if (temp_error)
        errno = temp_error;
    const int fd = temp_error ? -1 : _open(temporary, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                                           _S_IREAD | _S_IWRITE);
#else
    const int fd = mkstemp(temporary);
    if (fd >= 0 && fchmod(fd, 0640) != 0) {
        const int error = errno;
        close(fd);
        remove(temporary);
        free(temporary);
        errno = error;
        return false;
    }
#endif
    if (fd < 0) {
        const int error = errno;
        free(temporary);
        errno = error;
        return false;
    }

#ifdef _WIN32
    FILE *file = _fdopen(fd, "wb");
#else
    FILE *file = fdopen(fd, "wb");
#endif
    int error = 0;
    if (file) {
        errno = 0;
        if (!write_png(file, capture))
            error = errno ? errno : EIO;
        if (fclose(file) != 0 && !error)
            error = errno ? errno : EIO;
    } else {
        error = errno ? errno : EIO;
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
    }
    if (!error) {
#ifdef _WIN32
        if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            error = EIO;
#else
        if (rename(temporary, path) != 0)
            error = errno ? errno : EIO;
#endif
    }
    if (error)
        remove(temporary);
    free(temporary);
    errno = error;
    return !error;
}

VkResult pokopia_frame_capture_save(PokopiaFrameCapture *capture, VkFence fence, const char *path)
{
    if (!capture || !capture->recorded || !fence || !path || !*path)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = vkGetFenceStatus(capture->device, fence);
    if (result != VK_SUCCESS)
        return result;
    capture->recorded = false;
    if (!capture->coherent) {
        const VkMappedMemoryRange range = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = capture->memory,
            .size = VK_WHOLE_SIZE,
        };
        result = vkInvalidateMappedMemoryRanges(capture->device, 1, &range);
        if (result != VK_SUCCESS)
            return result;
    }
    return publish_png(path, capture) ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}
