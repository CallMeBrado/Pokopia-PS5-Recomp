from pathlib import Path

def edit(name, pairs):
    p = Path(name)
    s = p.read_text()
    for a, b in pairs:
        if s.count(a) != 1:
            raise ValueError((name, a, s.count(a)))
        s = s.replace(a, b)
    p.write_text(s)

edit('nvn_front.h', [
    ('    bool linked;\n} NvnFrontProgramInfo;', '    bool linked, has_unavailable_shader;\n} NvnFrontProgramInfo;'),
    ('    NVN_FRONT_PROGRAM_FAILED,\n', '    NVN_FRONT_PROGRAM_FAILED,\n    NVN_FRONT_PROGRAM_CACHE_MISS,\n'),
])
edit('nvn_front.c', [
    ('''    if (result != NVN_FRONT_PROGRAM_LINKED && result != NVN_FRONT_PROGRAM_FAILED)
        return NVN_FRONT_BOUNDARY;
    program->info.linked = result == NVN_FRONT_PROGRAM_LINKED && count;
    x[0] = result == NVN_FRONT_PROGRAM_LINKED;''',
     '''    if (result != NVN_FRONT_PROGRAM_LINKED && result != NVN_FRONT_PROGRAM_FAILED &&
        result != NVN_FRONT_PROGRAM_CACHE_MISS)
        return NVN_FRONT_BOUNDARY;
    program->info.linked = result == NVN_FRONT_PROGRAM_LINKED && count;
    program->info.has_unavailable_shader = result == NVN_FRONT_PROGRAM_CACHE_MISS;
    /* A verified cache miss does not invalidate the guest's shader data. */
    x[0] = result != NVN_FRONT_PROGRAM_FAILED;'''),
])
edit('vk_program.h', [
    ('''bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info);''',
     '''/* Registered stages retain their hash. Resources are null on a cache miss. */
bool pokopia_vk_program_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                             PokopiaVkProgramStage *info);
bool pokopia_vk_program_require_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                                     PokopiaVkProgramStage *info);'''),
])
edit('vk_program.c', [
    ('''    if (!program || !info || (unsigned)stage > NVN_SHADER_COMPUTE || !program->stages[stage].cache)''',
     '''    if (!program || !info || (unsigned)stage > NVN_SHADER_COMPUTE || !program->stages[stage].hash[0])'''),
    ('''bool pokopia_vk_program_acquire_module(''', '''bool pokopia_vk_program_require_stage(const PokopiaVkProgram *program, NvnShaderStage stage,
                                     PokopiaVkProgramStage *info)
{
    if (!pokopia_vk_program_stage(program, stage, info))
        return false;
    if (info->resources)
        return true;
    if (program->log) {
        fprintf(program->log, "NVN shader cache miss at use key=%s stage=%u\\n", info->hash, (unsigned)stage);
        fflush(program->log);
    }
    return false;
}

bool pokopia_vk_program_acquire_module('''),
    ('''    const ProgramStage *linked = &program->stages[stage];
    if (!linked->cache)
        return false;''',
     '''    PokopiaVkProgramStage info;
    if (!pokopia_vk_program_require_stage(program, stage, &info))
        return false;
    const ProgramStage *linked = &program->stages[stage];'''),
    ('''    const VkShaderModuleCreateInfo info = {''','''    const VkShaderModuleCreateInfo create_info = {'''),
    ('''    VkResult result = program->create(program->device, &info, NULL, &module);''',
     '''    VkResult result = program->create(program->device, &create_info, NULL, &module);'''),
    ('''    NvnFrontProgramLinkResult outcome = NVN_FRONT_PROGRAM_FAILED;
    for''','''    NvnFrontProgramLinkResult outcome = NVN_FRONT_PROGRAM_FAILED;
    bool incomplete = false;
    for'''),
    ('''linked[shader->parsed.stage].cache)''', '''linked[shader->parsed.stage].hash[0])'''),
    ('''        if (status != NVN_SHADER_CACHE_OK) {
            fprintf(log, "NVN shader cache result=%d key=%s stage=%u\\n", status, cached.hash, (unsigned)shader->parsed.stage);
            nvn_cached_shader_release(&cached);
            if (status == NVN_SHADER_CACHE_MISSING)
                outcome = NVN_FRONT_PROGRAM_UNSUPPORTED;
            goto done;
        }
        ProgramStage *stage = &linked[shader->parsed.stage];
        stage->cache = cache;''',
     '''        if (status != NVN_SHADER_CACHE_OK && log)
            fprintf(log, "NVN shader cache result=%d key=%s stage=%u\\n", status, cached.hash, (unsigned)shader->parsed.stage);
        if (status != NVN_SHADER_CACHE_OK && status != NVN_SHADER_CACHE_MISSING) {
            nvn_cached_shader_release(&cached);
            goto done;
        }
        ProgramStage *stage = &linked[shader->parsed.stage];
        if (status == NVN_SHADER_CACHE_OK)
            stage->cache = cache;
        else
            incomplete = true;'''),
    ('''    outcome = NVN_FRONT_PROGRAM_LINKED;

done:
    if (outcome != NVN_FRONT_PROGRAM_UNSUPPORTED)
        release_stages(program->stages);
    if (outcome == NVN_FRONT_PROGRAM_LINKED)''',
     '''    outcome = incomplete ? NVN_FRONT_PROGRAM_CACHE_MISS : NVN_FRONT_PROGRAM_LINKED;

done:
    release_stages(program->stages);
    if (outcome != NVN_FRONT_PROGRAM_FAILED)'''),
    ('''    fflush(log);
    return outcome;''', '''    if (log)
        fflush(log);
    return outcome;'''),
])
