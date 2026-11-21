from pathlib import Path
from difflib import unified_diff

files = ('live_draw.c', 'live_draw.h', 'nvn_replay.c', 'vk_program.c', 'vk_program.h')
for variant in ('downsample', 'depth-only', 'composite', 'combined'):
    lines = []
    for name in files:
        original = Path('base') / name
        candidate = Path(variant) / name
        if not candidate.exists():
            continue
        before, after = original.read_text().splitlines(keepends=True), candidate.read_text().splitlines(keepends=True)
        old, new = f'a/gfx-bridge/{name}', f'b/gfx-bridge/{name}'
        if before != after:
            lines += [f'diff --git {old} {new}\n', *unified_diff(before, after, old, new)]
    if variant in ('depth-only', 'combined'):
        name = 'loader-probe/loader_probe.c'
        source = Path('../..') / name
        before = source.read_text()
        old = '''                draw->vertex_shader.hash, draw->fragment_shader.hash, draw->extent.width, draw->extent.height,
'''
        new = '''                draw->vertex_shader.hash, draw->fragment_shader.hash ? draw->fragment_shader.hash : "-",
                draw->extent.width, draw->extent.height,
'''
        assert before.count(old) == 1
        after = before.replace(old,new)
        old, new = f'a/{name}', f'b/{name}'
        lines += [f'diff --git {old} {new}\n', *unified_diff(before.splitlines(keepends=True), after.splitlines(keepends=True), old, new)]
    with Path(variant+'.patch').open('w',encoding='utf-8',newline='\n') as output:
        output.writelines(lines)
