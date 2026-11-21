/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "debug_mailbox.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void publish(const char *directory, const char *name, const unsigned char *request, size_t bytes)
{
    char path[POKOPIA_DEBUG_PATH_BYTES];
    int length = snprintf(path, sizeof path, "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof path);
#ifdef _WIN32
    FILE *file = NULL;
    assert(fopen_s(&file, path, "wb") == 0);
#else
    FILE *file = fopen(path, "wb");
#endif
    assert(file && fwrite(request, 1, bytes, file) == bytes && fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    PokopiaDebugMailbox mailbox;
    assert(pokopia_debug_mailbox_init(&mailbox, argv[1]));
    unsigned char capture[] = {'P','D','B','G',1,0,1,0,2,0,0,0,0,0,0,0,0};
    char filename[POKOPIA_DEBUG_PATH_BYTES];
    publish(argv[1], "capture.req", capture, sizeof capture);
    assert(!pokopia_debug_capture(&mailbox, filename));
    publish(argv[1], "capture.req", capture, sizeof capture - 1);
    assert(pokopia_debug_capture(&mailbox, filename));
    assert(strstr(filename, "frame-0000000000000002.png"));
    assert(!pokopia_debug_capture(&mailbox, filename));
    capture[8] = 1;
    publish(argv[1], "capture.req", capture, sizeof capture - 1);
    assert(!pokopia_debug_capture(&mailbox, filename));

    const unsigned char input[] = {
        'P','D','B','G',1,0,2,0,3,0,0,0,0,0,0,0,
        1,0,0,0,0,0,0,0,0xff,0x7f,1,0x80,0xff,0xff,1,0,0x88,0x13,0,0,
    };
    publish(argv[1], "pad.req", input, sizeof input);
    HznDebugPadCommand command;
    assert(pokopia_debug_pad(&mailbox, &command));
    assert(command.seq == 3 && command.buttons == 1 && command.hold_ms == 5000);
    assert(command.lx == 32767 && command.ly == -32767 && command.rx == -1 && command.ry == 1);
    assert(!pokopia_debug_ack_pad(&mailbox, 2));
    assert(pokopia_debug_ack_pad(&mailbox, 3));
    assert(!pokopia_debug_pad(&mailbox, &command));
    snprintf(filename, sizeof filename, "%s/pad-0000000000000003.ack", argv[1]);
    assert(remove(filename) == 0);
    puts("PASS screenshot and controller mailbox parsing, one-shot claims and replay rejection");
    return 0;
}
