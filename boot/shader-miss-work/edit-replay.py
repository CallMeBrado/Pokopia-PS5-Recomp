exec(open('edit.py').read().split("edit('nvn_front.h'", 1)[0])
edit('nvn_replay.c', [
    ('''static bool get_compute_program(NvnFront *front, uint64_t device, uint64_t address,''',
     '''static bool get_program(NvnFront *front, uint64_t device, uint64_t address, NvnFrontProgramInfo *program)
{
    return nvn_front_get_program(front, address, program) && program->device == device &&
           (program->linked || program->has_unavailable_shader);
}

static bool get_compute_program(NvnFront *front, uint64_t device, uint64_t address,'''),
    ('''    return nvn_front_get_program(front, address, program) && program->device == device && program->linked &&
           pokopia_vk_program_stage(program->native, NVN_SHADER_COMPUTE, stage);''',
     '''    return get_program(front, device, address, program) &&
           pokopia_vk_program_require_stage(program->native, NVN_SHADER_COMPUTE, stage);'''),
    ('''        !pokopia_vk_program_stage(program, NVN_SHADER_COMPUTE, &stage) || compute_abi(&stage) == COMPUTE_UNSUPPORTED)''',
     '''        !pokopia_vk_program_stage(program, NVN_SHADER_COMPUTE, &stage) ||
        (stage.resources && compute_abi(&stage) == COMPUTE_UNSUPPORTED))'''),
    ('''static NvnFrontReplyKind bind_program(NvnReplay *replay,''',
     '''static bool supported_graphics_shader(unsigned stage, const char *hash)
{
    if (stage == NVN_SHADER_VERTEX)
        return !strcmp(hash, "821538a4398f85a7") || !strcmp(hash, "70b2ae4bef0dfe46") ||
               !strcmp(hash, "a26763a4bdd7b361");
    if (stage == NVN_SHADER_FRAGMENT)
        return !strcmp(hash, "7587f31f1945afaf") || !strcmp(hash, "9f3f4772c2ad7edc") ||
               !strcmp(hash, "42c917e6eb2c6411") || !strcmp(hash, "1c913e7b97e7e999") ||
               !strcmp(hash, "ff03fa7b63a79526") || !strcmp(hash, "c4664b38f6a87835") ||
               !strcmp(hash, "2db3327d9fbc5bb3");
    return false;
}

static NvnFrontReplyKind bind_program(NvnReplay *replay,'''),
    ('''    NvnFrontProgramInfo program;
    if (!recording || (stages & ~((1u << PROGRAM_STAGES) - 1)) ||
        (x[1] && (!nvn_front_get_program(replay->config.front, x[1], &program) ||
                  program.device != recording->device || !program.linked)))''',
     '''    NvnFrontProgramInfo program = {0};
    if (!recording || (stages & ~((1u << PROGRAM_STAGES) - 1)) ||
        (x[1] && !get_program(replay->config.front, recording->device, x[1], &program)))'''),
    ('''    uint32_t linked_stages = 0;
    for (unsigned stage = 0; stage < PROGRAM_STAGES; ++stage) {
        PokopiaVkProgramStage info;
        const bool linked = x[1] && pokopia_vk_program_stage(program.native, stage, &info);
        const bool requested = (stages & (1u << stage)) != 0;
        if (linked && (!requested || stage > NVN_SHADER_FRAGMENT))
            return NVN_FRONT_BOUNDARY;
        if (linked && stage == NVN_SHADER_VERTEX && strcmp(info.hash, "821538a4398f85a7") &&
            strcmp(info.hash, "70b2ae4bef0dfe46") && strcmp(info.hash, "a26763a4bdd7b361"))
            return NVN_FRONT_BOUNDARY;
        if (linked && stage == NVN_SHADER_FRAGMENT && strcmp(info.hash, "7587f31f1945afaf") &&
            strcmp(info.hash, "9f3f4772c2ad7edc") && strcmp(info.hash, "42c917e6eb2c6411") &&
            strcmp(info.hash, "1c913e7b97e7e999") && strcmp(info.hash, "ff03fa7b63a79526") &&
            strcmp(info.hash, "c4664b38f6a87835") && strcmp(info.hash, "2db3327d9fbc5bb3"))
            return NVN_FRONT_BOUNDARY;
        if (linked)
            linked_stages |= 1u << stage;
        if (requested)
            bytes += linked ? enable_bytes[stage] : disable_bytes[stage];
    }''',
     '''    uint32_t bound_stages = 0;
    for (unsigned stage = 0; stage < PROGRAM_STAGES; ++stage) {
        PokopiaVkProgramStage info;
        const bool registered = x[1] && pokopia_vk_program_stage(program.native, stage, &info);
        const bool requested = (stages & (1u << stage)) != 0;
        if (registered && !program.has_unavailable_shader &&
            (!requested || !supported_graphics_shader(stage, info.hash)))
            return NVN_FRONT_BOUNDARY;
        if (!requested)
            continue;
        if (registered) {
            if (stage > NVN_SHADER_FRAGMENT)
                return NVN_FRONT_BOUNDARY;
            bound_stages |= 1u << stage;
            bytes += enable_bytes[stage];
        } else {
            bytes += disable_bytes[stage];
        }
    }'''),
    ('''recording->program[stage] = linked_stages & (1u << stage) ? x[1] : 0;''',
     '''recording->program[stage] = bound_stages & (1u << stage) ? x[1] : 0;'''),
    ('''static NvnFrontReplyKind record_draw(NvnReplay *replay,''',
     '''static bool get_draw_program(NvnFront *front, uint64_t device, uint64_t address, unsigned stage,
                             NvnFrontProgramInfo *program, PokopiaVkProgramStage *info)
{
    return get_program(front, device, address, program) &&
           pokopia_vk_program_require_stage(program->native, stage, info) &&
           supported_graphics_shader(stage, info->hash);
}

static NvnFrontReplyKind record_draw(NvnReplay *replay,'''),
    ('''    if (!count || (instanced && !(uint32_t)x[7]))
        return NVN_FRONT_HANDLED;
    NvnFrontDeviceModes modes;''',
     '''    if (!count || (instanced && !(uint32_t)x[7]))
        return NVN_FRONT_HANDLED;
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info;
        if (r->program[stage] && !get_draw_program(replay->config.front, r->device, r->program[stage],
                                                   stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
    }
    NvnFrontDeviceModes modes;'''),
    ('''        if (!nvn_front_get_program(config->front, record->program[stage], &program) ||
            program.device != device || !program.linked || !pokopia_vk_program_stage(program.native, stage, &info))''',
     '''        if (!get_draw_program(config->front, device, record->program[stage], stage, &program, &info))'''),
])
