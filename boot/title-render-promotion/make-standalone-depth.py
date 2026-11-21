import ast
from pathlib import Path

source = Path('patch-depth-only.py').read_text()
for statement in ast.parse(source).body:
    if not isinstance(statement, ast.Expr) or not isinstance(statement.value, ast.Call):
        continue
    call = statement.value
    name, edits = map(ast.literal_eval, call.args)
    path = Path('depth-only') / name
    text = path.read_text()

    if name == 'nvn_replay.c':
        old = '''    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        PokopiaVkProgramStage info;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &info))
            return NVN_FRONT_BOUNDARY;
    }'''
        new = '''    PokopiaVkProgramStage stages[2] = {0};
    for (unsigned stage = NVN_SHADER_VERTEX; stage <= NVN_SHADER_FRAGMENT; ++stage) {
        NvnFrontProgramInfo program;
        if (r->program[stage] && !require_program_stage(replay->config.front, r->device, r->program[stage],
                                                          stage, &program, &stages[stage]))
            return NVN_FRONT_BOUNDARY;
    }'''
        assert text.count(old) == 1
        text = text.replace(old, new)

    for old, new in edits:
        if 'const unsigned uniform_slot = composite' in old:
            old = old.split('\n', 1)[1]
            new = new.split('\n', 1)[1]
        elif 'const bool composite = scene_composite(stages)' in old:
            old = '''    for (unsigned stage = 0; stage < 2; ++stage) {
        const PokopiaVkProgramStage info = stages[stage];'''
            new = '''    for (unsigned stage = 0; stage < stage_count; ++stage) {
        const PokopiaVkProgramStage info = stages[stage];'''
        elif '"caa3cba58abf8cc9"' in old:
            continue
        elif '    if (composite)' in old:
            old = '''    if (!indexed) {
        if (strcmp(fragment, "1c913e7b97e7e999")'''
            new = new[:new.index('    } else if (composite)')] + '''    } else if (!indexed) {
        if (strcmp(fragment, "1c913e7b97e7e999")'''
        assert text.count(old) == 1, (name, old, text.count(old))
        text = text.replace(old, new)
    path.write_text(text)
