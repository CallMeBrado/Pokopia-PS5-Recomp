/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef POKOPIA_ORBIS_MEMORY_H
#define POKOPIA_ORBIS_MEMORY_H

#include <stdio.h>

/* Log a native snapshot of the direct and flexible memory pools. */
void pokopia_orbis_memory_census(FILE *log, const char *phase);

#ifdef POKOPIA_ORBIS_MEMORY_TRACE
void pokopia_orbis_memory_trace_start(void);
void pokopia_orbis_memory_trace_flush(FILE *log);
#endif

#endif
