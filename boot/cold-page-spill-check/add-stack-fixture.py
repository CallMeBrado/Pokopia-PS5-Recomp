from pathlib import Path

p=Path('test-cold-spill.c')
s=p.read_text()
s=s.replace('static HANDLE save_started, load_started, resume_io;',
'''static HANDLE save_started, load_started, resume_io;
static const void *fail_alias_slice, *fail_alias_restore;''')
s=s.replace('''    if (fail_rollback && protect == PAGE_READWRITE) {''',
'''    if ((fail_rollback && protect == PAGE_READWRITE) ||
        (address == fail_alias_slice && protect == PAGE_NOACCESS) ||
        (address == fail_alias_restore && protect == PAGE_READWRITE)) {''')
s=s.replace('#include "cold_heap_windows.c"', '#include "cold_heap_windows.c"\n#include "heap_stack_alias.c"')
needle='''int main(void)
{'''
fixture='''static void check_cross_chunk_alias(HznHeap *heap)
{
    uint8_t *source = (uint8_t *)heap->mapping + 0x1c000;
    uint8_t *next_chunk = source + 0x4000;
    const size_t bytes = 0xc000;
    uint8_t *destination = (uint8_t *)(uintptr_t)UINT64_C(0x7c5ad1f000);
    assert(hzn_heap_sparse_prepare_io(source, bytes));
    assert(mapping(source).AllocationBase != mapping(next_chunk).AllocationBase);
    source[0] = 0x61;
    source[bytes - 1] = 0x62;
    HznHeapStackAliases aliases = {0};
    fail_alias_slice = next_chunk;
    assert(hzn_heap_stack_alias_map(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(!aliases.count && !aliases.unsafe && mapping(destination).State == MEM_FREE);
    assert(mapping(source).Protect == PAGE_READWRITE && mapping(next_chunk).Protect == PAGE_READWRITE);
    assert(source[0] == 0x61 && source[bytes - 1] == 0x62);

    fail_alias_restore = source;
    assert(hzn_heap_stack_alias_map(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) ==
           HZN_STACK_MAP_INVALID_MEMORY_STATE);
    assert(aliases.count == 1 && aliases.unsafe && mapping(destination).State == MEM_FREE);
    assert(mapping(source).Protect == PAGE_NOACCESS && mapping(next_chunk).Protect == PAGE_READWRITE);
    fail_alias_slice = fail_alias_restore = NULL;
    assert(hzn_heap_stack_alias_release(&aliases));
    assert(mapping(source).Protect == PAGE_READWRITE && mapping(next_chunk).Protect == PAGE_READWRITE);

    assert(hzn_heap_stack_alias_map(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) == HZN_STACK_MAP_OK);
    assert(mapping(source).Protect == PAGE_NOACCESS && mapping(next_chunk).Protect == PAGE_NOACCESS);
    assert(mapping(source - 1).Protect == PAGE_READWRITE && mapping(source + bytes).Protect == PAGE_READWRITE);
    assert(destination[0] == 0x61 && destination[bytes - 1] == 0x62);
    destination[0] = 0x83;
    destination[bytes - 1] = 0x84;
    assert(hzn_heap_stack_alias_unmap(&aliases, (uintptr_t)destination, (uintptr_t)source, bytes) == HZN_STACK_MAP_OK);
    assert(!aliases.count && !aliases.unsafe && mapping(destination).State == MEM_FREE);
    assert(source[0] == 0x83 && source[bytes - 1] == 0x84);
    puts("PASS exact 0xc000 cross-chunk stack MapMemory protection, copyback and rollback");
}

'''
assert s.count(needle)==1
s=s.replace(needle,fixture+needle)
s=s.replace('''    hzn_heap_sparse_report(stdout, "fixture");''','''    check_cross_chunk_alias(&heap);
    hzn_heap_sparse_report(stdout, "fixture");''')
p.write_text(s)
