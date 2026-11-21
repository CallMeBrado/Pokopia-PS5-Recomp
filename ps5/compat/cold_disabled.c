/* PS5: no cold-page spill. The PS4 build paged idle texture pages to a file to
 * fit 4.5 GiB; with ~11 GiB of direct memory the PS5 build keeps them resident.
 * loader_native.c calls these unconditionally and treats a failed open as
 * "disabled", after which none of the others are reached. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool hzn_heap_sparse_cold_open(const char *path)
{
   (void)path;
   return false;
}

int32_t hzn_heap_sparse_cold_error(void)
{
   return 0;
}

size_t hzn_heap_sparse_cold_park(void *address, size_t bytes)
{
   (void)address;
   (void)bytes;
   return 0;
}

size_t hzn_heap_sparse_cold_flush(void)
{
   return 0;
}
