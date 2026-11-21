/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pokopia-ctl: a small resident PS5 payload (send it once per boot to elfldr
 * on port 9021) that launches and closes apps on request, so the build
 * scripts can restart the title after a push without anyone at the console.
 *
 * Protocol: connect to TCP 9030, send one line, read one reply line.
 *   ping                 -> ok pong
 *   appid <TITLE>        -> ok <app id> | err <code>
 *   kill <TITLE>         -> ok | err <code>
 *   launch <TITLE>       -> ok | err <code>
 *   quit                 -> ok (the payload exits)
 */
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { CTL_PORT = 9030 };

int sceUserServiceInitialize(void *params);
int sceUserServiceGetForegroundUser(int *user_id);
int sceLncUtilGetAppId(const char *title_id);
int sceLncUtilKillApp(uint32_t app_id);

typedef struct {
    uint32_t size;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint32_t check_flag;
} LncAppParam;
int sceLncUtilLaunchApp(const char *title_id, const char **argv, LncAppParam *param);
int sceSystemServiceLaunchApp(const char *title_id, const char **argv, void *param);

static int valid_title(const char *title)
{
    if (strlen(title) != 9)
        return 0;
    for (const char *c = title; *c; ++c)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')))
            return 0;
    return 1;
}

static void handle(const char *line, char *reply, size_t size)
{
    char command[16] = {0}, title[16] = {0};
    if (sscanf(line, "%15s %15s", command, title) < 1) {
        snprintf(reply, size, "err parse\n");
        return;
    }
    if (!strcmp(command, "ping")) {
        snprintf(reply, size, "ok pong\n");
        return;
    }
    if (!strcmp(command, "quit")) {
        snprintf(reply, size, "ok\n");
        return;
    }
    if (!valid_title(title)) {
        snprintf(reply, size, "err title\n");
        return;
    }
    if (!strcmp(command, "appid")) {
        const int id = sceLncUtilGetAppId(title);
        snprintf(reply, size, id < 0 ? "err %#x\n" : "ok %#x\n", (unsigned)id);
    } else if (!strcmp(command, "kill")) {
        const int id = sceLncUtilGetAppId(title);
        if (id < 0) {
            snprintf(reply, size, "err not-running %#x\n", (unsigned)id);
            return;
        }
        const int result = sceLncUtilKillApp((uint32_t)id);
        snprintf(reply, size, result ? "err kill %#x\n" : "ok\n", (unsigned)result);
    } else if (!strcmp(command, "launch")) {
        int user = -1;
        sceUserServiceGetForegroundUser(&user);
        LncAppParam param = {.size = sizeof param, .user_id = (uint32_t)user};
        int result = sceLncUtilLaunchApp(title, NULL, &param);
        if (result < 0) {
            const int fallback = sceSystemServiceLaunchApp(title, NULL, NULL);
            if (fallback >= 0)
                result = fallback;
            else {
                snprintf(reply, size, "err launch lnc=%#x sys=%#x\n", (unsigned)result, (unsigned)fallback);
                return;
            }
        }
        snprintf(reply, size, "ok %#x\n", (unsigned)result);
    } else {
        snprintf(reply, size, "err command\n");
    }
}

int main(void)
{
    sceUserServiceInitialize(NULL);
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return 1;
    }
    const int on = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(CTL_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(server, (struct sockaddr *)&address, sizeof address) || listen(server, 4)) {
        perror("bind/listen");
        close(server);
        return 1;
    }
    printf("pokopia-ctl listening on %d\n", CTL_PORT);
    for (;;) {
        const int client = accept(server, NULL, NULL);
        if (client < 0)
            continue;
        char line[128] = {0};
        size_t used = 0;
        while (used + 1 < sizeof line) {
            const ssize_t got = read(client, line + used, 1);
            if (got <= 0 || line[used] == '\n')
                break;
            used += (size_t)got;
        }
        line[used] = 0;
        char reply[128];
        handle(line, reply, sizeof reply);
        write(client, reply, strlen(reply));
        close(client);
        if (!strncmp(line, "quit", 4))
            break;
    }
    close(server);
    return 0;
}
