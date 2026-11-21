from pathlib import Path

h = Path('cold_heap_windows.h')
s = h.read_text()
s = s.replace('bool hzn_heap_sparse_cold_open(const char *path);',
              'bool hzn_heap_sparse_cold_open(const char *path);\nbool hzn_heap_sparse_cold_contains(const void *address, size_t bytes);')
h.write_text(s)

p = Path('cold_heap_windows.c')
s = p.read_text()
anchor = 'size_t hzn_heap_sparse_cold_park(void *address, size_t bytes)'
assert s.count(anchor) == 1
s = s.replace(anchor, '''bool hzn_heap_sparse_cold_contains(const void *address, size_t bytes)
{
    if (!cold.handler || !bytes)
        return false;
    const uintptr_t start = (uintptr_t)address, base = (uintptr_t)cold.heap->base;
    return start >= base && start - base < cold.heap->size && bytes <= cold.heap->size - (start - base);
}

''' + anchor)
p.write_text(s)

p = Path('heap_stack_alias.c')
s = p.read_text().replace('#include "heap_stack_alias.h"', '#include "../../horizon/heap_stack_alias.h"')
s = s.replace('#include <windows.h>', '#include <windows.h>\n#include "cold_heap_windows.h"')
s = s.replace('''static int32_t protect_source(HznHeapStackAliasSlot *slot, bool inaccessible)
{''','''#ifdef _WIN32
static size_t cold_slice_bytes(uintptr_t address, uintptr_t end)
{
    const size_t next_chunk = HOST_RESERVE_ALIGNMENT - (address % HOST_RESERVE_ALIGNMENT);
    return end - address < next_chunk ? end - address : next_chunk;
}

static int32_t protect_cold_source(HznHeapStackAliases *aliases, HznHeapStackAliasSlot *slot,
                                   DWORD protection)
{
    const uintptr_t start = (uintptr_t)slot->range.source;
    const uintptr_t end = start + slot->source_protection_size;
    const DWORD original = protection == PAGE_NOACCESS ? PAGE_READWRITE : PAGE_NOACCESS;
    const bool recovering = aliases->unsafe && protection == PAGE_READWRITE;
    uintptr_t cursor = start;
    for (; cursor < end;) {
        const size_t bytes = cold_slice_bytes(cursor, end);
        DWORD previous;
        if (!VirtualProtect((void *)cursor, bytes, protection, &previous))
            break;
        cursor += bytes;
    }
    if (cursor == end)
        return 0;

    const int32_t error = (int32_t)GetLastError();
    if (recovering)
        return error;
    for (uintptr_t restore = start; restore < cursor;) {
        const size_t bytes = cold_slice_bytes(restore, cursor);
        DWORD previous;
        if (!VirtualProtect((void *)restore, bytes, original, &previous)) {
            aliases->unsafe = true;
            slot->source_protected = true;
        }
        restore += bytes;
    }
    return error;
}
#endif

static int32_t protect_source(HznHeapStackAliases *aliases, HznHeapStackAliasSlot *slot, bool inaccessible)
{''')
s = s.replace('''    const int32_t result = VirtualProtect(source, size, protection, &previous)
        ? 0 : (int32_t)GetLastError();
#else
    const int protection''', '''    const int32_t result = hzn_heap_sparse_cold_contains(source, size) ?
        protect_cold_source(aliases, slot, protection) :
        (VirtualProtect(source, size, protection, &previous) ? 0 : (int32_t)GetLastError());
#else
    (void)aliases;
    const int protection''')
s = s.replace('protect_source(&slot, true)', 'protect_source(aliases, &slot, true)')
s = s.replace('protect_source(slot, false)', 'protect_source(aliases, slot, false)')
s = s.replace('protect_source(slot, true)', 'protect_source(aliases, slot, true)')
s = s.replace('''        if (cleanup) {
            aliases->slots[aliases->count++] = slot;''',
              '''        if (cleanup || slot.source_protected) {
            aliases->slots[aliases->count++] = slot;''')
s = s.replace('''    const int32_t result = release_destination(slot);
    if (result == 0)''',
              '''    const int32_t result = hzn_stack_backing_owned(&slot->backing) ? release_destination(slot) : 0;
    if (result == 0)''')
p.write_text(s)
