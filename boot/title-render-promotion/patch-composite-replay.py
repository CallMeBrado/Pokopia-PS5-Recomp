from pathlib import Path

def edit(filename, replacements):
    path = Path(filename)
    text = path.read_text()
    for old, new in replacements:
        assert text.count(old) == 1, (filename, old, text.count(old))
        text = text.replace(old, new)
    path.write_text(text)

edit('live_draw.h', [
    ('''    PokopiaLiveDrawSpan vertices, indices, vertex_uniform, fragment_uniform;
    PokopiaLiveDrawSpan vertex_constants;''',
     '''    /* Procedural draws have shader constants instead of a vertex stream. */
    union { PokopiaLiveDrawSpan vertices, vertex_constants; };
    PokopiaLiveDrawSpan indices, vertex_uniform, fragment_uniform;''')
])

edit('live_draw.c', [
    ('''        if (indexed || !depth || draw->depth_write || draw->stencil_test || draw->target_layer ||''',
     '''        if (indexed || !depth || draw->depth_write || draw->target_layer ||'''),
    ('''            draw->color_write_mask != rgba || draw->vertex_count != 3 || draw->first_vertex ||
            draw->vertex_stride || draw->vertices.data || draw->vertices.bytes)''',
     '''            draw->color_write_mask != rgba || draw->vertex_count != 3 || draw->first_vertex ||
            draw->vertex_stride)'''),
    ('''    if (draw->vertex_constants.bytes)
        return false;
''', ''),
    ('''    if (draw->vertex_constants.bytes)
        memcpy(vertex_constants, draw->vertex_constants.data, draw->vertex_constants.bytes);''',
     '''    const bool composite = is_scene_composite(draw);
    if (composite)
        memcpy(vertex_constants, draw->vertex_constants.data, draw->vertex_constants.bytes);'''),
    ('''        {vertex_constants, draw->vertex_constants.bytes ? sizeof vertex_constants : 0},''',
     '''        {vertex_constants, composite ? sizeof vertex_constants : 0},'''),
    ('''        {draw->vertices.data, vertex_bytes},''',
     '''        {composite ? NULL : draw->vertices.data, vertex_bytes},''')
])

edit('vk_program.h', [
    ('''    const char *hash, *resources;
} PokopiaVkProgramStage;''',
     '''    const char *hash, *resources;
} PokopiaVkProgramStage;

enum { POKOPIA_COMPOSITE_C1_BYTES = 0x30 };
#define POKOPIA_COMPOSITE_VERTEX "caa3cba58abf8cc9"
#define POKOPIA_COMPOSITE_FRAGMENT "8526214507533768"'''),
    ('''bool pokopia_vk_program_acquire_module''',
     '''/* Borrow retained constant data from the live program until the next link/destroy. */
bool pokopia_vk_program_constants(const PokopiaVkProgram *program, NvnShaderStage stage,
                                 const void **data, size_t *bytes);
bool pokopia_vk_program_acquire_module''')
])

edit('vk_program.c', [
    ('''    char *resources;
} ProgramStage;''',
     '''    char *resources;
    void *constants;
} ProgramStage;'''),
    ('''        free(stages[i].resources);''',
     '''        free(stages[i].resources);
        free(stages[i].constants);'''),
    ('''bool pokopia_vk_program_acquire_module''',
     '''bool pokopia_vk_program_constants(const PokopiaVkProgram *program, NvnShaderStage stage,
                                 const void **data, size_t *bytes)
{
    if (!program || stage != NVN_SHADER_VERTEX || !data || !bytes || !program->stages[stage].constants)
        return false;
    *data = program->stages[stage].constants;
    *bytes = POKOPIA_COMPOSITE_C1_BYTES;
    return true;
}

bool pokopia_vk_program_acquire_module'''),
    ('''        cached.resources = NULL;
        nvn_cached_shader_release(&cached);''',
     '''        cached.resources = NULL;
        nvn_cached_shader_release(&cached);
        if (status == NVN_SHADER_CACHE_OK && !strcmp(stage->hash, POKOPIA_COMPOSITE_VERTEX)) {
            const uint32_t offset = shader->parsed.constants_offset;
            if (shader->parsed.constants_bytes != POKOPIA_COMPOSITE_C1_BYTES ||
                (uint64_t)offset + POKOPIA_COMPOSITE_C1_BYTES > shader->parsed.code_bytes)
                goto done;
            stage->constants = malloc(POKOPIA_COMPOSITE_C1_BYTES);
            if (!stage->constants)
                goto done;
            memcpy(stage->constants, (const uint8_t *)shader->code + offset, POKOPIA_COMPOSITE_C1_BYTES);
        }''')
])

edit('nvn_replay.c', [
    ('''               !strcmp(hash, "a26763a4bdd7b361");''',
     '''               !strcmp(hash, "a26763a4bdd7b361") || !strcmp(hash, POKOPIA_COMPOSITE_VERTEX);'''),
    ('''               !strcmp(hash, "2db3327d9fbc5bb3");''',
     '''               !strcmp(hash, "2db3327d9fbc5bb3") || !strcmp(hash, POKOPIA_COMPOSITE_FRAGMENT);'''),
    ('''static NvnFrontReplyKind record_draw(NvnReplay *replay''',
     '''static bool scene_composite(const PokopiaVkProgramStage stages[2])
{
    return stages[0].hash && stages[1].hash &&
           !strcmp(stages[0].hash, POKOPIA_COMPOSITE_VERTEX) &&
           !strcmp(stages[1].hash, POKOPIA_COMPOSITE_FRAGMENT);
}

static NvnFrontReplyKind record_draw(NvnReplay *replay'''),
    ('''    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
    }
    NvnFrontDeviceModes modes;''',
     '''    PokopiaVkProgramStage stages[2] = {0};
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &stages[stage]))
            return NVN_FRONT_BOUNDARY;
    }
    const bool composite = scene_composite(stages);
    const unsigned uniform_slot = composite ? 2 : 0;
    NvnFrontDeviceModes modes;'''),
    ('''        !r->stream_count || r->streams[0].divisor ||
        (indexed ? r->attribute_count < 3 : r->attribute_count != 1) ||''',
     '''        (composite ? indexed || instanced || count != 3 || (uint32_t)x[2] != 0 :
         (!r->stream_count || r->streams[0].divisor ||
          (indexed ? r->attribute_count < 3 : r->attribute_count != 1))) ||'''),
    ('''        .vertex = r->vertex[0], .index = indexed ? (BufferRange){x[4], (size_t)count * 2} : (BufferRange){0},
        .uniform = {r->uniform[0][0], r->uniform[1][0]},''',
     '''        .vertex = composite ? (BufferRange){0} : r->vertex[0],
        .index = indexed ? (BufferRange){x[4], (size_t)count * 2} : (BufferRange){0},
        .uniform = {r->uniform[0][uniform_slot], r->uniform[1][uniform_slot]},'''),
    ('''            .vertex_stride = r->streams[0].stride & 0xfff, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,''',
     '''            .vertex_stride = composite ? 0 : r->streams[0].stride & 0xfff,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,'''),
    ('''    for (unsigned i = 0; i < r->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute = &r->attributes[i];''',
     '''    const unsigned active_attributes = composite ? 0 : r->attribute_count;
    for (unsigned i = 0; i < active_attributes; ++i) {
        const NvnFrontVertexAttribInfo *attribute = &r->attributes[i];'''),
    ('''    bool uniform0;
    int fragment_slots''',
     '''    bool uniform, vertex_constants;
    int fragment_slots'''),
    ('''static bool draw_abi(const char *resources, unsigned stage, DrawAbi *abi)''',
     '''static bool draw_abi(const char *resources, unsigned stage, bool composite, DrawAbi *abi)'''),
    ('''    const char *uniform = stage == NVN_SHADER_VERTEX ? "cb3=s0b4" : "cb3=s0b22";''',
     '''    const bool vertex = stage == NVN_SHADER_VERTEX;
    const char *uniform = composite ? (vertex ? "cb5=s0b6" : "cb5=s0b24") :
                                      (vertex ? "cb3=s0b4" : "cb3=s0b22");'''),
    ('''        if (length == strlen(uniform) && !strncmp(token, uniform, length) && !abi->uniform0) {
            abi->uniform0 = true;
        } else if (length) {''',
     '''        if (length == strlen(uniform) && !strncmp(token, uniform, length) && !abi->uniform) {
            abi->uniform = true;
        } else if (composite && vertex && length == sizeof "cb1=s0b2" - 1 &&
                   !strncmp(token, "cb1=s0b2", length) && !abi->vertex_constants) {
            abi->vertex_constants = true;
        } else if (length) {'''),
    ('''    return true;
}

static bool resolve_sampled_view''',
     '''    return !composite || (abi->uniform && (vertex ? abi->vertex_constants :
           (abi->fragment_slots[0] == 0 && abi->fragment_slots[1] < 0)));
}

static bool resolve_sampled_view'''),
    ('''        programs[stage] = program.native;
    }
    for (unsigned stage = 0; stage < 2; ++stage) {''',
     '''        programs[stage] = program.native;
    }
    const bool composite = scene_composite(stages);
    for (unsigned stage = 0; stage < 2; ++stage) {'''),
    ('''        if (!draw_abi(info.resources, stage, &abi))
            return NVN_FRONT_BOUNDARY;
        if (abi.uniform0) {''',
     '''        if (!draw_abi(info.resources, stage, composite, &abi))
            return NVN_FRONT_BOUNDARY;
        if (abi.vertex_constants &&
            !pokopia_vk_program_constants(programs[stage], stage, &draw.vertex_constants.data,
                                          &draw.vertex_constants.bytes))
            return NVN_FRONT_BOUNDARY;
        if (abi.uniform) {'''),
    ('''    draw.vertices = draw_span(config, device, record->vertex);
    if (!draw.vertices.data)
        return NVN_FRONT_INVALID_GUEST;''',
     '''    if (!composite) {
        draw.vertices = draw_span(config, device, record->vertex);
        if (!draw.vertices.data)
            return NVN_FRONT_INVALID_GUEST;
    }''')
])
