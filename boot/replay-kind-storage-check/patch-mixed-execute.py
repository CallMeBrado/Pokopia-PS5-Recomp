from pathlib import Path

p = Path('test-production.c')
s = p.read_text()
s = s.replace('static unsigned fences;', '''static unsigned fences, timestamps;
static uint64_t sample_timestamp(void)
{
    assert(fences == 4 + 2 * timestamps);
    return 1000 + ++timestamps;
}''')
s = s.replace('.finish = finish_unused,', '.finish = finish_unused, .timestamp_ns = sample_timestamp,')
old = '''    puts("PASS all seven operation kinds survive unaligned mixed-list serialization");
    nvn_replay_destroy(replay);'''
new = '''    puts("PASS all seven operation kinds survive unaligned mixed-list serialization");

    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    x[0] = COMMAND_BUFFER;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    active = *find_recording(replay, COMMAND_BUFFER);
    const uint64_t timestamp_address = QUEUE_MEMORY + 0xc00;
    const Operation sync = {.kind = OP_SYNC, .data.sync = {SYNC, &probe.sync[0], false}};
    const Operation timestamp = {.kind = OP_TIMESTAMP, .data.timestamp_address = timestamp_address};
    assert(append_operation(active, sync) && append_operation(active, timestamp) && append_operation(active, sync));
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    memcpy(guest(&probe, OUTPUT, sizeof x[0]), x, sizeof x[0]);
    x[0] = QUEUE; x[1] = 1; x[2] = OUTPUT;
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 5 && timestamps == 1);
    assert(nvn_replay_dispatch(replay, "nvnQueueSubmitCommands", x, NULL) == NVN_FRONT_HANDLED);
    assert(fences == 7 && timestamps == 2);
    CounterReport report;
    memcpy(&report, guest(&probe, timestamp_address, sizeof report), sizeof report);
    assert(report.counter == 0 && report.timestamp == 1002);
    puts("PASS repeated mixed submissions executed fence, timestamp, fence in order");
    nvn_replay_destroy(replay);'''
assert s.count(old) == 1
p.write_text(s.replace(old,new))
