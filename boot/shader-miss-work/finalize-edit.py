exec(open('edit.py').read().split("edit('nvn_front.h'", 1)[0])
edit('shader_cache.c', [
    ('''    if (!status.length)
        return NVN_SHADER_CACHE_INVALID_CACHE;
    if (!field_is(status, "ok") && !field_is(status, "ok-retry"))
        return NVN_SHADER_CACHE_MISSING;''',
     '''    if (!field_is(status, "ok") && !field_is(status, "ok-retry")) {
        /* The packed manifest contains compiled shaders only. Loose harness
         * output can also explicitly record a failed translation attempt. */
        const bool untranslated = cache->pack_offset_col == SIZE_MAX &&
            (field_is(status, "invalid") || field_is(status, "exception") || field_is(status, "timeout") ||
             field_is(status, "no-output") || field_is(status, "unsupported"));
        return untranslated ? NVN_SHADER_CACHE_MISSING : NVN_SHADER_CACHE_INVALID_CACHE;
    }'''),
])
edit('nvn_replay.c', [
    ('''static bool get_compute_program(NvnFront *front, uint64_t device, uint64_t address,
                                NvnFrontProgramInfo *program, PokopiaVkProgramStage *stage)
{
    return get_program(front, device, address, program) &&
           pokopia_vk_program_require_stage(program->native, NVN_SHADER_COMPUTE, stage);
}''',
     '''static bool require_program_stage(NvnFront *front, uint64_t device, uint64_t address, NvnShaderStage stage,
                                  NvnFrontProgramInfo *program, PokopiaVkProgramStage *info)
{
    return get_program(front, device, address, program) &&
           pokopia_vk_program_require_stage(program->native, stage, info);
}'''),
    ('''static bool get_draw_program(NvnFront *front, uint64_t device, uint64_t address, unsigned stage,
                             NvnFrontProgramInfo *program, PokopiaVkProgramStage *info)
{
    return get_program(front, device, address, program) &&
           pokopia_vk_program_require_stage(program->native, stage, info) &&
           supported_graphics_shader(stage, info->hash);
}

''',''),
    ('''        if (r->program[stage] && !get_draw_program(replay->config.front, r->device, r->program[stage],
                                                   stage, &program, &info))''',
     '''        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &info))'''),
    ('''    if (!get_compute_program(replay->config.front, r->device, r->program[NVN_SHADER_COMPUTE], &program, &stage))''',
     '''    if (!require_program_stage(replay->config.front, r->device, r->program[NVN_SHADER_COMPUTE],
                               NVN_SHADER_COMPUTE, &program, &stage))'''),
    ('''    const PokopiaVkProgram *programs[2];
    for (unsigned stage = 0; stage < 2; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info;
        if (!get_draw_program(config->front, device, record->program[stage], stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
        programs[stage] = program.native;
        shaders[stage]->hash = info.hash;
        DrawAbi abi;''',
     '''    const PokopiaVkProgram *programs[2];
    PokopiaVkProgramStage stages[2];
    for (unsigned stage = 0; stage < 2; ++stage) {
        NvnFrontProgramInfo program;
        if (!require_program_stage(config->front, device, record->program[stage], stage, &program, &stages[stage]))
            return NVN_FRONT_BOUNDARY;
        programs[stage] = program.native;
    }
    for (unsigned stage = 0; stage < 2; ++stage) {
        const PokopiaVkProgramStage info = stages[stage];
        if (!supported_graphics_shader(stage, info.hash))
            return NVN_FRONT_BOUNDARY;
        shaders[stage]->hash = info.hash;
        DrawAbi abi;'''),
    ('''    if (!config->compute || !get_compute_program(config->front, device, record->program, &program, &stage))''',
     '''    if (!config->compute || !require_program_stage(config->front, device, record->program, NVN_SHADER_COMPUTE,
                                                   &program, &stage))'''),
])
