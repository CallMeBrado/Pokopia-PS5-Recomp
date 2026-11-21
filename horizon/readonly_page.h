/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_HORIZON_READONLY_PAGE_H
#define POKOPIA_HORIZON_READONLY_PAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HZN_READONLY_PAGE_BYTES = 0x1000 };

typedef struct {
    uint64_t address;
    int32_t last_host_error;
    bool unsafe;

    /* Private ownership of the isolated host reservation. */
    void *reservation;
    size_t reservation_bytes;
} HznReadonlyPage;

/* Start with a zero-initialized object. The caller validates guest ownership
 * and excludes guest access until map or unmap completes. initial_data must
 * contain a complete page. The address is published only after protection. */
bool hzn_readonly_page_map(HznReadonlyPage *page, uint64_t address, const void *initial_data);
/* Same ownership rules for a larger guest-page-aligned range. A null input
 * leaves the anonymous backing zero-initialized. */
bool hzn_readonly_span_map(HznReadonlyPage *page, uint64_t address, size_t size, const void *initial_data);

/* Release the reservation, if any. If host cleanup fails, unsafe is set and
 * the caller must stop guest access. Retain ownership for a cleanup retry. */
bool hzn_readonly_page_unmap(HznReadonlyPage *page);

#ifdef __cplusplus
}
#endif
#endif
