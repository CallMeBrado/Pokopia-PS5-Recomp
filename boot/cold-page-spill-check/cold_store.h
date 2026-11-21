/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HZN_COLD_STORE_H
#define HZN_COLD_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { HZN_COLD_PAGE_BYTES = 64 * 1024 };

typedef struct {
    intptr_t file;
    size_t slots;
} HznColdStore;

/* Open a new ephemeral file, with no existing data. The caller serializes
 * access and tracks which slots were completely saved. A failed save can
 * leave partial file bytes, so resident bytes must remain canonical. */
bool hzn_cold_store_open(HznColdStore *store, const char *path, size_t slots);
bool hzn_cold_store_save(const HznColdStore *store, size_t slot, const void *page);
bool hzn_cold_store_load(const HznColdStore *store, size_t slot, void *page);
bool hzn_cold_store_close(HznColdStore *store);

#endif
