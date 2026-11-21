exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('live_draw.h', [
    ('typedef struct {\n    PokopiaVkTexture *target, *depth_target;', '''enum { POKOPIA_LIVE_DRAW_SAMPLE_COUNT = 2 };

typedef struct {
    PokopiaVkTexture *target, *depth_target;'''),
    ('    PokopiaLiveDrawSample fragment_sample;', '    PokopiaLiveDrawSample fragment_samples[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];'),
    ('    uint32_t index_count,', '    uint32_t vertex_count, first_vertex;\n    uint32_t index_count,'),
    ('Submit a supported indexed pipeline', 'Submit a supported draw'),
    ('pokopia_live_draw_indexed','pokopia_live_draw'),
])
p=Path('nvn_replay.c')
p.write_text(p.read_text().replace('IndexedDraw','Draw'))
edit('nvn_replay.c', [
    ('''        if (linked && stage == NVN_SHADER_VERTEX && strcmp(info.hash, "821538a4398f85a7"))''','''        if (linked && stage == NVN_SHADER_VERTEX && strcmp(info.hash, "821538a4398f85a7") &&
            strcmp(info.hash, "70b2ae4bef0dfe46"))'''),
    ('''            strcmp(info.hash, "9f3f4772c2ad7edc") && strcmp(info.hash, "42c917e6eb2c6411"))''','''            strcmp(info.hash, "9f3f4772c2ad7edc") && strcmp(info.hash, "42c917e6eb2c6411") &&
            strcmp(info.hash, "1c913e7b97e7e999") && strcmp(info.hash, "ff03fa7b63a79526") &&
            strcmp(info.hash, "c4664b38f6a87835"))'''),
    ('''static NvnFrontReplyKind record_indexed_draw(NvnReplay *replay, const uint64_t x[32], bool base_vertex, bool instanced)''','''static NvnFrontReplyKind record_draw(NvnReplay *replay, const uint64_t x[32], bool indexed,
                                     bool base_vertex, bool instanced)'''),
    ('''    if (!r || count > INT32_MAX)
        return NVN_FRONT_INVALID_GUEST;
    if (!count || (instanced''','''    if (!r || count > INT32_MAX || (!indexed && x[2] > INT32_MAX))
        return NVN_FRONT_INVALID_GUEST;
    if (!count || (instanced'''),
    ('''modes.depth_mode != NVN_FRONT_DEPTH_NEAR_IS_ZERO || (uint32_t)x[1] != 4 || (uint32_t)x[2] != 1 ||''','''modes.depth_mode != NVN_FRONT_DEPTH_NEAR_IS_ZERO || (uint32_t)x[1] != 4 ||
        (indexed && (uint32_t)x[2] != 1) ||'''),
    ('''        !r->viewport_set || !r->scissor_set || r->attribute_count < 3 || !r->stream_count ||
        r->streams[0].divisor || r->scissor[2] <= 0 || r->scissor[3] <= 0 ||''','''        !r->viewport_set || !r->scissor_set || r->scissor[2] <= 0 || r->scissor[3] <= 0 ||
        (indexed && (r->attribute_count < 3 || !r->stream_count || r->streams[0].divisor)) ||'''),
    ('''        .vertex = r->vertex[0], .index = {x[4], (size_t)count * 2},''','''        .vertex = r->vertex[0], .index = indexed ? (BufferRange){x[4], (size_t)count * 2} : (BufferRange){0},'''),
    ('''            .vertex_stride = r->streams[0].stride & 0xfff, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,''','''            .vertex_stride = indexed ? r->streams[0].stride & 0xfff : 0, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,'''),
    ('''            .index_count = count, .base_vertex = base_vertex ? (int32_t)x[5] : 0,''','''            .vertex_count = indexed ? 0 : count, .first_vertex = indexed ? 0 : (uint32_t)x[2],
            .index_count = indexed ? count : 0, .base_vertex = base_vertex ? (int32_t)x[5] : 0,'''),
    ('''    for (unsigned i = 0; i < r->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute''','''    for (unsigned i = 0; indexed && i < r->attribute_count; ++i) {
        const NvnFrontVertexAttribInfo *attribute'''),
    ('''    const size_t bytes = !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);''','''    const size_t bytes = !indexed ? 16 : !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);'''),
    ('''    int fragment_texture;
} DrawAbi;''','''    int fragment_slots[POKOPIA_LIVE_DRAW_SAMPLE_COUNT];
} DrawAbi;'''),
    ('''    *abi = (DrawAbi){.fragment_texture = -1};''','''    *abi = (DrawAbi){.fragment_slots = {-1, -1}};'''),
    ('''            const char suffix[] = "]=s2b128";''','''            const char suffix[] = "]=s2b";'''),
    ('''            if (stage != NVN_SHADER_FRAGMENT || abi->fragment_texture >= 0 ||
                length <= prefix_length + suffix_length || strncmp(token, prefix, prefix_length))''','''            if (stage != NVN_SHADER_FRAGMENT ||
                length <= prefix_length + suffix_length + 3 || strncmp(token, prefix, prefix_length))'''),
    ('''            if (!digits || digits > 2 || length != prefix_length + digits + suffix_length ||
                strncmp(number + digits, suffix, suffix_length))
                return false;
            const unsigned word = (unsigned)strtoul(number, NULL, 10);
            if (word < 8 || (word - 8) % 2 || (word - 8) / 2 >= TEXTURE_SLOTS)
                return false;
            abi->fragment_texture = (int)(word - 8) / 2;''','''            if (!digits || digits > 2 || length != prefix_length + digits + suffix_length + 3 ||
                strncmp(number + digits, suffix, suffix_length))
                return false;
            const char *binding = number + digits + suffix_length;
            const unsigned word = (unsigned)strtoul(number, NULL, 10);
            if (word < 8 || (word - 8) % 2 || (word - 8) / 2 >= TEXTURE_SLOTS ||
                binding[0] != '1' || binding[1] != '2' || (binding[2] != '8' && binding[2] != '9'))
                return false;
            const unsigned sample = (unsigned)(binding[2] - '8');
            if (abi->fragment_slots[sample] >= 0)
                return false;
            abi->fragment_slots[sample] = (int)(word - 8) / 2;'''),
    ('''        if (abi.fragment_texture >= 0 &&
            !resolve_fragment_sample(config->front, device, record, (unsigned)abi.fragment_texture, &draw.fragment_sample))
            return NVN_FRONT_BOUNDARY;''','''        for (unsigned sample = 0; sample < POKOPIA_LIVE_DRAW_SAMPLE_COUNT; ++sample) {
            const int slot = abi.fragment_slots[sample];
            if (slot >= 0 &&
                !resolve_fragment_sample(config->front, device, record, (unsigned)slot, &draw.fragment_samples[sample]))
                return NVN_FRONT_BOUNDARY;
        }'''),
    ('''    draw.vertices = draw_span(config, device, record->vertex);
    draw.indices = draw_span(config, device, record->index);
    if (!draw.vertices.data || !draw.indices.data)
        return NVN_FRONT_INVALID_GUEST;''','''    if (draw.index_count) {
        draw.vertices = draw_span(config, device, record->vertex);
        draw.indices = draw_span(config, device, record->index);
        if (!draw.vertices.data || !draw.indices.data)
            return NVN_FRONT_INVALID_GUEST;
    }'''),
    ('''    if (!strcmp(name, "nvnCommandBufferDrawElements"))
        return record_indexed_draw(replay, x, false, false);
    if (!strcmp(name, "nvnCommandBufferDrawElementsBaseVertex"))
        return record_indexed_draw(replay, x, true, false);
    if (!strcmp(name, "nvnCommandBufferDrawElementsInstanced"))
        return record_indexed_draw(replay, x, true, true);''','''    if (!strcmp(name, "nvnCommandBufferDrawArrays"))
        return record_draw(replay, x, false, false, false);
    if (!strcmp(name, "nvnCommandBufferDrawElements"))
        return record_draw(replay, x, true, false, false);
    if (!strcmp(name, "nvnCommandBufferDrawElementsBaseVertex"))
        return record_draw(replay, x, true, true, false);
    if (!strcmp(name, "nvnCommandBufferDrawElementsInstanced"))
        return record_draw(replay, x, true, true, true);'''),
])

