/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nxrecomp/ctx.h>

#include <stdio.h>
#include <stdlib.h>

extern void f_7700002140(Ctx *);
extern void f_7700002220(Ctx *);
extern void f_7700002340(Ctx *);

enum { BUFFER_SIZE = 1024, MAX_LENGTH = 257 };

static int check_fill(Ctx *cpu, size_t start, size_t length)
{
    unsigned char actual[BUFFER_SIZE];
    unsigned char expected[BUFFER_SIZE];

    memset(actual, 0x29, sizeof actual);
    memset(expected, 0x29, sizeof expected);
    memset(expected + start, 0xb3, length);
    cpu->x[0] = (uintptr_t)(actual + start);
    cpu->x[1] = 0x135b3;
    cpu->x[2] = length;
    f_7700002140(cpu);
    return memcmp(actual, expected, sizeof actual) == 0;
}

static int check_copy(Ctx *cpu, size_t destination, size_t source, size_t length)
{
    unsigned char actual[BUFFER_SIZE];
    unsigned char expected[BUFFER_SIZE];

    for (size_t i = 0; i < sizeof actual; ++i)
        actual[i] = (unsigned char)(i * 67u + i / 23u);
    memcpy(expected, actual, sizeof actual);
    memmove(expected + destination, expected + source, length);
    cpu->x[0] = (uintptr_t)(actual + destination);
    cpu->x[1] = (uintptr_t)(actual + source);
    cpu->x[2] = length;
    f_7700002340(cpu);
    return cpu->x[0] == (uintptr_t)(actual + destination) &&
           memcmp(actual, expected, sizeof actual) == 0;
}

static int check_length(Ctx *cpu, size_t start, size_t length)
{
    unsigned char string[BUFFER_SIZE];
    memset(string, 0xff, sizeof string);
    string[start + length] = 0;

    cpu->x[0] = (uintptr_t)(string + start);
    f_7700002220(cpu);
    return cpu->x[0] == length;
}

int pokopia_rtld_boot_probe(FILE *log);

int pokopia_guest_probe(FILE *log)
{
    if (!log)
        log = stdout;
    Ctx cpu = {0};
    unsigned tests = 0;

    for (size_t alignment = 0; alignment < 8; ++alignment) {
        size_t start = 16 + alignment;

        for (size_t length = 0; length <= MAX_LENGTH; ++length) {
            if (!check_fill(&cpu, start, length) ||
                !check_length(&cpu, start, length) ||
                !check_copy(&cpu, start, start + 7, length) ||
                !check_copy(&cpu, start + 7, start, length) ||
                !check_copy(&cpu, start + 384, start, length)) {
                fprintf(log, "FAIL rtld guest routines, alignment=%zu length=%zu\n",
                        alignment, length);
                return 1;
            }
            tests += 5;
        }
    }

    fprintf(log, "PASS Pokopia update 655360 rtld, %u checks\n", tests);
    return pokopia_rtld_boot_probe(log);
}

#ifdef POKOPIA_PROBE_STANDALONE
int main(void)
{
    return pokopia_guest_probe(stdout);
}
#endif
