/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_SAVE_STORE_H
#define POKOPIA_HORIZON_SAVE_STORE_H

#include <stdint.h>

#define HZN_SAVE_PATH_MAX 512
#define HZN_SAVE_TITLE_ID UINT64_C(0x0400a3a01ee4c000)

/* The title owns root, and its immediate parent is already provisioned. */
uint32_t hzn_save_ensure_account(const char *root, uint64_t uid_hi, uint64_t uid_lo);
uint32_t hzn_save_open_account(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               char path[HZN_SAVE_PATH_MAX]);

#endif
