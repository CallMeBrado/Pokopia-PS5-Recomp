/* Scratch guest resource tracing. These observations never change guest state. */
static _Thread_local uint64_t traced_texcontext;
static _Thread_local unsigned traced_texcontext_attempts;

static uint64_t trace_u64(uint64_t address)
{
    const void *p = read_guest(NULL, address, sizeof(uint64_t));
    return p ? rd64((uint64_t)(uintptr_t)p) : 0;
}

static bool trace_texcontext(Ctx *context, uint64_t address)
{
    const uint64_t *x = context->x;
    if (address == UINT64_C(0x7101ff6270)) {
        const uint64_t entry = x[22];
        const uint64_t type_id = trace_u64(entry + 8);
        if (type_id != UINT64_C(0xc268437afbec60c) || traced_texcontext_attempts >= 3)
            return false;
        ++traced_texcontext_attempts;
        traced_texcontext = entry;
        fprintf(probe_log, "TexContext trace begin dep=%#" PRIx64 " tag=%#" PRIx64
                " manager=%#" PRIx64 " provider=%#" PRIx64 " creator=%#" PRIx64 "\n",
                entry, type_id, x[19], x[27], x[8]);
        return true;
    }
    if (!traced_texcontext)
        return false;
    if (address == UINT64_C(0x7101ff6278) && x[22] == traced_texcontext) {
        fprintf(probe_log, "TexContext trace result dep=%#" PRIx64 " backend=%#" PRIx64 "\n",
                traced_texcontext, x[0]);
        pokopia_boot_report_heap(probe_log, "exact TexContext result");
        pokopia_vk_ledger_report(probe_log, "exact TexContext result");
        fflush(probe_log);
        traced_texcontext = 0;
        return true;
    }
    const char *event = NULL;
    switch (address) {
    case 0x710089dc14: event = "create decoded texture"; break;
    case 0x710089dc18: event = "decoded texture result"; break;
    case 0x710089dc40: event = "allocate wrapper"; break;
    case 0x710089dc44: event = "wrapper result"; break;
    case 0x710179ce2c: event = "parse texture header result"; break;
    case 0x710179ce50: event = "seek texture payload result"; break;
    case 0x710179ce64: case 0x710179ce84: event = "allocate texture structures result"; break;
    case 0x710179ceb0: event = "initialize textures result"; break;
    case 0x710179cd04: case 0x710179cd58: event = "allocate texture structures"; break;
    case 0x710179cd08: case 0x710179cd5c: event = "texture structures allocator result"; break;
    case 0x710179c7d4: case 0x710179c854: case 0x710179c8c0: case 0x710179c8f4: event = "read texture bytes"; break;
    case 0x710179c7d8: case 0x710179c858: case 0x710179c8c4: case 0x710179c8f8: event = "read texture bytes result"; break;
    case 0x7101797e60: event = "guest texture pool allocator"; break;
    case 0x7101797e64: event = "guest texture pool allocator result"; break;
    case 0x710179cb8c: case 0x710179ca0c: event = "texture data operation"; break;
    case 0x710179ca10: case 0x710179ca9c: case 0x710179cacc: case 0x710179caf4:
    case 0x710179cb34: case 0x710179cb58: case 0x710179cb90: case 0x710179cbc0:
        event = "texture data operation result";
        break;
    }
    if (!event)
        return false;
    fprintf(probe_log, "TexContext trace %#" PRIx64 " %s x0=%#" PRIx64 " x1=%#" PRIx64
            " x2=%#" PRIx64 " x3=%#" PRIx64 " x4=%#" PRIx64 " x5=%#" PRIx64
            " x8=%#" PRIx64 " x19=%#" PRIx64 " x21=%#" PRIx64 " x22=%#" PRIx64 "\n",
            address, event, x[0], x[1], x[2], x[3], x[4], x[5], x[8], x[19], x[21], x[22]);
    if (address == 0x7101797e60 || address == 0x710179cd04 || address == 0x710179cd58 || address == 0x710089dc40) {
        fprintf(probe_log, "TexContext trace allocator %#" PRIx64 " words"
                " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 "\n",
                x[0], trace_u64(x[0]), trace_u64(x[0]+8), trace_u64(x[0]+16),
                trace_u64(x[0]+24), trace_u64(x[0]+32), trace_u64(x[0]+40));
    }
    if (address == 0x710179ce2c) {
        fprintf(probe_log, "TexContext trace parsed header words %#" PRIx64 " %#" PRIx64 " %#" PRIx64 " %#" PRIx64 "\n",
                trace_u64(x[31]+4), trace_u64(x[31]+12), trace_u64(x[31]+20), trace_u64(x[31]+28));
    }
    return true;
}

