/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "capture_stream.h"
#include "capture_memory.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <orbis/libkernel.h>
#endif

#ifndef PS4_REMOTE_CONFIG_DIR
#define PS4_REMOTE_CONFIG_DIR "/data/ps4-remote-service"
#endif
#ifndef PS4_REMOTE_CAPTURE_PORT
#define PS4_REMOTE_CAPTURE_PORT 4265
#endif
#ifdef __FreeBSD__
/* The OpenOrbis headers use Linux values for these two FreeBSD constants. */
#define PS4_REMOTE_MONOTONIC 4
#define PS4_REMOTE_NO_SIGNAL 0x20000
#else
#define PS4_REMOTE_MONOTONIC CLOCK_MONOTONIC
#define PS4_REMOTE_NO_SIGNAL MSG_NOSIGNAL
#endif

enum { JPEG_LIMIT = 512 * 1024, HEADER_LIMIT = 2048, TOKEN_BYTES = 64, IO_TIMEOUT_MS = 400,
       LISTEN_POLL_MS = 100, FLIP_GRACE_MS = 100 };
enum { REPORTED_COPY_FAILURE = 1, REPORTED_FLIP = 2, REPORTED_JPEG_FAILURE = 4,
       REPORTED_STABLE_FRAME = 8, REPORTED_OVERLAP_FRAME = 16 };

typedef struct {
    Ps4RemoteCaptureFrame *sampler;
    pthread_t thread;
    int listener;
    int discovery;
    struct in_addr client;
    char token[TOKEN_BYTES + 1];
    pthread_mutex_t latest_lock;
    atomic_bool stopping;
    atomic_uint_fast64_t requested_at;
    uint64_t captured_at;
    uint64_t overlap_since;
    unsigned reported;
    uint8_t work[JPEG_LIMIT], latest[JPEG_LIMIT], response[JPEG_LIMIT];
    size_t latest_bytes;
} CaptureStream;

static CaptureStream *stream;

void ps4_remote_capture_status(const char *stage, int result)
{
#if defined(__FreeBSD__)
    /* The compositor can write here before the network worker is ready. */
    int fd = sceKernelOpen("/data/ps4-ambilight/remote-start.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    char line[192];
    int length = snprintf(line, sizeof line, "%s (%d)\n", stage, result);
    if (length > 0 && (size_t)length < sizeof line)
        sceKernelWrite(fd, line, (size_t)length);
    sceKernelClose(fd);
#else
    (void)stage;
    (void)result;
#endif
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(PS4_REMOTE_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void report_once(CaptureStream *capture, unsigned event, const char *stage, int result)
{
    if (!(capture->reported & event)) {
        capture->reported |= event;
        ps4_remote_capture_status(stage, result);
    }
}

static bool read_line(const char *path, char *value, size_t capacity)
{
#if defined(__FreeBSD__)
    /* Ambilight's worker must use kernel file I/O. libc stdio has faulted
       when called on a plugin worker in an injected process. */
    int fd = sceKernelOpen(path, O_RDONLY, 0);
#else
    int fd = open(path, O_RDONLY);
#endif
    if (fd < 0) {
        ps4_remote_capture_status(path, fd);
        return false;
    }
    size_t count = 0;
    ssize_t got;
    do {
#if defined(__FreeBSD__)
        got = (ssize_t)sceKernelRead(fd, value + count, capacity - count);
#else
        got = read(fd, value + count, capacity - count);
        if (got < 0 && errno == EINTR) continue;
#endif
        if (got <= 0) break;
        count += (size_t)got;
    } while (count < capacity);
#if defined(__FreeBSD__)
    sceKernelClose(fd);
#else
    close(fd);
#endif
    if (got < 0 || !count || count >= capacity || memchr(value, 0, count))
        return false;
    if (value[count - 1] == '\n')
        --count;
    if (count && value[count - 1] == '\r')
        --count;
    value[count] = 0;
    return count != 0;
}

static bool load_config(CaptureStream *capture)
{
    char ip[32], token[TOKEN_BYTES + 3];
    if (!read_line(PS4_REMOTE_CONFIG_DIR "/client-ip", ip, sizeof ip) ||
        inet_pton(AF_INET, ip, &capture->client) != 1 ||
        !read_line(PS4_REMOTE_CONFIG_DIR "/view-token", token, sizeof token) ||
        strlen(token) != TOKEN_BYTES || strspn(token, "0123456789abcdef") != TOKEN_BYTES)
        return false;
    memcpy(capture->token, token, sizeof capture->token);
    return true;
}

static bool authorized_request(char *request, const char *token)
{
    char *line = strstr(request, "\r\n");
    if (!line)
        return false;
    *line = '\0';
    if (strcmp(request, "GET /frame.jpg HTTP/1.1") && strcmp(request, "GET /frame.jpg HTTP/1.0"))
        return false;
    bool authorized = false;
    for (char *start = line + 2; (line = strstr(start, "\r\n")) && line != start; start = line + 2) {
        *line = '\0';
        const char prefix[] = "Authorization: Bearer ";
        if (strncasecmp(start, prefix, sizeof prefix - 1))
            continue;
        if (authorized || strlen(start) != sizeof prefix - 1 + TOKEN_BYTES)
            return false;
        unsigned mismatch = 0;
        for (size_t i = 0; i < TOKEN_BYTES; ++i)
            mismatch |= (uint8_t)start[sizeof prefix - 1 + i] ^ (uint8_t)token[i];
        if (mismatch)
            return false;
        authorized = true;
    }
    return authorized;
}

static bool io_ready(int fd, short event, uint64_t deadline)
{
    while (true) {
        uint64_t now = monotonic_ms();
        if (!now || now >= deadline)
            return false;
        struct pollfd descriptor = { .fd = fd, .events = event };
        int result = poll(&descriptor, 1, (int)(deadline - now));
        if (result >= 0)
            return result == 1 && (descriptor.revents & event);
        if (errno != EINTR)
            return false;
    }
}

static bool send_bytes(int fd, const void *bytes, size_t count, uint64_t deadline)
{
    const uint8_t *next = bytes;
    while (count && io_ready(fd, POLLOUT, deadline)) {
        ssize_t sent = send(fd, next, count, PS4_REMOTE_NO_SIGNAL);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (sent <= 0)
            return false;
        next += sent;
        count -= (size_t)sent;
    }
    return !count;
}

static bool read_request(int fd, char request[HEADER_LIMIT], uint64_t deadline)
{
    size_t used = 0;
    while (used + 1 < HEADER_LIMIT && io_ready(fd, POLLIN, deadline)) {
        ssize_t got = recv(fd, request + used, HEADER_LIMIT - 1 - used, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (got <= 0 || memchr(request + used, 0, (size_t)got))
            return false;
        used += (size_t)got;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n"))
            return true;
    }
    return false;
}

static void serve_frame(CaptureStream *capture, int client)
{
    const uint64_t start = monotonic_ms();
    char request[HEADER_LIMIT];
    if (!start || fcntl(client, F_SETFL, O_NONBLOCK) != 0 ||
        !read_request(client, request, start + IO_TIMEOUT_MS))
        return;
    const uint64_t now = monotonic_ms();
    if (!now)
        return;
    const uint64_t deadline = now + IO_TIMEOUT_MS;
    if (!authorized_request(request, capture->token)) {
        const char denied[] = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        send_bytes(client, denied, sizeof denied - 1, deadline);
        return;
    }
    atomic_store(&capture->requested_at, now);
    pthread_mutex_lock(&capture->latest_lock);
    const uint64_t read_at = monotonic_ms();
    const size_t bytes = read_at >= capture->captured_at && read_at - capture->captured_at <= 1000
        ? capture->latest_bytes : 0;
    if (bytes)
        memcpy(capture->response, capture->latest, bytes);
    pthread_mutex_unlock(&capture->latest_lock);
    if (!bytes) {
        const char missing[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 0\r\n\r\n";
        send_bytes(client, missing, sizeof missing - 1, deadline);
        return;
    }
    char header[160];
    int count = snprintf(header, sizeof header, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                         "Connection: close\r\nCache-Control: no-store\r\nContent-Length: %zu\r\n\r\n", bytes);
    if (count > 0 && (size_t)count < sizeof header && send_bytes(client, header, (size_t)count, deadline))
        send_bytes(client, capture->response, bytes, deadline);
}

static void answer_discovery(CaptureStream *capture)
{
    const char request[] = "P4LAN-DISCOVER/1";
    const char response[] = "P4LAN-SERVICE/1";
    char packet[sizeof request];
    struct sockaddr_in peer = {0};
    socklen_t size = sizeof peer;
    /* The extra byte makes oversized datagrams fail the exact-length check. */
    const ssize_t count = recvfrom(capture->discovery, packet, sizeof packet, 0,
                                   (struct sockaddr *)&peer, &size);
    if (count == (ssize_t)(sizeof request - 1) && size == sizeof peer && peer.sin_family == AF_INET &&
        peer.sin_addr.s_addr == capture->client.s_addr && !memcmp(packet, request, sizeof request - 1)) {
        sendto(capture->discovery, response, sizeof response - 1, 0,
               (const struct sockaddr *)&peer, size);
    }
}

static void accept_frame_request(CaptureStream *capture)
{
    struct sockaddr_in peer = {0};
    socklen_t size = sizeof peer;
    int client = accept(capture->listener, (struct sockaddr *)&peer, &size);
    if (client < 0)
        return;
    if (peer.sin_family == AF_INET && peer.sin_addr.s_addr == capture->client.s_addr)
        serve_frame(capture, client);
    close(client);
}

static void *listen_for_frames(void *opaque)
{
    CaptureStream *capture = opaque;
    struct pollfd sockets[] = {
        { .fd = capture->listener, .events = POLLIN },
        { .fd = capture->discovery, .events = POLLIN },
    };
    while (!atomic_load(&capture->stopping)) {
        if (poll(sockets, sizeof sockets / sizeof sockets[0], LISTEN_POLL_MS) <= 0)
            continue;
        if (atomic_load(&capture->stopping))
            break;
        if (sockets[1].revents & POLLIN)
            answer_discovery(capture);
        if (sockets[0].revents & POLLIN)
            accept_frame_request(capture);
    }
    return NULL;
}

bool ps4_remote_stream_start(Ps4RemotePixelOffset pixel_offset)
{
    if (stream)
        return false;
    int error;
    CaptureStream *capture = capture_allocate(sizeof *capture, &error);
    if (!capture) {
        ps4_remote_capture_status("Could not map capture stream buffers", error);
        return false;
    }
    capture->listener = -1;
    capture->discovery = -1;
    const char *failure = "Could not read or validate capture settings";
    if (!load_config(capture))
        goto fail;
    failure = "Could not allocate frame sampler";
    if (!(capture->sampler = ps4_remote_capture_frame_create(pixel_offset, &error)))
        goto fail;
    failure = "Could not open TCP socket";
    capture->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (capture->listener < 0) {
        error = errno;
        goto fail;
    }
    const int yes = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(PS4_REMOTE_CAPTURE_PORT),
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
    };
#if defined(__FreeBSD__)
    address.sin_len = sizeof address;
#endif
    failure = "Could not bind capture listener";
    if (setsockopt(capture->listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) != 0 ||
        fcntl(capture->listener, F_SETFL, O_NONBLOCK) != 0 ||
        bind(capture->listener, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(capture->listener, 4) != 0) {
        error = errno;
        goto fail;
    }
    failure = "Could not initialize capture mutex";
    error = pthread_mutex_init(&capture->latest_lock, NULL);
    if (error)
        goto fail;
    capture->discovery = socket(AF_INET, SOCK_DGRAM, 0);
    if (capture->discovery < 0 || fcntl(capture->discovery, F_SETFL, O_NONBLOCK) != 0 ||
        bind(capture->discovery, (struct sockaddr *)&address, sizeof address) != 0) {
        const int error = errno;
        if (capture->discovery >= 0)
            close(capture->discovery);
        capture->discovery = -1;
        ps4_remote_capture_status("Discovery socket unavailable", error);
    }
    atomic_init(&capture->stopping, false);
    atomic_init(&capture->requested_at, 0);
    failure = "Could not create capture network worker";
    error = pthread_create(&capture->thread, NULL, listen_for_frames, capture);
    if (error) {
        pthread_mutex_destroy(&capture->latest_lock);
        goto fail;
    }
    stream = capture;
    ps4_remote_capture_status("Capture listener started", PS4_REMOTE_CAPTURE_PORT);
    return true;

fail:
    ps4_remote_capture_status(failure, error);
    if (capture->discovery >= 0)
        close(capture->discovery);
    if (capture->listener >= 0)
        close(capture->listener);
    ps4_remote_capture_frame_destroy(capture->sampler);
    capture_release(capture, sizeof *capture);
    return false;
}

void ps4_remote_stream_submit(const void *tiled, size_t tiled_bytes, uint64_t sequence,
                              uint64_t (*snapshot)(void))
{
    CaptureStream *capture = stream;
    if (!capture)
        return;
    const uint64_t request = atomic_load(&capture->requested_at);
    const uint64_t now = monotonic_ms();
    if (!request || !now || now - request > 2000)
        return;
    if (!ps4_remote_capture_frame_copy(capture->sampler, tiled, tiled_bytes)) {
        report_once(capture, REPORTED_COPY_FAILURE, "Frame copy rejected the buffer", 0);
        return;
    }
    const bool overlap = snapshot && snapshot() != sequence;
    if (overlap) {
        report_once(capture, REPORTED_FLIP, "Flip changed during copy, elapsed ms", (int)(monotonic_ms() - now));
        if (!capture->overlap_since)
            capture->overlap_since = now;
        /* With two compositor buffers, a full copy can span every flip.
           Prefer stable copies and serve a readable copy if that starves video. */
        if (now - capture->overlap_since < FLIP_GRACE_MS || now - capture->captured_at < FLIP_GRACE_MS)
            return;
    } else {
        capture->overlap_since = 0;
    }
    size_t bytes;
    if (!ps4_remote_capture_jpeg(capture->sampler, capture->work, sizeof capture->work, &bytes)) {
        report_once(capture, REPORTED_JPEG_FAILURE, "JPEG encoding failed or exceeded its limit", 0);
        return;
    }
    pthread_mutex_lock(&capture->latest_lock);
    memcpy(capture->latest, capture->work, bytes);
    capture->captured_at = now;
    capture->latest_bytes = bytes;
    pthread_mutex_unlock(&capture->latest_lock);
    report_once(capture, overlap ? REPORTED_OVERLAP_FRAME : REPORTED_STABLE_FRAME,
                overlap ? "Published overlapping frame, bytes" : "Published stable frame, bytes", (int)bytes);
}

void ps4_remote_stream_stop(void)
{
    CaptureStream *capture = stream;
    if (!capture)
        return;
    stream = NULL;
    atomic_store(&capture->stopping, true);
    pthread_join(capture->thread, NULL);
    if (capture->discovery >= 0)
        close(capture->discovery);
    close(capture->listener);
    pthread_mutex_destroy(&capture->latest_lock);
    ps4_remote_capture_frame_destroy(capture->sampler);
    capture_release(capture, sizeof *capture);
}
