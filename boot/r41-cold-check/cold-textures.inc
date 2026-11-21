/* Age and throughput are explicit so the budget trace distinguishes
 * successful page outs from queued texture ranges. */
bool hzn_heap_sparse_cold_open(const char *path);
size_t hzn_heap_sparse_cold_park(void *address, size_t bytes);

enum {
    COLD_PAGE = 64 * 1024,
    COLD_GRACE = 30,
    COLD_QUEUE = 4096,
    COLD_BYTES_PER_PRESENT = 8 * 1024 * 1024,
};

typedef struct {
    uint64_t first, end, ready_present;
} ColdTextureRange;

static ColdTextureRange cold_textures[COLD_QUEUE];
static size_t cold_head, cold_count;
static uint64_t cold_present, cold_seen, cold_queued, cold_parked, cold_dropped;
static bool cold_enabled;

static void cold_initialize(void)
{
#ifdef _WIN32
    char path[MAX_PATH];
    const DWORD length = GetEnvironmentVariableA("POKOPIA_PC_COLD_STORE", path, sizeof path);
    if (!length || length >= sizeof path)
        return;
#else
    char path[96];
    const int length = snprintf(path, sizeof path, "/data/pokopia-ps4-texture-pages-%d.tmp", (int)getpid());
    if (length <= 0 || (size_t)length >= sizeof path)
        return;
#endif
    cold_enabled = hzn_heap_sparse_cold_open(path);
    fprintf(probe_log, "PC cold texture backing open=%u\n", (unsigned)cold_enabled);
    fflush(probe_log);
}

static void register_cold_texture(const NvnFrontTextureCreateInfo *texture)
{
    const uint64_t address = texture->storage_address, size = texture->storage_size;
    const uint64_t mask = COLD_PAGE - 1;
    if (!cold_enabled || address > UINT64_MAX - mask || size > UINT64_MAX - address)
        return;
    const uint64_t first = (address + mask) & ~mask, end = (address + size) & ~mask;
    if (first >= end)
        return;
    ++cold_seen;
    if (cold_count == COLD_QUEUE) {
        cold_dropped += end - first;
        return;
    }
    ColdTextureRange *range = &cold_textures[(cold_head + cold_count) % COLD_QUEUE];
    *range = (ColdTextureRange){first, end, cold_present + COLD_GRACE};
    cold_queued += end - first;
    ++cold_count;
}

static void retire_cold_textures(void)
{
    ++cold_present;
    if (!cold_enabled)
        return;
    size_t remaining = COLD_BYTES_PER_PRESENT;
    while (cold_count && remaining) {
        ColdTextureRange *range = &cold_textures[cold_head];
        if (range->ready_present > cold_present)
            break;
        const uint64_t available = range->end - range->first;
        const size_t count = available < remaining ? (size_t)available : remaining;
        cold_parked += hzn_heap_sparse_cold_park((void *)(uintptr_t)range->first, count);
        remaining -= count;
        range->first += count;
        if (range->first == range->end) {
            cold_head = (cold_head + 1) % COLD_QUEUE;
            --cold_count;
        }
    }
    if (cold_present % 128 == 0) {
        fprintf(probe_log, "PC cold textures present=%" PRIu64 " seen=%" PRIu64 " waiting=%zu"
            " scheduled=%" PRIu64 " parked=%" PRIu64 " dropped=%" PRIu64 "\n",
            cold_present, cold_seen, cold_count, cold_queued, cold_parked, cold_dropped);
        fflush(probe_log);
    }
}
