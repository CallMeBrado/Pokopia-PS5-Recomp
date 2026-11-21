from pathlib import Path

p = Path('test-production.c')
s = p.read_text()
old = '''    printf("recording=%zu bytes, finalized header=%zu bytes, operations=%zu bytes each\\n",
           sizeof(Recording), sizeof(RecordedList), sizeof(Operation));'''
new = '''    invoke(front, 0x75005c4bb0ULL, COMMAND_BUFFER, CONTROL_MEMORY, 256);
    x[0] = COMMAND_BUFFER;
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferBeginRecording", x, NULL) == NVN_FRONT_HANDLED);
    active = *find_recording(replay, COMMAND_BUFFER);
    Operation operations[OP_TIMESTAMP + 1] = {0};
    size_t exact = sizeof(RecordedList);
    for (unsigned kind = OP_COPY; kind <= OP_TIMESTAMP; ++kind) {
        Operation *operation = &operations[kind];
        operation->kind = (OperationKind)kind;
        const size_t bytes = operation_payload_size(operation->kind);
        memset(&operation->data, (int)(1 + kind), bytes);
        assert(append_operation(active, *operation));
        exact += 1 + bytes;
    }
    assert(nvn_replay_dispatch(replay, "nvnCommandBufferEndRecording", x, NULL) == NVN_FRONT_HANDLED);
    const RecordedList *mixed = *find_list(replay, x[0]);
    assert(mixed && mixed->count == OP_TIMESTAMP + 1 && last_list_size == exact);
    const uint8_t *cursor = mixed->operations;
    printf("recording=%zu finalized_header=%zu working_operation=%zu packed_operations=",
           sizeof(Recording), sizeof(RecordedList), sizeof(Operation));
    for (unsigned kind = OP_COPY; kind <= OP_TIMESTAMP; ++kind) {
        const size_t bytes = operation_payload_size((OperationKind)kind);
        assert(*cursor++ == kind && !memcmp(cursor, &operations[kind].data, bytes));
        cursor += bytes;
        printf("%s%zu", kind ? "," : "", bytes + 1);
    }
    assert(cursor == (const uint8_t *)mixed + exact);
    puts("");
    puts("PASS all seven operation kinds survive unaligned mixed-list serialization");'''
assert s.count(old) == 1
p.write_text(s.replace(old, new))
