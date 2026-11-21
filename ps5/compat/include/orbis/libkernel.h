/* PS5 stand-in for OpenOrbis <orbis/libkernel.h>.
 *
 * Declares only what the Pokopia runtime uses. The PS5 libkernel exports the same
 * functions under the same names (and NIDs), so these resolve against the payload
 * SDK's libkernel stubs. Values match OpenOrbis _types/kernel.h and _types/errors.h.
 */
#ifndef POKOPIA_PS5_ORBIS_LIBKERNEL_H
#define POKOPIA_PS5_ORBIS_LIBKERNEL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The runtime asks for WB_ONION meaning "ordinary cached CPU memory". The PS5
 * numbers its direct-memory types differently: type 0 made guest code (whose
 * heap and stacks use it) run no faster than on a PS4 Jaguar, while host code
 * on malloc memory ran 3.3x faster. Type 12 is what the PS5 RADV winsys
 * allocates and measured cached and coherent (PS5_Vulkan HARDWARE_FINDINGS,
 * 2026-09-27). */
#define PS5_KERNEL_MTYPE_PS4_WB_ONION 0x0
#define ORBIS_KERNEL_WB_ONION 12
#define ORBIS_KERNEL_WC_GARLIC 0x3
#define ORBIS_KERNEL_WB_GARLIC 0xA

#define ORBIS_KERNEL_PROT_CPU_NONE 0x00
#define ORBIS_KERNEL_PROT_CPU_READ 0x01
#define ORBIS_KERNEL_PROT_CPU_WRITE 0x02
#define ORBIS_KERNEL_PROT_CPU_RW 0x03
#define ORBIS_KERNEL_PROT_CPU_EXEC 0x04
#define ORBIS_KERNEL_PROT_CPU_ALL 0x07
#define ORBIS_KERNEL_PROT_GPU_READ 0x10
#define ORBIS_KERNEL_PROT_GPU_WRITE 0x20
#define ORBIS_KERNEL_PROT_GPU_RW 0x30
#define ORBIS_KERNEL_PROT_GPU_ALL 0x30

#define ORBIS_KERNEL_ERROR_ENOMEM 0x8002000C
#define ORBIS_KERNEL_ERROR_EFAULT 0x8002000E
#define ORBIS_KERNEL_ERROR_EBUSY 0x80020010
#define ORBIS_KERNEL_ERROR_EAGAIN 0x80020023

typedef struct {
   void *unk01;
   void *unk02;
   off_t offset;
   int32_t unk04;
   int32_t unk05;
   unsigned isFlexibleMemory : 1;
   unsigned isDirectMemory : 1;
   unsigned isStack : 1;
   unsigned isPooledMemory : 1;
   unsigned isCommitted : 1;
   char name[32];
} OrbisKernelVirtualQueryInfo;

typedef pthread_t OrbisPthread;
typedef pthread_attr_t OrbisPthreadAttr;

int32_t sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int32_t, off_t *);
int32_t sceKernelAvailableDirectMemorySize(off_t, off_t, size_t, off_t *, size_t *);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *);
size_t sceKernelGetDirectMemorySize(void);
uint64_t sceKernelGetProcessTimeCounter(void);
uint64_t sceKernelGetProcessTimeCounterFrequency(void);
int32_t sceKernelMapDirectMemory(void **, size_t, int32_t, int32_t, off_t, size_t);
int32_t sceKernelMapFlexibleMemory(void **, size_t, int32_t, int);
int32_t sceKernelMmap(void *, size_t, int32_t, int32_t, int32_t, off_t, void **);
int32_t sceKernelMprotect(const void *, size_t, int);
int32_t sceKernelMunmap(void *, size_t);
int32_t sceKernelQueryMemoryProtection(void *, void **, void **, int32_t *);
int32_t sceKernelReleaseDirectMemory(off_t, size_t);
int32_t sceKernelReserveVirtualRange(void **, size_t, int32_t, size_t);
int32_t sceKernelUsleep(uint32_t);
int32_t sceKernelVirtualQuery(const void *, int32_t, OrbisKernelVirtualQueryInfo *, size_t);
int32_t scePthreadCreate(OrbisPthread *, const OrbisPthreadAttr *, void *(*)(void *), void *, const char *);
int sceKernelDebugOutText(int channel, const char *text);

#ifdef __cplusplus
}
#endif
#endif
