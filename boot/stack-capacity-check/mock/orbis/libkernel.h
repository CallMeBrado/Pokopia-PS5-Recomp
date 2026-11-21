#include <stddef.h>
#include <stdint.h>
#define ORBIS_KERNEL_ERROR_EFAULT 0x8002000eU
int32_t sceKernelMprotect(void *address, size_t bytes, int protection);
int32_t sceKernelMunmap(void *address, size_t bytes);
