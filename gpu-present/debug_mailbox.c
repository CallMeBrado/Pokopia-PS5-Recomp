/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "debug_mailbox.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

enum { HEADER_BYTES = 16, PAD_BYTES = 36, CAPTURE_KIND = 1, PAD_KIND = 2 };

static uint64_t unpack(const unsigned char *bytes, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static bool make_directory(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

bool pokopia_debug_mailbox_init(PokopiaDebugMailbox *mailbox, const char *directory)
{
    if (!mailbox || !directory || strlen(directory) > POKOPIA_DEBUG_PATH_BYTES - 40)
        return false;
    if (strcmp(directory, POKOPIA_DEBUG_DIRECTORY) == 0 && !make_directory("/data/pokopia"))
        return false;
    if (!make_directory(directory))
        return false;
#ifndef _WIN32
    /* R70: 0777 like /data/pokopia, so the FTP server can drop requests and
     * fetch captures (0770 locked it out on the PS5). */
    if (strcmp(directory, POKOPIA_DEBUG_DIRECTORY) == 0 && chmod(directory, 0777) != 0)
        return false;
#endif
    *mailbox = (PokopiaDebugMailbox){ .directory = directory };
    return true;
}

static FILE *open_file(const char *path, const char *mode)
{
#ifdef _WIN32
    FILE *file = NULL;
    return fopen_s(&file, path, mode) == 0 ? file : NULL;
#else
    return fopen(path, mode);
#endif
}

static uint64_t request(const PokopiaDebugMailbox *mailbox, const char *name, uint16_t kind,
                        unsigned char *out, size_t bytes, uint64_t previous)
{
    if (!mailbox->directory)
        return 0;
    char path[POKOPIA_DEBUG_PATH_BYTES], claimed[POKOPIA_DEBUG_PATH_BYTES];
    snprintf(path, sizeof path, "%s/%s", mailbox->directory, name);
    snprintf(claimed, sizeof claimed, "%s/%s.claimed", mailbox->directory, name);
    if (rename(path, claimed) != 0)
        return 0;
    FILE *file = open_file(claimed, "rb");
    if (!file) {
        remove(claimed);
        return 0;
    }
    bool complete = fread(out, 1, bytes, file) == bytes && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    remove(claimed);
    if (!complete || memcmp(out, "PDBG", 4) || unpack(out + 4, 2) != 1 || unpack(out + 6, 2) != kind)
        return 0;
    uint64_t seq = unpack(out + 8, 8);
    return seq > previous ? seq : 0;
}

bool pokopia_debug_capture(PokopiaDebugMailbox *mailbox, char path[POKOPIA_DEBUG_PATH_BYTES])
{
    unsigned char header[HEADER_BYTES];
    uint64_t seq = request(mailbox, "capture.req", CAPTURE_KIND, header, sizeof header, mailbox->capture_seq);
    if (!seq)
        return false;
    mailbox->capture_seq = seq;
    snprintf(path, POKOPIA_DEBUG_PATH_BYTES, "%s/frame-%016" PRIx64 ".png", mailbox->directory, seq);
    return true;
}

static int16_t unpack_stick(const unsigned char *bytes)
{
    uint32_t value = (uint32_t)unpack(bytes, 2);
    return (int16_t)(value < 0x8000 ? (int32_t)value : (int32_t)value - 0x10000);
}

bool pokopia_debug_pad(PokopiaDebugMailbox *mailbox, HznDebugPadCommand *command)
{
    unsigned char raw[PAD_BYTES];
    uint64_t seq = request(mailbox, "pad.req", PAD_KIND, raw, sizeof raw, mailbox->pad_seq);
    if (!seq)
        return false;
    mailbox->pad_seq = seq;
    *command = (HznDebugPadCommand){
        .seq = seq, .buttons = unpack(raw + 16, 8),
        .lx = unpack_stick(raw + 24), .ly = unpack_stick(raw + 26),
        .rx = unpack_stick(raw + 28), .ry = unpack_stick(raw + 30),
        .hold_ms = (uint32_t)unpack(raw + 32, 4),
    };
    return true;
}

bool pokopia_debug_ack_pad(const PokopiaDebugMailbox *mailbox, uint64_t seq)
{
    if (!mailbox->directory || !seq || seq != mailbox->pad_seq)
        return false;
    char path[POKOPIA_DEBUG_PATH_BYTES], temp[POKOPIA_DEBUG_PATH_BYTES];
    snprintf(path, sizeof path, "%s/pad-%016" PRIx64 ".ack", mailbox->directory, seq);
    snprintf(temp, sizeof temp, "%s/pad-%016" PRIx64 ".tmp", mailbox->directory, seq);
    FILE *file = open_file(temp, "wb");
    if (!file)
        return false;
    unsigned char raw[8];
    for (size_t i = 0; i < sizeof raw; ++i)
        raw[i] = (unsigned char)(seq >> (8 * i));
    bool ok = fwrite(raw, 1, sizeof raw, file) == sizeof raw;
    if (fclose(file) != 0)
        ok = false;
    if (ok && rename(temp, path) == 0)
        return true;
    remove(temp);
    return false;
}
