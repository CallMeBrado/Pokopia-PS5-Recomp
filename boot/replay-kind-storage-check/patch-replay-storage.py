from pathlib import Path

p = Path('nvn_replay.c')
s = p.read_text()
def edit(old, new):
    global s
    assert s.count(old) == 1, (old, s.count(old))
    s = s.replace(old, new)

edit('''typedef struct {
    enum { OP_COPY, OP_CLEAR, OP_DEPTH_CLEAR, OP_DRAW, OP_DISPATCH, OP_SYNC, OP_TIMESTAMP } kind;''',
     '''typedef enum {
    OP_COPY, OP_CLEAR, OP_DEPTH_CLEAR, OP_DRAW, OP_DISPATCH, OP_SYNC, OP_TIMESTAMP,
} OperationKind;

typedef struct {
    OperationKind kind;''')
edit('''    Operation operations[];
} RecordedList;''',
     '''    /* Each operation is a one-byte kind followed by its payload. */
    uint8_t operations[];
} RecordedList;

static size_t operation_payload_size(OperationKind kind)
{
    static const size_t sizes[] = {
        [OP_COPY] = sizeof(Copy),
        [OP_CLEAR] = sizeof(Clear),
        [OP_DEPTH_CLEAR] = sizeof(DepthClear),
        [OP_DRAW] = sizeof(Draw),
        [OP_DISPATCH] = sizeof(ComputeDispatch),
        [OP_SYNC] = sizeof(Sync),
        [OP_TIMESTAMP] = sizeof(uint64_t),
    };
    return (unsigned)kind < sizeof sizes / sizeof sizes[0] ? sizes[kind] : 0;
}

_Static_assert(OP_TIMESTAMP <= UINT8_MAX, "recorded operation kind must fit in one byte");''')
edit('''    /* Keep only the immutable operations that a submitted handle needs. */
    const size_t operation_bytes = recording->count * sizeof(Operation);
    RecordedList *list = malloc(sizeof *list + operation_bytes);''',
     '''    size_t bytes = sizeof(RecordedList);
    for (size_t i = 0; i < recording->count; ++i) {
        const size_t payload = operation_payload_size(recording->operations[i].kind);
        if (!payload || payload >= SIZE_MAX - bytes)
            return NVN_FRONT_BOUNDARY;
        bytes += 1 + payload;
    }
    RecordedList *list = malloc(bytes);''')
edit('''    if (operation_bytes)
        memcpy(list->operations, recording->operations, operation_bytes);''',
     '''    uint8_t *cursor = list->operations;
    for (size_t i = 0; i < recording->count; ++i) {
        const Operation *operation = &recording->operations[i];
        const size_t payload = operation_payload_size(operation->kind);
        *cursor++ = (uint8_t)operation->kind;
        memcpy(cursor, &operation->data, payload);
        cursor += payload;
    }''')
edit('''static const RecordedList *command_at''',
     '''static NvnFrontReplyKind run_list(NvnReplay *replay, uint64_t device, const RecordedList *list, bool execute)
{
    const uint8_t *cursor = list->operations;
    for (size_t i = 0; i < list->count; ++i) {
        Operation operation;
        operation.kind = (OperationKind)*cursor++;
        const size_t payload = operation_payload_size(operation.kind);
        if (!payload)
            return NVN_FRONT_BOUNDARY;
        memcpy(&operation.data, cursor, payload);
        cursor += payload;
        const NvnFrontReplyKind result = run_operation(replay, device, &operation, execute);
        if (result != NVN_FRONT_HANDLED)
            return result;
    }
    return NVN_FRONT_HANDLED;
}

static const RecordedList *command_at''')
edit('''        for (size_t j = 0; j < list->count; ++j) {
            result = run_operation(replay, device, &list->operations[j], false);
            if (result != NVN_FRONT_HANDLED)
                goto done;
        }''',
     '''        result = run_list(replay, device, list, false);
        if (result != NVN_FRONT_HANDLED)
            goto done;''')
edit('''        const RecordedList *list = batch[i];
        for (size_t j = 0; j < list->count; ++j) {
            result = run_operation(replay, device, &list->operations[j], true);
            if (result != NVN_FRONT_HANDLED)
                goto done;
        }''',
     '''        result = run_list(replay, device, batch[i], true);
        if (result != NVN_FRONT_HANDLED)
            goto done;''')
p.write_text(s)
