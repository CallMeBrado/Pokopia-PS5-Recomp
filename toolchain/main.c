#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

#ifdef POKOPIA_GUEST_PROBE
int pokopia_guest_probe(FILE *log);
#endif

#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

int main(void)
{
#if defined(__PROSPERO__) && defined(POKOPIA_GAME_BOOT)
    const char *log_path = "/data/pokopia/ps5-game-boot-r" STRINGIFY(POKOPIA_GAME_BOOT) ".txt";
#elif defined(POKOPIA_GAME_BOOT) && POKOPIA_GAME_BOOT > 1
    const char *log_path = "/data/pokopia-ps4-game-boot-r" STRINGIFY(POKOPIA_GAME_BOOT) ".txt";
#elif defined(POKOPIA_GAME_BOOT)
    const char *log_path = "/data/pokopia-ps4-game-boot.txt";
#elif defined(POKOPIA_GFX)
    const char *log_path = "/data/pokopia-ps4-graphics-boot.txt";
#elif defined(POKOPIA_BOOT_PROBE)
    const char *log_path = "/data/pokopia-ps4-worker-probe.txt";
#elif defined(POKOPIA_SDK_PROBE)
    const char *log_path = "/data/pokopia-ps4-sdk-probe.txt";
#elif defined(POKOPIA_LOADER_PROBE)
    const char *log_path = "/data/pokopia-ps4-loader-probe.txt";
#else
    const char *log_path = "/data/pokopia-ps4-probe.txt";
#endif
    FILE *log = fopen(log_path, "w");
    if (log == NULL)
        fprintf(stderr, "Could not open %s (%s)\n", log_path, strerror(errno));

#if defined(POKOPIA_ORBIS_MEMORY_TRACE) && defined(POKOPIA_GAME_BOOT)
    const char *mesa_log_path = "/data/pokopia-ps4-mesa-r" STRINGIFY(POKOPIA_GAME_BOOT) ".txt";
    FILE *diagnostic = log ? log : stderr;
    if (setenv("MESA_LOG_FILE", mesa_log_path, 1) || setenv("MESA_LOG_LEVEL", "info", 0))
        fprintf(diagnostic, "Could not configure the Mesa driver log (%s)\n", strerror(errno));
    else
        fprintf(diagnostic, "Mesa driver log %s\n", mesa_log_path);
#endif

#if defined(__PROSPERO__)
    const char *started = "Pokopia PS5: native PPSA title reached main";
#else
    const char *started = "Pokopia PS4: native OpenOrbis executable reached main";
#endif
    puts(started);
    fflush(stdout);
    if (log != NULL) {
        fprintf(log, "%s\n", started);
        fflush(log);
    }

    size_t direct = sceKernelGetDirectMemorySize();
    char memory[128];
    snprintf(memory, sizeof(memory), "Pokopia PS4: direct memory size %zu bytes", direct);
    puts(memory);
    fflush(stdout);
    if (log != NULL) {
        fprintf(log, "%s\n", memory);
        fflush(log);
    }

    int result = 0;
#ifdef POKOPIA_GUEST_PROBE
    result = pokopia_guest_probe(log);
#endif

    printf("Pokopia PS4: probe exit status %d\n", result);
    fflush(stdout);
    if (log != NULL) {
        fprintf(log, "Pokopia PS4: probe exit status %d\n", result);
        fclose(log);
    }

    // The Orbis launcher reports a return through musl _exit as a crash.
    // Leave the app open so the result can be collected before closing it.
    for (;;)
        sceKernelUsleep(1000000);
}

