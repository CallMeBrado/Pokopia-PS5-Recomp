"""Read the guest TexContext free list from an isolated Windows boot probe."""
import sys

from read_boot_process import open_readonly


def pool_snapshot(read):
    def value(address):
        return read(address, '<Q')[0]

    tex_context = value(value(0x7103484108))
    allocator = value(tex_context + 19784)
    if allocator != 0x710113011C:
        raise ValueError(f'Unexpected texture allocator {allocator:#x}')
    pool = tex_context + 0x4778
    free_lists = pool + 320
    grain, threshold = read(free_lists + 104, '<2Q')
    seen = set()
    chunks = []
    for node in read(free_lists + 48, '<2Q'):
        while node and node not in seen:
            seen.add(node)
            if len(seen) > 4096:
                raise ValueError('Too many texture free-list nodes')
            start, size, node = read(node, '<3Q')
            chunks.append((start, size))
    provider = value(pool + 480)
    callback = value(value(provider)) if provider else 0
    delegate = value(provider + 160) if callback == 0x710112E394 else 0
    method = value(value(delegate)) if delegate else 0
    return (pool, provider, callback, delegate, method, grain, threshold, chunks)


if __name__ == '__main__':
    pid = int(sys.argv[1])
    with open_readonly(pid) as read:
        pool, provider, callback, delegate, method, grain, threshold, chunks = pool_snapshot(read)
    total = sum(size for _, size in chunks)
    largest = max((size for _, size in chunks), default=0)
    print(f'pool={pool:#x} override={provider:#x}/{callback:#x} delegate={delegate:#x}/{method:#x} grain={grain:#x} threshold={threshold:#x} '
          f'free_spans={len(chunks)} free_total={total:#x} largest={largest:#x} '
          f'largest_spans={sorted(chunks, key=lambda chunk: chunk[1], reverse=True)[:5]}')



