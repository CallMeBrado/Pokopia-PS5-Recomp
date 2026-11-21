/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_PC_BOOT_TRACE_H
#define POKOPIA_PC_BOOT_TRACE_H

#include "vk_program.h"

enum { PC_TRACE_RECORD, PC_TRACE_SUBMIT_VALIDATE, PC_TRACE_SUBMIT_EXECUTE };

typedef const void *(*PcReadGuest)(void *, uint64_t, size_t);

void pokopia_pc_trace_program(const PokopiaVkProgram *program, NvnFrontProgramLinkResult result, FILE *log);
void pokopia_pc_trace_draw_phase(const char *hash, unsigned phase, uint64_t target, NvnFrontReplyKind result);
void pokopia_pc_trace_ipc(uint32_t service, uint32_t handle, uint64_t tls_address, const uint8_t *tls);
void pokopia_pc_trace_wait_start(uint64_t tls, uint32_t svc, uint64_t caller, uint64_t fp, uint64_t arg0, uint64_t arg1);
void pokopia_pc_trace_wait_end(uint64_t tls);
void pokopia_pc_trace_present(FILE *log, PcReadGuest read_guest, uint64_t tls, uint64_t lr, uint64_t fp);
void pokopia_pc_trace_packet(FILE *log, uint64_t site, const uint64_t x[32]);
void pokopia_pc_trace_texture_tail(FILE *log, uint64_t storage);
void pokopia_pc_trace_nvn_tail(FILE *log, const NvnFrontReply *reply, uint64_t lr, uint64_t tls);

#endif
