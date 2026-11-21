/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_SAVE_STORE_H
#define POKOPIA_HORIZON_SAVE_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define HZN_SAVE_PATH_MAX 512
#define HZN_SAVE_TITLE_ID UINT64_C(0x0400a3a01ee4c000)
/* Fixed account save quotas supported by this title's host backend. */
#define HZN_SAVE_ACCOUNT_DATA_LIMIT INT64_C(0x4000000)
#define HZN_SAVE_ACCOUNT_JOURNAL_LIMIT INT64_C(0x4000000)

/* The title owns root, and its immediate parent is already provisioned. */
uint32_t hzn_save_ensure_account(const char *root, uint64_t uid_hi, uint64_t uid_lo);
uint32_t hzn_save_open_account(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               char path[HZN_SAVE_PATH_MAX]);
uint32_t hzn_save_account_size(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               int64_t *normal, int64_t *journal);
uint32_t hzn_save_create_directory(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                                   const char *guest_path);
uint32_t hzn_save_create_file(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                              const char *guest_path, uint64_t size);
uint32_t hzn_save_get_entry_type(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                                 const char *guest_path, uint32_t *type);
uint32_t hzn_save_free_space(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                             const char *guest_path, uint64_t *bytes);
uint32_t hzn_save_open_read(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                            const char *guest_path, FILE **stream);

uint32_t hzn_save_open_write(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                             const char *guest_path, FILE **stream);
uint32_t hzn_save_write_file(const char *root, uint64_t uid_hi, uint64_t uid_lo, FILE *stream,
                             uint64_t offset, const void *data, size_t bytes, bool allow_append, bool flush);
uint32_t hzn_save_flush(FILE *stream);
uint32_t hzn_save_commit(const char *root, uint64_t uid_hi, uint64_t uid_lo);

#endif
