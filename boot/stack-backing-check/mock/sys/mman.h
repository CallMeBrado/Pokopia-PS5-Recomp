
#ifndef TEST_ORBIS_MMAN_H
#define TEST_ORBIS_MMAN_H
#include <stddef.h>
#include <stdint.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_FAILED ((void *)(intptr_t)-1)
void *mmap(void *address, size_t size, int prot, int flags, int fd, int64_t offset);
#endif
