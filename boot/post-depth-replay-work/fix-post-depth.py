exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('nvn_replay.c', [
    ('(!indexed && x[2] > INT32_MAX)', '(!indexed && (uint32_t)x[2] > INT32_MAX)'),
    ('    const size_t bytes = !indexed ? 16 : !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);',
     '    if (!indexed)\n        return append_metered(replay, r, operation, 16, 0);\n    const size_t bytes = !base_vertex ? 24 : (operation.data.draw.state.base_vertex ? 72 : 48);'),
])
edit('live_draw.c', [
    ('''        draw->extent.height > adapter->properties.limits.maxFramebufferHeight)''',
     '''        draw->extent.height > adapter->properties.limits.maxFramebufferHeight ||
        (target->extent.width && target->extent.width != draw->extent.width) ||
        (target->extent.height && target->extent.height != draw->extent.height))'''),
    ('''    const bool needs_fragment_uniform = !strcmp(fragment, "7587f31f1945afaf") || !strcmp(fragment, "ff03fa7b63a79526");
    const bool tonemap = !strcmp(fragment, "ff03fa7b63a79526");''',
     '''    const bool tonemap = !strcmp(fragment, "ff03fa7b63a79526");
    const bool needs_fragment_uniform = tonemap || !strcmp(fragment, "7587f31f1945afaf");'''),
    ('''            target->extent.width != draw->extent.width || target->extent.height != draw->extent.height ||
            samples[0].texture''', '''            samples[0].texture'''),
])
