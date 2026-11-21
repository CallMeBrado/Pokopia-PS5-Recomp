/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_BOOT_H
#define POKOPIA_BOOT_H

#include <nxrecomp/ctx.h>
#include <stdbool.h>
#include <stdio.h>
#include "../loader-probe/generated/process.inc"

#define POKOPIA_BOOT_HEAP_BASE UINT64_C(0x7800000000)
#define POKOPIA_BOOT_STACK_REGION UINT64_C(0x7c00000000)
#define POKOPIA_BOOT_STACK_REGION_SIZE UINT64_C(0x100000000)
#define POKOPIA_BOOT_STACK_BASE UINT64_C(0x7c00010000)
#define POKOPIA_BOOT_STACK_SIZE POKOPIA_MAIN_STACK_SIZE
#define POKOPIA_BOOT_TLS_BASE UINT64_C(0x7e00000000)
#define POKOPIA_BOOT_TLS_SIZE 0x4000u
#define POKOPIA_BOOT_TCB_BASE (POKOPIA_BOOT_TLS_BASE + POKOPIA_BOOT_TLS_SIZE)
#define POKOPIA_BOOT_THREAD_HANDLE 0x10000u
#define POKOPIA_BOOT_WORKER_HANDLE 0x20000u
#define POKOPIA_BOOT_WORKER_STACK UINT64_C(0x7e00010000)
#define POKOPIA_BOOT_WORKER_TLS UINT64_C(0x7e00020000)
#define POKOPIA_BOOT_WORKER_TLS_STRIDE 0x10000u

bool pokopia_boot_init(FILE *log, uint64_t mapped_bytes);
bool pokopia_boot_check_heap(FILE *log);
uint64_t pokopia_boot_heap_size(void);
bool pokopia_boot_release_heap(FILE *log);
bool pokopia_boot_svc(Ctx *cpu, uint32_t number, FILE *log);
bool pokopia_boot_sample_clocks(uint64_t *tick_ns, int64_t *utc_ns);
uint64_t pokopia_boot_monotonic_ns(void);
typedef void (*PokopiaBootEntry)(Ctx *);
PokopiaBootEntry pokopia_boot_lookup(uint64_t address);
bool pokopia_boot_dispatch(Ctx *cpu, uint64_t address);
uint64_t pokopia_boot_import(uint32_t id, const char **name);

#endif
