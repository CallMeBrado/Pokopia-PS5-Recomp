/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _CRT_SECURE_NO_WARNINGS
#include "save_store.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum {
    FS_GENERIC_FAILURE = 0x3e802,
    FS_TARGET_NOT_FOUND = 0x7d402,
    FS_INVALID_ARGUMENT = 0x2ee202,
};

typedef enum { NODE_ERROR, NODE_MISSING, NODE_DIRECTORY, NODE_FILE } SaveNode;

static SaveNode node(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? NODE_MISSING : NODE_ERROR;
    }
    if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
        return NODE_ERROR;
    return attrs & FILE_ATTRIBUTE_DIRECTORY ? NODE_DIRECTORY : NODE_FILE;
#else
    struct stat status;
    if (lstat(path, &status))
        return errno == ENOENT ? NODE_MISSING : NODE_ERROR;
    if (S_ISDIR(status.st_mode))
        return NODE_DIRECTORY;
    return S_ISREG(status.st_mode) ? NODE_FILE : NODE_ERROR;
#endif
}

static int ensure_dir(const char *path)
{
    if (node(path) == NODE_MISSING) {
#ifdef _WIN32
        CreateDirectoryA(path, NULL);
#else
        mkdir(path, 0700);
#endif
    }
    return node(path) == NODE_DIRECTORY;
}

static int publish_meta(const char *path, const char *bytes, size_t length)
{
    SaveNode old = node(path);
    if (old != NODE_MISSING)
        return old == NODE_FILE;
    char temporary[HZN_SAVE_PATH_MAX + 28];
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    for (unsigned suffix = 0; suffix < 16; ++suffix) {
        int name_length = snprintf(temporary, sizeof temporary, "%s.tmp-%lu-%u", path, pid, suffix);
        if (name_length < 0 || (size_t)name_length >= sizeof temporary)
            return 0;
#ifdef _WIN32
        HANDLE file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS)
                continue;
            return 0;
        }
        DWORD done = 0;
        int written = WriteFile(file, bytes, (DWORD)length, &done, NULL) && done == length &&
                      FlushFileBuffers(file);
        if (!CloseHandle(file))
            written = 0;
        if (written && MoveFileExA(temporary, path, MOVEFILE_WRITE_THROUGH))
            return 1;
        DeleteFileA(temporary);
#else
        int file = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (file < 0) {
            if (errno == EEXIST)
                continue;
            return 0;
        }
        size_t offset = 0;
        while (offset < length) {
            ssize_t done = write(file, bytes + offset, length - offset);
            if (done < 0 && errno == EINTR)
                continue;
            if (done <= 0)
                break;
            offset += (size_t)done;
        }
        int written = offset == length && !fsync(file);
        if (close(file))
            written = 0;
        /* Applet and FSP dispatch are serialized in the single title process. */
        if (written && node(path) == NODE_MISSING && !rename(temporary, path))
            return 1;
        unlink(temporary);
#endif
        return node(path) == NODE_FILE;
    }
    return 0;
}

static void put_word(uint8_t *out, uint64_t word)
{
    for (unsigned i = 0; i < 8; ++i)
        out[i] = (uint8_t)(word >> (8 * i));
}

static void hex(char *out, const uint8_t *bytes, size_t count)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * count] = '\0';
}

static uint32_t account(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                        char path[HZN_SAVE_PATH_MAX], int create)
{
    if (!root || !*root || (!uid_hi && !uid_lo) || !path)
        return FS_INVALID_ARGUMENT;
    size_t length = strlen(root);
    if (length >= HZN_SAVE_PATH_MAX - sizeof "/account/00000000000000000000000000000000.meta" ||
        root[length - 1] == '/' || root[length - 1] == '\\')
        return FS_INVALID_ARGUMENT;
    uint8_t attr[64] = {0};
    put_word(attr, HZN_SAVE_TITLE_ID);
    put_word(attr + 8, uid_hi);
    put_word(attr + 16, uid_lo);
    attr[32] = 1;  /* Account save data. */
    char uid[33], parent[HZN_SAVE_PATH_MAX], meta[HZN_SAVE_PATH_MAX];
    hex(uid, attr + 8, 16);
    snprintf(parent, sizeof parent, "%s/account", root);
    snprintf(path, HZN_SAVE_PATH_MAX, "%s/%s", parent, uid);
    snprintf(meta, sizeof meta, "%s.meta", path);
    if (!create) {
        SaveNode root_type = node(root);
        SaveNode parent_type = root_type == NODE_DIRECTORY ? node(parent) : root_type;
        if (parent_type == NODE_MISSING)
            return FS_TARGET_NOT_FOUND;
        if (parent_type != NODE_DIRECTORY)
            return FS_GENERIC_FAILURE;
        SaveNode data = node(path), metadata = node(meta);
        if ((data != NODE_DIRECTORY && data != NODE_MISSING) ||
            (metadata != NODE_FILE && metadata != NODE_MISSING))
            return FS_GENERIC_FAILURE;
        return data == NODE_DIRECTORY && metadata == NODE_FILE ? 0 : FS_TARGET_NOT_FOUND;
    }
    if (!ensure_dir(root) || !ensure_dir(parent) || !ensure_dir(path))
        return FS_GENERIC_FAILURE;
    char attr_hex[129], contents[224];
    hex(attr_hex, attr, sizeof attr);
    /* This title's control metadata specifies 64 MiB each for data and journal. */
    int size = snprintf(contents, sizeof contents,
                        "size=67108864\njournal=67108864\nowner=400a3a01ee4c000\nattr=%s\n", attr_hex);
    return size > 0 && (size_t)size < sizeof contents && publish_meta(meta, contents, (size_t)size)
         ? 0 : FS_GENERIC_FAILURE;
}

uint32_t hzn_save_ensure_account(const char *root, uint64_t uid_hi, uint64_t uid_lo)
{
    char path[HZN_SAVE_PATH_MAX];
    return account(root, uid_hi, uid_lo, path, 1);
}

uint32_t hzn_save_open_account(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               char path[HZN_SAVE_PATH_MAX])
{
    return account(root, uid_hi, uid_lo, path, 0);
}
