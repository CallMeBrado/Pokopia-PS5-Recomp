#include "probe.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ps4/klog.h>

_Static_assert(sizeof(off_t) == sizeof(DirectOffset), "PS4 direct-memory offset ABI");

int main(void)
{
    const char *path = "/data/pokopia/memory-partial-release-r2.log";
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        klog_printf("Pokopia direct-memory proof: could not create %s (errno %d)\n", path, errno);
        return 1;
    }
    FILE *log = fdopen(fd, "w");
    if (!log) {
        klog_printf("Pokopia direct-memory proof: fdopen failed (errno %d)\n", errno);
        close(fd);
        return 1;
    }
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(log, "Pokopia direct-memory partial release proof r2 pid=%d monotonic-seconds=%lld\n",
            (int)getpid(), (long long)now.tv_sec);
    if (fflush(log) || fsync(fd)) {
        klog_printf("Pokopia direct-memory proof: could not flush start marker (errno %d)\n", errno);
        fclose(log);
        return 1;
    }
    const bool ok = run_direct_memory_proof(log);
    const bool synced = !fflush(log) && !fsync(fd);
    const bool closed = !fclose(log);
    klog_printf("Pokopia direct-memory proof %s; %s\n", ok && synced && closed ? "PASS" : "FAIL", path);
    return ok && synced && closed ? 0 : 1;
}
