/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define _CRT_SECURE_NO_WARNINGS
#include "save_store.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

enum {
    FS_PATH_NOT_FOUND = 0x202,
    FS_PATH_ALREADY_EXISTS = 0x402,
    FS_GENERIC_FAILURE = 0x3e802,
    FS_TARGET_NOT_FOUND = 0x7d402,
    FS_OUT_OF_RANGE = 0x177a02,
    FS_INVALID_ARGUMENT = 0x2ee202,
    FS_NO_APPEND = 0x307202,
};

typedef enum { NODE_ERROR, NODE_MISSING, NODE_DIRECTORY, NODE_FILE } SaveNode;
enum { SAVE_ENTRY_PATH_MAX = HZN_SAVE_PATH_MAX + 0x301 };

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
#elif defined(__PROSPERO__)
    /* PS5 run 5: the account save root was never created, so node() reported
     * something other than "missing" for it. Use stat() (no symlinks under
     * /data here) and record the first unexpected failure of each call. */
    struct stat status;
    if (stat(path, &status)) {
        const int stat_errno = errno;
        static int reported;
        if (stat_errno != ENOENT && !reported) {
            reported = 1;
            struct stat link_status;
            const int lstat_result = lstat(path, &link_status);
            const int lstat_errno = lstat_result ? errno : 0;
            FILE *diag = fopen("/data/pokopia/ps5-save-diag.txt", "a");
            if (diag) {
                fprintf(diag, "node(%s): stat errno=%d lstat result=%d errno=%d\n", path, stat_errno,
                        lstat_result, lstat_errno);
                fclose(diag);
            }
        }
        errno = stat_errno;
        return stat_errno == ENOENT ? NODE_MISSING : NODE_ERROR;
    }
    if (S_ISDIR(status.st_mode))
        return NODE_DIRECTORY;
    return S_ISREG(status.st_mode) ? NODE_FILE : NODE_ERROR;
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
    int size = snprintf(contents, sizeof contents,
                        "size=%" PRId64 "\njournal=%" PRId64 "\nowner=400a3a01ee4c000\nattr=%s\n",
                        HZN_SAVE_ACCOUNT_DATA_LIMIT, HZN_SAVE_ACCOUNT_JOURNAL_LIMIT, attr_hex);
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

static uint32_t open_regular_file(const char *path, FILE **stream, uint32_t not_found, bool write)
{
#ifdef _WIN32
    const DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                        (write ? FILE_FLAG_WRITE_THROUGH : 0);
    HANDLE file = CreateFileA(path, access, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
             ? not_found : FS_GENERIC_FAILURE;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))) {
        CloseHandle(file);
        return FS_GENERIC_FAILURE;
    }
    int fd = _open_osfhandle((intptr_t)file, (write ? _O_RDWR : _O_RDONLY) | _O_BINARY);
    if (fd < 0) {
        CloseHandle(file);
        return FS_GENERIC_FAILURE;
    }
    *stream = _fdopen(fd, write ? "r+b" : "rb");
    if (!*stream)
        _close(fd);
#else
    int fd = open(path, (write ? O_RDWR | O_SYNC : O_RDONLY) | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? not_found : FS_GENERIC_FAILURE;
    struct stat status;
    if (!fstat(fd, &status) && S_ISREG(status.st_mode))
        *stream = fdopen(fd, write ? "r+b" : "rb");
    if (!*stream)
        close(fd);
#endif
    return *stream ? 0 : FS_GENERIC_FAILURE;
}

static bool parse_quota(const char *value, int64_t *quota)
{
    if (*value < '0' || *value > '9')
        return false;
    errno = 0;
    char *end;
    const long long parsed = strtoll(value, &end, 10);
    if (errno || parsed < 0 || (*end && strcmp(end, "\n") && strcmp(end, "\r\n")))
        return false;
    *quota = parsed;
    return true;
}

uint32_t hzn_save_account_size(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               int64_t *normal, int64_t *journal)
{
    if (!normal || !journal)
        return FS_INVALID_ARGUMENT;
    char path[HZN_SAVE_PATH_MAX], meta[HZN_SAVE_PATH_MAX + sizeof ".meta"];
    uint32_t result = hzn_save_open_account(root, uid_hi, uid_lo, path);
    if (result)
        return result;
    snprintf(meta, sizeof meta, "%s.meta", path);
    FILE *stream = NULL;
    result = open_regular_file(meta, &stream, FS_TARGET_NOT_FOUND, false);
    if (result)
        return result;

    char line[224];
    int64_t quotas[2];
    unsigned seen = 0;
    while (fgets(line, sizeof line, stream)) {
        if (!strchr(line, '\n') && !feof(stream)) {
            result = FS_GENERIC_FAILURE;
            break;
        }
        unsigned field;
        size_t prefix;
        if (!strncmp(line, "size=", 5)) {
            field = 0;
            prefix = 5;
        } else if (!strncmp(line, "journal=", 8)) {
            field = 1;
            prefix = 8;
        } else {
            continue;
        }
        if ((seen & (1u << field)) || !parse_quota(line + prefix, &quotas[field])) {
            result = FS_GENERIC_FAILURE;
            break;
        }
        seen |= 1u << field;
    }
    if (ferror(stream) || seen != 3)
        result = FS_GENERIC_FAILURE;
    if (fclose(stream))
        result = FS_GENERIC_FAILURE;
    if (!result) {
        *normal = quotas[0];
        *journal = quotas[1];
    }
    return result;
}

static uint32_t resolve_entry(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                              const char *guest_path, char path[SAVE_ENTRY_PATH_MAX],
                              SaveNode *type, bool allow_missing_leaf)
{
    if (!guest_path || guest_path[0] != '/')
        return FS_INVALID_ARGUMENT;
    uint32_t result = hzn_save_open_account(root, uid_hi, uid_lo, path);
    if (result)
        return result == FS_TARGET_NOT_FOUND ? FS_PATH_NOT_FOUND : result;
    *type = NODE_DIRECTORY;
    size_t used = strlen(path);
    for (const char *part = guest_path + 1; *part;) {
        const char *separator = strchr(part, '/');
        size_t length = separator ? (size_t)(separator - part) : strlen(part);
        if (!length || length >= SAVE_ENTRY_PATH_MAX - used - 1 ||
            part[length - 1] == '.' || part[length - 1] == ' ' ||
            memchr(part, '\\', length) || memchr(part, ':', length))
            return FS_INVALID_ARGUMENT;
        path[used++] = '/';
        memcpy(path + used, part, length);
        used += length;
        path[used] = '\0';
        *type = node(path);
        if (*type == NODE_ERROR)
            return FS_GENERIC_FAILURE;
        if (separator ? *type != NODE_DIRECTORY : (*type == NODE_MISSING && !allow_missing_leaf))
            return FS_PATH_NOT_FOUND;
        if (!separator)
            break;
        part = separator + 1;
        if (!*part)
            return FS_INVALID_ARGUMENT;
    }
    return 0;
}

#ifndef _WIN32
static bool sync_directory(const char *path)
{
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
        return false;
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
}

static bool sync_parent(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    *slash = '\0';
    const bool synced = sync_directory(path);
    *slash = '/';
    return synced;
}
#endif

uint32_t hzn_save_create_directory(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                                   const char *guest_path)
{
    char path[SAVE_ENTRY_PATH_MAX];
    SaveNode type;
    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &type, true);
    if (result)
        return result;
    if (type != NODE_MISSING)
        return FS_PATH_ALREADY_EXISTS;
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL))
        return 0;
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
        return FS_PATH_ALREADY_EXISTS;
    if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND)
        return FS_PATH_NOT_FOUND;
#else
    if (!mkdir(path, 0700)) {
        if (sync_parent(path))
            return 0;
        rmdir(path);
        return FS_GENERIC_FAILURE;
    }
    if (errno == EEXIST)
        return FS_PATH_ALREADY_EXISTS;
    if (errno == ENOENT || errno == ENOTDIR)
        return FS_PATH_NOT_FOUND;
#endif
    return FS_GENERIC_FAILURE;
}

uint32_t hzn_save_create_file(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                              const char *guest_path, uint64_t size)
{
    if (size > INT64_MAX)
        return FS_OUT_OF_RANGE;
    char path[SAVE_ENTRY_PATH_MAX];
    SaveNode type;
    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &type, true);
    if (result)
        return result;
    if (type != NODE_MISSING)
        return FS_PATH_ALREADY_EXISTS;
    if (size) {
        uint64_t free_bytes;
        result = hzn_save_free_space(root, uid_hi, uid_lo, "/", &free_bytes);
        if (result)
            return result;
        if (size > free_bytes)
            return FS_GENERIC_FAILURE;
    }
#ifdef _WIN32
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
            return FS_PATH_ALREADY_EXISTS;
        if (error == ERROR_PATH_NOT_FOUND || error == ERROR_FILE_NOT_FOUND)
            return FS_PATH_NOT_FOUND;
        return FS_GENERIC_FAILURE;
    }
    const LARGE_INTEGER end = {.QuadPart = (int64_t)size};
    const bool sized = !size || (SetFilePointerEx(file, end, NULL, FILE_BEGIN) && SetEndOfFile(file));
    const bool synced = sized && FlushFileBuffers(file);
    if (CloseHandle(file) && synced)
        return 0;
    DeleteFileA(path);
#else
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) {
        if (errno == EEXIST)
            return FS_PATH_ALREADY_EXISTS;
        if (errno == ENOENT || errno == ENOTDIR)
            return FS_PATH_NOT_FOUND;
        return FS_GENERIC_FAILURE;
    }
    const bool sized = !size || ftruncate(file, (off_t)size) == 0;
    const bool synced = sized && fsync(file) == 0;
    if (close(file) == 0 && synced && sync_parent(path))
        return 0;
    unlink(path);
#endif
    return FS_GENERIC_FAILURE;
}

uint32_t hzn_save_get_entry_type(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                                 const char *guest_path, uint32_t *type)
{
    if (!type)
        return FS_INVALID_ARGUMENT;
    char path[SAVE_ENTRY_PATH_MAX];
    SaveNode node_type;
    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &node_type, false);
    if (!result)
        *type = node_type == NODE_DIRECTORY ? 0 : 1;
    return result;
}

static uint32_t open_save_file(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               const char *guest_path, FILE **stream, bool write)
{
    if (!stream)
        return FS_INVALID_ARGUMENT;
    *stream = NULL;
    if (!guest_path || guest_path[0] != '/' || !guest_path[1])
        return FS_INVALID_ARGUMENT;
    char path[SAVE_ENTRY_PATH_MAX];
    SaveNode node_type;
    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &node_type, false);
    if (result)
        return result;
    if (node_type != NODE_FILE)
        return FS_PATH_NOT_FOUND;
    return open_regular_file(path, stream, FS_PATH_NOT_FOUND, write);
}

uint32_t hzn_save_open_read(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                            const char *guest_path, FILE **stream)
{
    return open_save_file(root, uid_hi, uid_lo, guest_path, stream, false);
}

uint32_t hzn_save_open_write(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                             const char *guest_path, FILE **stream)
{
    return open_save_file(root, uid_hi, uid_lo, guest_path, stream, true);
}

uint32_t hzn_save_flush(FILE *stream)
{
    if (!stream)
        return FS_INVALID_ARGUMENT;
    if (fflush(stream))
        return FS_GENERIC_FAILURE;
#ifdef _WIN32
    return _commit(_fileno(stream)) == 0 ? 0 : FS_GENERIC_FAILURE;
#else
    return fsync(fileno(stream)) == 0 ? 0 : FS_GENERIC_FAILURE;
#endif
}

uint32_t hzn_save_write_file(const char *root, uint64_t uid_hi, uint64_t uid_lo, FILE *stream,
                             uint64_t offset, const void *data, size_t bytes, bool allow_append, bool flush)
{
    if (!stream || (!data && bytes))
        return FS_INVALID_ARGUMENT;
    if (offset > INT64_MAX || bytes > INT64_MAX - offset)
        return FS_OUT_OF_RANGE;

    clearerr(stream);
#ifdef _WIN32
    const int64_t length = _fseeki64(stream, 0, SEEK_END) ? -1 : _ftelli64(stream);
#else
    const int64_t length = fseeko(stream, 0, SEEK_END) ? -1 : ftello(stream);
#endif
    if (length < 0)
        return FS_GENERIC_FAILURE;
    const uint64_t end = offset + bytes;
    if (end > (uint64_t)length) {
        if (!allow_append)
            return FS_NO_APPEND;
        if (bytes) {
            uint64_t free_bytes;
            const uint32_t result = hzn_save_free_space(root, uid_hi, uid_lo, "/", &free_bytes);
            if (result)
                return result;
            if (end - (uint64_t)length > free_bytes)
                return FS_GENERIC_FAILURE;
        }
    }
    if (bytes) {
#ifdef _WIN32
        const int seek_error = _fseeki64(stream, (int64_t)offset, SEEK_SET);
#else
        const int seek_error = fseeko(stream, (off_t)offset, SEEK_SET);
#endif
        if (seek_error || fwrite(data, 1, bytes, stream) != bytes || fflush(stream))
            return FS_GENERIC_FAILURE;
    }
    return flush ? hzn_save_flush(stream) : 0;
}

uint32_t hzn_save_commit(const char *root, uint64_t uid_hi, uint64_t uid_lo)
{
    char path[HZN_SAVE_PATH_MAX];
    const uint32_t result = hzn_save_open_account(root, uid_hi, uid_lo, path);
#ifndef _WIN32
    if (!result && !sync_directory(path))
        return FS_GENERIC_FAILURE;
#endif
    /* Writable handles use synchronous host writes. The FSP caller flushes
     * any writable streams that remain open before committing the volume. */
    return result;
}

static bool charge_file(uint64_t bytes, uint64_t *free_bytes)
{
    *free_bytes -= bytes < *free_bytes ? bytes : *free_bytes;
    return true;
}

static bool scan_save_usage(char path[SAVE_ENTRY_PATH_MAX], size_t used, uint64_t *free_bytes);

static bool scan_save_child(char path[SAVE_ENTRY_PATH_MAX], size_t used, const char *name,
                            uint64_t *free_bytes)
{
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return true;
    const size_t length = strlen(name);
    if (length >= SAVE_ENTRY_PATH_MAX - used - 1)
        return false;
    path[used] = '/';
    memcpy(path + used + 1, name, length + 1);
    bool result = false;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &info)) {
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
            result = true;
        else if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            result = scan_save_usage(path, used + length + 1, free_bytes);
        else
            result = charge_file((uint64_t)info.nFileSizeHigh << 32 | info.nFileSizeLow, free_bytes);
    }
#else
    struct stat info;
    if (!lstat(path, &info)) {
        if (S_ISDIR(info.st_mode))
            result = scan_save_usage(path, used + length + 1, free_bytes);
        else if (S_ISREG(info.st_mode) && info.st_size >= 0)
            result = charge_file((uint64_t)info.st_size, free_bytes);
        else
            result = true;
    }
#endif
    path[used] = '\0';
    return result;
}

static bool scan_save_usage(char path[SAVE_ENTRY_PATH_MAX], size_t used, uint64_t *free_bytes)
{
    if (!*free_bytes)
        return true;
#ifdef _WIN32
    if (used > SAVE_ENTRY_PATH_MAX - 3)
        return false;
    memcpy(path + used, "/*", 3);
    WIN32_FIND_DATAA entry;
    HANDLE directory = FindFirstFileA(path, &entry);
    path[used] = '\0';
    if (directory == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool result = true;
    do {
        if (!scan_save_child(path, used, entry.cFileName, free_bytes)) {
            result = false;
            break;
        }
        if (!*free_bytes)
            break;
        if (!FindNextFileA(directory, &entry)) {
            result = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    } while (true);
    return FindClose(directory) && result;
#else
    DIR *directory = opendir(path);
    if (!directory)
        return false;
    bool result = true;
    while (*free_bytes) {
        errno = 0;
        const struct dirent *entry = readdir(directory);
        if (!entry) {
            result = errno == 0;
            break;
        }
        if (!scan_save_child(path, used, entry->d_name, free_bytes)) {
            result = false;
            break;
        }
    }
    return closedir(directory) == 0 && result;
#endif
}

uint32_t hzn_save_free_space(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                             const char *guest_path, uint64_t *bytes)
{
    if (!bytes)
        return FS_INVALID_ARGUMENT;
    char path[SAVE_ENTRY_PATH_MAX];
    SaveNode kind;
    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &kind, false);
    if (result)
        return result;
    int64_t normal, journal;
    result = hzn_save_account_size(root, uid_hi, uid_lo, &normal, &journal);
    if (result)
        return result;
    result = hzn_save_open_account(root, uid_hi, uid_lo, path);
    if (result)
        return result;
    uint64_t remaining = (uint64_t)normal;
    /* R74: the directory scan fails on the PS5 (opendir/readdir); free space
     * is advisory, so report the whole quota rather than failing the query. */
    if (!scan_save_usage(path, strlen(path), &remaining))
        remaining = (uint64_t)normal;
    *bytes = remaining;
    return 0;
}
