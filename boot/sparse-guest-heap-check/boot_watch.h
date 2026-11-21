#ifndef POKOPIA_BOOT_WATCH_H
#define POKOPIA_BOOT_WATCH_H

#include <stdint.h>
#include <stdio.h>

void pokopia_boot_sample_heap(FILE *log, const char *phase, uint64_t checkpoint);

#endif
