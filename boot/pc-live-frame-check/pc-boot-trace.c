/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc-boot-trace.h"
#include "cmif.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <inttypes.h>
#include <string.h>

enum { TRACE_THREADS = 128, TRACE_IPCS = 192, TRACE_PRESENCE_INTERVAL = 240 };

typedef struct {
    uint64_t tls, caller, fp, arg0, arg1, start;
    uint32_t svc;
} PcWait;

typedef struct {
    uint32_t service, handle, type, object, command;
    uint64_t tls, count, last, reported;
} PcIpc;

static SRWLOCK wait_lock = SRWLOCK_INIT;
static PcWait waits[TRACE_THREADS];
/* IPC, graphics and present hooks all hold the existing guest kernel lock. */
static PcIpc ipcs[TRACE_IPCS];
static size_t ipc_kinds;
static volatile LONG packet_entries, packet_prepared, packet_skipped;
static bool trace_texture_tail;
static unsigned texture_tail_calls, texture_tail_presents;

void pokopia_pc_trace_texture_tail(FILE *log, uint64_t storage)
{
    if (storage != UINT64_C(0x78ac196600) || trace_texture_tail)
        return;
    trace_texture_tail = true;
    fprintf(log, "PC native-tail trigger guest texture %#" PRIx64 "\n", storage);
    fflush(log);
}

void pokopia_pc_trace_nvn_tail(FILE *log, const NvnFrontReply *reply, uint64_t lr, uint64_t tls)
{
    if (!trace_texture_tail || texture_tail_presents >= 4 || texture_tail_calls >= 256 || !reply->name[0])
        return;
    fprintf(log, "PC native-tail call %u %s result=%u lr=%#" PRIx64 " tls=%#" PRIx64 "\n",
            ++texture_tail_calls, reply->name, (unsigned)reply->kind, lr, tls);
    fflush(log);
}

void pokopia_pc_trace_packet(FILE *log, uint64_t site, const uint64_t x[32])
{
    if (site == UINT64_C(0x71017b7b50)) {
        LONG hit = InterlockedIncrement(&packet_entries);
        if (hit <= 12)
            fprintf(log, "PC loading packet entry=%ld context=%#" PRIx64 " first=%" PRIu32 " count=%" PRIu32 "\n",
                    hit, x[0], (uint32_t)x[4], (uint32_t)x[5]);
        return;
    }
    const bool prepared = (uint8_t)x[0] != 0;
    LONG hit = InterlockedIncrement(prepared ? &packet_prepared : &packet_skipped);
    if (hit <= 12)
        fprintf(log, "PC loading packet prepared=%u hit=%ld context=%#" PRIx64 " first=%" PRIu32 " count=%" PRIu32 "\n",
                (unsigned)prepared, hit, x[19], (uint32_t)x[21], (uint32_t)x[20]);
}

void pokopia_pc_trace_program(const PokopiaVkProgram *program, NvnFrontProgramLinkResult result, FILE *log)
{
    static unsigned observed;
    if (observed >= 12)
        return;
    PokopiaVkProgramStage vertex = {0}, fragment = {0};
    pokopia_vk_program_stage(program, NVN_SHADER_VERTEX, &vertex);
    pokopia_vk_program_stage(program, NVN_SHADER_FRAGMENT, &fragment);
    if ((!vertex.hash || strcmp(vertex.hash, "a26763a4bdd7b361")) &&
        (!fragment.hash || strcmp(fragment.hash, "2db3327d9fbc5bb3")))
        return;
    ++observed;
    fprintf(log, "PC loading program linked result=%d vs=%s cache=%u fs=%s cache=%u\n", result,
            vertex.hash ? vertex.hash : "-", (unsigned)(vertex.resources != NULL),
            fragment.hash ? fragment.hash : "-", (unsigned)(fragment.resources != NULL));
    fflush(log);
}

void pokopia_pc_trace_draw_phase(const char *hash, unsigned phase, uint64_t target, NvnFrontReplyKind result)
{
    static unsigned counts[3];
    static const char *const names[] = {"record", "submit-validate", "submit-execute"};
    if (phase >= sizeof names / sizeof names[0] || counts[phase] >= 12 || !hash || strcmp(hash, "2db3327d9fbc5bb3"))
        return;
    ++counts[phase];
    printf("PC loading draw phase=%s result=%d target=%#" PRIx64 "\n", names[phase], result, target);
    fflush(stdout);
}

void pokopia_pc_trace_ipc(uint32_t service, uint32_t handle, uint64_t tls_address, const uint8_t *tls)
{
    HznCmifRequest request;
    const size_t size = 0x100;
    if (!hzn_cmif_parse(tls, size, &request) && !hzn_cmif_parse_with_nonsecure_output(tls, size, &request) &&
        !hzn_cmif_parse_with_input_output(tls, size, &request) && !hzn_cmif_parse_with_auto_output(tls, size, &request))
        return;
    const uint8_t *raw = request.raw;
    uint32_t object = 0, command;
    if (request.raw_size >= 16 && hzn_cmif_read32(raw) == HZN_SFCI) {
        command = hzn_cmif_read32(raw + 8);
    } else if (request.raw_size >= 32 && raw[0] == 1 && hzn_cmif_read32(raw + 16) == HZN_SFCI) {
        object = hzn_cmif_read32(raw + 4);
        command = hzn_cmif_read32(raw + 24);
    } else {
        return;
    }
    size_t i;
    for (i = 0; i < ipc_kinds; ++i) {
        if (ipcs[i].service == service && ipcs[i].command == command && ipcs[i].object == object)
            break;
    }
    if (i == ipc_kinds) {
        if (ipc_kinds == TRACE_IPCS)
            return;
        ++ipc_kinds;
        ipcs[i] = (PcIpc){.service = service, .type = request.type, .object = object, .command = command};
    }
    PcIpc *ipc = &ipcs[i];
    ipc->handle = handle;
    ipc->tls = tls_address;
    ipc->last = GetTickCount64();
    ++ipc->count;
}

void pokopia_pc_trace_wait_start(uint64_t tls, uint32_t svc, uint64_t caller, uint64_t fp, uint64_t arg0, uint64_t arg1)
{
    if (!tls)
        return;
    AcquireSRWLockExclusive(&wait_lock);
    for (size_t i = 0; i < TRACE_THREADS; ++i) {
        if (!waits[i].tls || waits[i].tls == tls) {
            waits[i] = (PcWait){tls, caller, fp, arg0, arg1, GetTickCount64(), svc};
            break;
        }
    }
    ReleaseSRWLockExclusive(&wait_lock);
}

void pokopia_pc_trace_wait_end(uint64_t tls)
{
    if (!tls)
        return;
    AcquireSRWLockExclusive(&wait_lock);
    for (size_t i = 0; i < TRACE_THREADS; ++i) {
        if (waits[i].tls == tls) {
            waits[i] = (PcWait){0};
            break;
        }
    }
    ReleaseSRWLockExclusive(&wait_lock);
}

static uint64_t read_word(PcReadGuest read_guest, uint64_t address, size_t size)
{
    uint64_t value = 0;
    const void *guest = address ? read_guest(NULL, address, size) : NULL;
    if (guest)
        memcpy(&value, guest, size);
    return value;
}

static void trace_dispatcher(FILE *log, PcReadGuest read_guest, uint64_t presents)
{
    static uint64_t last_object, last_state, last_gate, last_ready, last_secondary;
    /* The main frame callback at 0x7100bfc194 passes this global to 0x7100230b00. */
    const uint64_t object = read_word(read_guest, read_word(read_guest, UINT64_C(0x71034840e8), 8), 8);
    if (!object)
        return;
    const uint64_t state = read_word(read_guest, object + 0x84828, 4);
    const uint64_t gate_object = read_word(read_guest, read_word(read_guest, UINT64_C(0x7103483b58), 8), 8);
    const uint64_t gate = gate_object ? read_word(read_guest, gate_object + 144, 1) : 0;
    uint64_t ready = 0;
    if (state == 8) {
        const uint64_t loader = read_word(read_guest, read_word(read_guest, UINT64_C(0x7103484618), 8), 8);
        const uint64_t work = loader ? read_word(read_guest, loader + 16, 8) : 0;
        ready = work ? read_word(read_guest, work + 40, 1) : 0;
    }
    const uint64_t secondary = read_word(read_guest, read_word(read_guest, UINT64_C(0x7103483fd8), 8), 8);
    const uint64_t secondary_state = secondary ? read_word(read_guest, secondary + 52, 1) : 0;
    if (last_object == object && last_state == state && last_gate == gate && last_ready == ready &&
        last_secondary == secondary_state &&
        presents % TRACE_PRESENCE_INTERVAL)
        return;
    last_object = object;
    last_state = state;
    last_gate = gate;
    last_ready = ready;
    last_secondary = secondary_state;
    fprintf(log, "PC boot dispatcher present=%" PRIu64 " object=%#" PRIx64 " state=%" PRIu64
                 " alternate=%#" PRIx64 " state8_ready=%" PRIu64 " secondary=%#" PRIx64 " secondary_state=%" PRIu64
                 "\n", presents, object, state, gate, ready, secondary, secondary_state);
    fflush(log);
}

static size_t guest_callers(PcReadGuest read_guest, uint64_t frame, uint64_t callers[12])
{
    size_t count = 0;
    while (count < 12 && !(frame & 7) && frame >= UINT64_C(0x7c00000000) && frame < UINT64_C(0x7d00000000)) {
        uint64_t saved[2];
        const void *guest = read_guest(NULL, frame, sizeof saved);
        if (!guest)
            break;
        memcpy(saved, guest, sizeof saved);
        callers[count++] = saved[1];
        if (saved[0] <= frame || saved[0] - frame > 0x200000)
            break;
        frame = saved[0];
    }
    return count;
}

static void trace_callers(FILE *log, const uint64_t *callers, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        fprintf(log, "%c%#" PRIx64, i ? ',' : ' ', callers[i]);
    fputc('\n', log);
}

void pokopia_pc_trace_present(FILE *log, PcReadGuest read_guest, uint64_t tls, uint64_t lr, uint64_t fp)
{
    static uint64_t presents;
    ++presents;
    if (trace_texture_tail && texture_tail_presents < 4) {
        fprintf(log, "PC native-tail present=%" PRIu64 " number=%u\n", presents, ++texture_tail_presents);
        fflush(log);
    }
    trace_dispatcher(log, read_guest, presents);
    if (presents % TRACE_PRESENCE_INTERVAL)
        return;
    uint64_t now = GetTickCount64();
    fprintf(log, "PC boot phase present=%" PRIu64 " IPC kinds=%zu loading packets=%ld prepared=%ld skipped=%ld\n",
            presents, ipc_kinds, InterlockedCompareExchange(&packet_entries, 0, 0),
            InterlockedCompareExchange(&packet_prepared, 0, 0), InterlockedCompareExchange(&packet_skipped, 0, 0));
    for (size_t i = 0; i < ipc_kinds; ++i) {
        PcIpc *ipc = &ipcs[i];
        const uint64_t delta = ipc->count - ipc->reported;
        ipc->reported = ipc->count;
        if (!delta)
            continue;
        fprintf(log, "PC boot IPC service=%" PRIu32 " handle=%#" PRIx32 " type=%" PRIu32
                " object=%#" PRIx32 " cmd=%#" PRIx32 " tls=%#" PRIx64 " count=%" PRIu64 " delta=%" PRIu64
                " ago_ms=%" PRIu64 "\n", ipc->service, ipc->handle, ipc->type, ipc->object, ipc->command,
                ipc->tls, ipc->count, delta, now - ipc->last);
    }
    uint64_t callers[12];
    fprintf(log, "PC boot presenter tls=%#" PRIx64 " lr=%#" PRIx64 " frames=", tls, lr);
    trace_callers(log, callers, guest_callers(read_guest, fp, callers));
    PcWait pending[TRACE_THREADS];
    AcquireSRWLockShared(&wait_lock);
    memcpy(pending, waits, sizeof pending);
    ReleaseSRWLockShared(&wait_lock);
    unsigned unclassified = 0;
    for (size_t i = 0; i < TRACE_THREADS; ++i) {
        const PcWait *wait = &pending[i];
        if (!wait->tls || wait->start > now || now - wait->start < 1500)
            continue;
        const size_t count = guest_callers(read_guest, wait->fp, callers);
        size_t frame = 0;
        while (frame < count && (callers[frame] < UINT64_C(0x7100000000) || callers[frame] >= UINT64_C(0x7103000000)))
            ++frame;
        if (frame == count && unclassified++ >= 6)
            continue;
        fprintf(log, "PC boot wait svc=%#" PRIx32 " tls=%#" PRIx64 " lr=%#" PRIx64
                " arg0=%#" PRIx64 " arg1=%#" PRIx64 " pending_ms=%" PRIu64 " frames=",
                wait->svc, wait->tls, wait->caller, wait->arg0, wait->arg1, now - wait->start);
        trace_callers(log, callers, count);
    }
    fflush(log);
}
