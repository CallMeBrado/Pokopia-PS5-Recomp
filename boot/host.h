/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_BOOT_HOST_H
#define POKOPIA_BOOT_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool pokopia_host_init(void);
void pokopia_host_destroy(void);
bool pokopia_host_set_thread(void *value);
void *pokopia_host_thread(void);
void pokopia_host_lock(void);
void pokopia_host_unlock(void);
bool pokopia_host_trylock(void);
/* Both functions use the held kernel lock. Waiting releases it until woken. */
bool pokopia_host_wait(const bool *ready, const bool *failed, uint32_t timeout_ms);
void pokopia_host_notify(void);
size_t pokopia_host_page_size(void);
bool pokopia_host_protect(uint64_t address, uint64_t size, uint32_t permission);

#endif
