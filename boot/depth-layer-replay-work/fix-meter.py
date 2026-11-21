exec(open('edit-depth.py').read().split("edit('vk_adapter.h'", 1)[0])
edit('nvn_replay.c', [
    ('    const bool flush = control_bytes && recording->command_segment;', '    const bool flush = control_bytes && (recording->command_segment || command_bytes);'),
    ('    recording->command_segment = command_bytes || (recording->command_segment && !flush);','    recording->command_segment = !flush && (command_bytes || recording->command_segment);'),
    ('    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), 0) ||\n        (x[4] && !charge_recording(replay, recording, 0, 16)))','    if (!charge_recording(replay, recording, 76 + 32 * present + (x[4] ? 108 : 0), x[4] ? 16 : 0))'),
])
