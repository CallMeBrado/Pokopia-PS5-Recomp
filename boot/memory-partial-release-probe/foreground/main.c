#define _POSIX_C_SOURCE 200809L
#include "../probe.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

int32_t sceKernelMmap(void *, size_t, int32_t, int32_t, int32_t, off_t, void **);
int32_t sceKernelUsleep(unsigned int);
int32_t probe_flexible_available(size_t *) __asm__("sceKernelAvailableFlexibleMemorySize");

static uint8_t data[PROBE_SLICE];

static uint8_t pattern(size_t index, uint8_t seed)
{
    return (uint8_t)(seed ^ index ^ (index >> 6) ^ (index >> 12));
}

static bool write_slice(int fd, uint8_t seed)
{
    for (size_t i = 0; i < sizeof data; ++i)
        data[i] = pattern(i, seed);
    size_t written = 0;
    while (written < sizeof data) {
        ssize_t result = write(fd, data + written, sizeof data - written);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return false;
        written += (size_t)result;
    }
    return true;
}

static void memory_snapshot(FILE *log, const char *phase)
{
    DirectOffset offset = -1;
    size_t span = 0, flexible = 0;
    const int32_t direct_result = sceKernelAvailableDirectMemorySize(0,
        (DirectOffset)sceKernelGetDirectMemorySize(), PROBE_PAGE, &offset, &span);
    const int32_t flexible_result = probe_flexible_available(&flexible);
    fprintf(log, "file-map %s direct rc=%#" PRIx32 " span=%zu flexible rc=%#" PRIx32 " free=%zu\n",
        phase, (uint32_t)direct_result, span, (uint32_t)flexible_result, flexible);
    fflush(log);
}

static bool mapped_slice(FILE *log, int fd, int flags, off_t offset, uint8_t seed)
{
    void *mapping = NULL;
    const int32_t result = sceKernelMmap(NULL, PROBE_SLICE, PROT_READ, flags, fd, offset, &mapping);
    fprintf(log, "file-map flags=%#x offset=%#" PRIx64 " rc=%#" PRIx32 " address=%p\n",
        flags, (uint64_t)offset, (uint32_t)result, mapping);
    fflush(log);
    if (result || !mapping || mapping == MAP_FAILED)
        return false;

    void *start = NULL, *end = NULL;
    int32_t protection = 0;
    const int32_t query = sceKernelQueryMemoryProtection(mapping, &start, &end, &protection);
    fprintf(log, "file-map protection rc=%#" PRIx32 " flags=%#x %p..%p\n",
        (uint32_t)query, (unsigned)protection, start, end);
    fflush(log);
    const uintptr_t address = (uintptr_t)mapping;
    bool verified = !query && (protection & PROT_READ) && (uintptr_t)start <= address &&
        (uintptr_t)end > address && (uintptr_t)end - address >= PROBE_SLICE;
    if (verified) {
        const uint8_t *bytes = mapping;
        for (size_t i = 0; i < PROBE_SLICE; ++i) {
            if (bytes[i] == pattern(i, seed))
                continue;
            fprintf(log, "file-map byte mismatch at %#zx expected=%#x actual=%#x\n",
                i, pattern(i, seed), bytes[i]);
            verified = false;
            break;
        }
    }
    memory_snapshot(log, "after touching view");
    const int32_t unmap = sceKernelMunmap(mapping, PROBE_SLICE);
    fprintf(log, "%s file-map bytes=%u unmap=%#" PRIx32 "\n",
        verified && !unmap ? "PASS" : "FAIL", PROBE_SLICE, (uint32_t)unmap);
    fflush(log);
    return verified && !unmap;
}

static bool run_file_proof(FILE *log)
{
    char path[96];
    snprintf(path, sizeof path, "/data/pokopia-ps4-memory-proof-r3-%d.tmp", (int)getpid());
    const int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        fprintf(log, "FAIL could not create the file-map fixture (errno %d)\n", errno);
        return false;
    }
    bool prepared = write_slice(fd, 0x47) && write_slice(fd, 0x9a) && !fsync(fd);
    bool verified = false;
    if (prepared) {
        memory_snapshot(log, "before mapping");
        const bool first = mapped_slice(log, fd, MAP_SHARED, 0, 0x47);
        const bool second = mapped_slice(log, fd, MAP_SHARED, PROBE_SLICE, 0x9a);
        const bool private_view = mapped_slice(log, fd, MAP_PRIVATE, PROBE_SLICE, 0x9a);
        verified = (first && second) || private_view;
    } else {
        fprintf(log, "FAIL could not write the file-map fixture (errno %d)\n", errno);
    }
    const int closed = close(fd);
    const int removed = unlink(path);
    memory_snapshot(log, "after cleanup");
    return verified && !closed && !removed;
}

int main(void)
{
    FILE *log = fopen("/data/pokopia-ps4-memory-proof-r3.txt", "w");
    if (log) {
        fprintf(log, "Pokopia memory proof R3 foreground PID %d\n", (int)getpid());
        fflush(log);
        const bool partial = run_direct_memory_proof(log);
        const bool file = run_file_proof(log);
        fprintf(log, "RESULT partial-direct=%s file-mapping=%s\n",
            partial ? "PASS" : "FAIL", file ? "PASS" : "FAIL");
        fclose(log);
    }
    for (;;)
        sceKernelUsleep(1000000);
}
