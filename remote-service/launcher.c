/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "sandbox.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <orbis/NetCtl.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>

#define ASSET_DIR "/app0/assets/"
#define SERVICE_DIR "/data/ps4-remote-service"
#define CAPTURE_DIR "/data/ps4-ambilight"

enum {
    PAYLOADER_PORT = 9090,
    CONNECT_TIMEOUT_MS = 700,
    UPLOAD_TIMEOUT_MS = 10000,
    ORBIS_CLOCK_MONOTONIC = 4,
    ORBIS_SO_NOSIGPIPE = 0x0800,
    ORBIS_SYS_LINK = 9,
    MAX_BINARY_BYTES = 2 * 1024 * 1024,
};

typedef struct {
    const char *name;
    size_t limit;
    uint8_t *bytes;
    size_t size;
} Asset;

static bool read_exact(int fd, void *data, size_t count)
{
    uint8_t *next = data;
    while (count) {
        const ssize_t got = read(fd, next, count);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (got == 0) errno = EIO;
            return false;
        }
        next += got;
        count -= (size_t)got;
    }
    return true;
}

static bool read_asset(Asset *asset)
{
    char path[128];
    snprintf(path, sizeof path, ASSET_DIR "%s", asset->name);
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    const off_t end = lseek(fd, 0, SEEK_END);
    int error = 0;
    if (end < 0) error = errno;
    else if (end == 0) error = EINVAL;
    else if ((uint64_t)end > asset->limit) error = EFBIG;
    else if (lseek(fd, 0, SEEK_SET) < 0) error = errno;
    if (!error) {
        asset->size = (size_t)end;
        asset->bytes = malloc(asset->size);
        if (!asset->bytes || !read_exact(fd, asset->bytes, asset->size)) error = errno;
    }
    close(fd);
    if (error) errno = error;
    return !error;
}

static bool config_line(const Asset *asset, char *line, size_t capacity)
{
    size_t count = asset->size;
    if (count && asset->bytes[count - 1] == '\n') --count;
    if (count && asset->bytes[count - 1] == '\r') --count;
    if (!count || count >= capacity || memchr(asset->bytes, 0, count)) return false;
    memcpy(line, asset->bytes, count);
    line[count] = 0;
    return true;
}

static bool validate_config(const Asset *client, const Asset *token)
{
    char ip[INET_ADDRSTRLEN], key[65];
    struct in_addr address;
    return config_line(client, ip, sizeof ip) && inet_pton(AF_INET, ip, &address) == 1 &&
        config_line(token, key, sizeof key) && strspn(key, "0123456789abcdef") == 64;
}

static bool ensure_directory(const char *path)
{
    if (mkdir(path, 0755) == 0) return true;
    if (errno != EEXIST) return false;
    /* OpenOrbis implements lstat through an fstatat stub that returns ENOSYS. */
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return false;
    close(fd);
    return true;
}

static int verify_plugin_fd(int fd, size_t size, struct stat *info)
{
    if (fstat(fd, info) < 0) return errno;
    if (!S_ISREG(info->st_mode)) return EEXIST;
    /* The OpenOrbis stat header places st_size past the native PS4 offset. */
    const off_t length = lseek(fd, 0, SEEK_END);
    if (length < 0) return errno;
    if ((uint64_t)length != size) return EEXIST;
    return lseek(fd, 0, SEEK_SET) < 0 ? errno : 0;
}

/* Return 1 for identical contents, 0 for an absent path, or -1 on conflict or error.
   A nonzero mode repairs permissions only after verifying the exact plugin. */
static int existing_plugin(const char *path, const Asset *asset, mode_t mode)
{
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat info;
    int error = verify_plugin_fd(fd, asset->size, &info);
    uint8_t block[4096];
    for (size_t pos = 0; !error && pos < asset->size;) {
        size_t count = asset->size - pos;
        if (count > sizeof block) count = sizeof block;
        if (!read_exact(fd, block, count)) error = errno;
        else if (memcmp(block, asset->bytes + pos, count)) error = EEXIST;
        pos += count;
    }
    if (!error) {
        const int current_fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (current_fd < 0) error = errno;
        else {
            struct stat current;
            error = verify_plugin_fd(current_fd, asset->size, &current);
            if (!error && (current.st_dev != info.st_dev || current.st_ino != info.st_ino)) error = EEXIST;
            if (!error && mode && (current.st_mode & 0777) != mode && fchmod(current_fd, mode) < 0)
                error = errno;
            close(current_fd);
        }
    }
    close(fd);
    if (error) {
        errno = error;
        return -1;
    }
    return 1;
}

static bool write_exact(int fd, const void *data, size_t count)
{
    const uint8_t *next = data;
    while (count) {
        const ssize_t wrote = write(fd, next, count);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            if (wrote == 0) errno = EIO;
            return false;
        }
        next += wrote;
        count -= (size_t)wrote;
    }
    return true;
}

static int publish_new_plugin(const char *source, const char *target)
{
    /* The OpenOrbis link stub returns ENOSYS. Use the native hard link call
       so an existing plugin stays intact even if another process creates it. */
    long result = ORBIS_SYS_LINK;
    int failed;
    __asm__ volatile("syscall" : "+a"(result), "=@ccc"(failed) : "D"(source), "S"(target)
                     : "rcx", "r11", "memory");
    if (!failed) return 0;
    errno = (int)result;
    return -1;
}

static bool matches_previous_plugin(const char *path, const Asset *previous, size_t previous_count)
{
    for (size_t i = 0; i < previous_count; ++i) {
        const int existing = existing_plugin(path, &previous[i], 0);
        if (existing == 1) return true;
        if (existing == 0 || errno != EEXIST) return false;
    }
    return false;
}

static bool stage_file(const char *path, const Asset *asset, const Asset *previous,
                       size_t previous_count, bool *upgraded)
{
    /* The compositor's module loader checks the PRX's execute permission. */
    const mode_t mode = previous ? 0755 : 0644;
    bool replacing = false;
    if (previous) {
        const int existing = existing_plugin(path, asset, mode);
        if (existing == 1) return true;
        if (existing < 0) {
            if (errno != EEXIST) return false;
            replacing = true;
        }
    }

    char temporary[160];
    snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
    const int fd = mkstemp(temporary);
    if (fd < 0) return false;

    int error = 0;
    if (!write_exact(fd, asset->bytes, asset->size) || fchmod(fd, mode) < 0 || fsync(fd) < 0)
        error = errno;
    if (close(fd) < 0 && !error) error = errno;
    if (!error) {
        if (replacing) {
            /* Recheck the exact prior release immediately before atomic replacement. */
            if (matches_previous_plugin(path, previous, previous_count)) {
                if (rename(temporary, path) < 0) error = errno;
                else *upgraded = true;
            } else if (!(errno == EEXIST && existing_plugin(path, asset, mode) == 1)) {
                error = errno;
            }
        } else if (previous) {
            /* Publish a fresh PRX without replacing a competing file. */
            if (publish_new_plugin(temporary, path) < 0 &&
                !(errno == EEXIST && existing_plugin(path, asset, mode) == 1))
                error = errno;
        } else if (rename(temporary, path) < 0) error = errno;
    }
    unlink(temporary);
    if (error) errno = error;
    return !error;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(ORBIS_CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool wait_writable(int fd, uint64_t deadline)
{
    for (;;) {
        const uint64_t now = monotonic_ms();
        if (!now || now >= deadline) {
            errno = ETIMEDOUT;
            return false;
        }
        struct pollfd socket = { .fd = fd, .events = POLLOUT };
        const int result = poll(&socket, 1, (int)(deadline - now));
        if (result > 0) return true;
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) errno = ETIMEDOUT;
        return false;
    }
}

static int connect_payloader(struct in_addr address)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    const struct sockaddr_in local = {
        .sin_len = sizeof local,
        .sin_family = AF_INET,
        .sin_port = htons(PAYLOADER_PORT),
        .sin_addr = address,
    };
    const int yes = 1;
    int saved_error;
    if (setsockopt(fd, SOL_SOCKET, ORBIS_SO_NOSIGPIPE, &yes, sizeof yes) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) goto done;

    if (connect(fd, (const struct sockaddr *)&local, sizeof local) < 0) {
        if (errno != EINPROGRESS || !wait_writable(fd, monotonic_ms() + CONNECT_TIMEOUT_MS)) goto done;
        int error = 0;
        socklen_t size = sizeof error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) goto done;
        if (error) {
            errno = error;
            goto done;
        }
    }
    return fd;

done:
    saved_error = errno;
    close(fd);
    errno = saved_error;
    return -1;
}

static struct in_addr console_address(void)
{
    struct in_addr address = {0};
    if (sceNetCtlInit() == 0) {
        OrbisNetCtlInfo info = {0};
        if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0) {
            info.ip_address[sizeof info.ip_address - 1] = '\0';
            inet_pton(AF_INET, info.ip_address, &address);
        }
        sceNetCtlTerm();
    }
    return address;
}

static bool send_payload(const Asset *asset, struct in_addr address)
{
    const struct in_addr loopback = { .s_addr = htonl(INADDR_LOOPBACK) };
    int fd = connect_payloader(loopback);
    if (fd < 0 && address.s_addr && address.s_addr != loopback.s_addr)
        fd = connect_payloader(address);
    if (fd < 0) return false;

    bool ok = false;
    int saved_error;
    const uint64_t deadline = monotonic_ms() + UPLOAD_TIMEOUT_MS;
    for (size_t sent = 0; sent < asset->size;) {
        if (!wait_writable(fd, deadline)) goto done;
        const ssize_t count = send(fd, asset->bytes + sent, asset->size - sent, 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (count <= 0) {
            if (count == 0) errno = EPIPE;
            goto done;
        }
        sent += (size_t)count;
    }
    ok = shutdown(fd, SHUT_WR) == 0;
done:
    saved_error = errno;
    close(fd);
    errno = saved_error;
    return ok;
}

static void record(FILE *log, const char *what, int error)
{
    if (error) fprintf(log, "%s (errno %d)\n", what, error);
    else fprintf(log, "%s\n", what);
    fflush(log);
}

static void notify(const char *message)
{
    OrbisNotificationRequest request = {
        .type = NotificationRequest, .targetId = -1, .useIconImageUri = 1,
        .iconUri = "cxml://psnotification/tex_icon_system",
    };
    snprintf(request.message, sizeof request.message, "PS4 LAN Service\n%s", message);
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
}

static bool valid_prx(const Asset *asset)
{
    return asset->size >= 64 && !memcmp(asset->bytes, "\x4f\x15\x3d\x1d", 4);
}

int main(void)
{
    enum { CAPTURE_PRX, CAPTURE_R5, CAPTURE_R4, CAPTURE_R3, CAPTURE_R2, CAPTURE_R1,
           CLIENT_IP, VIEW_TOKEN, CAPTURE_LOADER, CONTROLLER, ASSET_COUNT };
    Asset assets[ASSET_COUNT] = {
        [CAPTURE_PRX] = { "ps4_shell_feed.prx", MAX_BINARY_BYTES, NULL, 0 },
        [CAPTURE_R5] = { "capture-v5.prx", MAX_BINARY_BYTES, NULL, 0 },
        [CAPTURE_R4] = { "capture-v4.prx", MAX_BINARY_BYTES, NULL, 0 },
        [CAPTURE_R3] = { "capture-v3.prx", MAX_BINARY_BYTES, NULL, 0 },
        [CAPTURE_R2] = { "capture-v2.prx", MAX_BINARY_BYTES, NULL, 0 },
        [CAPTURE_R1] = { "capture-v1.prx", MAX_BINARY_BYTES, NULL, 0 },
        [CLIENT_IP] = { "client-ip", 17, NULL, 0 },
        [VIEW_TOKEN] = { "view-token", 66, NULL, 0 },
        [CAPTURE_LOADER] = { "ps4-ambilight.bin", MAX_BINARY_BYTES, NULL, 0 },
        [CONTROLLER] = { "control4free.elf", MAX_BINARY_BYTES, NULL, 0 },
    };
    const char *problem_asset = NULL;
    int asset_error = 0;
    char directory_error[128];
    FILE *log = NULL;
    const char *message;

    /* The package mount may be relative to the sandbox root. Read it before leaving. */
    for (size_t i = 0; i < ASSET_COUNT; ++i) {
        if (!read_asset(&assets[i])) {
            problem_asset = assets[i].name;
            asset_error = errno;
            break;
        }
    }
    if (c4fSandboxLeave() < 0) {
        message = "GoldHEN sandbox exit failed. Load GoldHEN and reopen this app.";
        goto finish;
    }
    if (!ensure_directory(SERVICE_DIR)) {
        snprintf(directory_error, sizeof directory_error, "Could not open or create %s (errno %d).",
                 SERVICE_DIR, errno);
        message = directory_error;
        goto finish;
    }
    const int log_fd = open(SERVICE_DIR "/launcher.log", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (log_fd >= 0) {
        log = fdopen(log_fd, "w");
        if (!log) close(log_fd);
    }
    if (!log) log = stderr;
    record(log, "PS4 LAN Service launch requested.", 0);

    if (problem_asset) {
        record(log, problem_asset, asset_error);
        message = "A packaged asset could not be read. See launcher.log.";
        goto finish;
    }
    if (!validate_config(&assets[CLIENT_IP], &assets[VIEW_TOKEN]) ||
        !valid_prx(&assets[CAPTURE_PRX]) || !valid_prx(&assets[CAPTURE_R5]) ||
        !valid_prx(&assets[CAPTURE_R4]) || !valid_prx(&assets[CAPTURE_R3]) ||
        !valid_prx(&assets[CAPTURE_R2]) || !valid_prx(&assets[CAPTURE_R1]) ||
        assets[CAPTURE_LOADER].size != 69216 || assets[CONTROLLER].size < 64 ||
        memcmp(assets[CONTROLLER].bytes, "\177ELF\2\1", 6)) {
        message = "A packaged asset failed validation. Reinstall the LAN Service package.";
        goto finish;
    }
    bool capture_upgraded = false;
    if (!ensure_directory(CAPTURE_DIR) ||
        !stage_file(CAPTURE_DIR "/ps4_shell_feed.prx", &assets[CAPTURE_PRX],
                    &assets[CAPTURE_R5], CAPTURE_R1 - CAPTURE_R5 + 1, &capture_upgraded)) {
        const int error = errno;
        record(log, "Could not stage the shell capture PRX", error);
        message = error == EEXIST ? "A different Ambilight shell plugin is already installed. See launcher.log."
                                  : "Could not stage the shell capture plugin. See launcher.log.";
        goto finish;
    }
    if (capture_upgraded)
        record(log, "Upgraded a previous PS4 LAN Service capture plugin.", 0);
    if (!stage_file(SERVICE_DIR "/client-ip", &assets[CLIENT_IP], NULL, 0, NULL) ||
        !stage_file(SERVICE_DIR "/view-token", &assets[VIEW_TOKEN], NULL, 0, NULL)) {
        record(log, "Could not stage the LAN service configuration", errno);
        message = "Could not stage the LAN service configuration. See launcher.log.";
        goto finish;
    }

    const struct in_addr address = console_address();
    const bool capture = send_payload(&assets[CAPTURE_LOADER], address);
    record(log, capture ? "Capture loader sent to GoldHEN." : "Capture loader transfer failed", capture ? 0 : errno);
    const bool controller = send_payload(&assets[CONTROLLER], address);
    record(log, controller ? "Controller payload sent to GoldHEN." : "Controller payload transfer failed", controller ? 0 : errno);
    message = capture && controller ? "Capture and controller payloads sent. Open the LAN dashboard."
        : "Payload transfer failed. Check GoldHEN PayLoader Server and launcher.log.";
    if (capture_upgraded && capture && controller)
        message = "Capture upgraded. If capture was running, restart the PS4, load GoldHEN, and reopen this app.";

finish:
    record(log ? log : stderr, message, 0);
    if (log && log != stderr) fclose(log);
    for (size_t i = 0; i < ASSET_COUNT; ++i) free(assets[i].bytes);
    notify(message);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(1000000);
}
