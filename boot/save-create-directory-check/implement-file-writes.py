from pathlib import Path

root = Path(__file__).resolve().parents[2]
def edit(name, replacements):
    path = root / name
    source = path.read_text()
    for old, new in replacements:
        assert source.count(old) == 1, (name, old, source.count(old))
        source = source.replace(old, new)
    path.write_text(source)

edit('horizon/save_store.h', [
    ('#include <stdint.h>', '#include <stdbool.h>\n#include <stdint.h>'),
    ('''uint32_t hzn_save_get_entry_type(''',
     '''uint32_t hzn_save_create_file(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                              const char *guest_path, uint64_t size);
uint32_t hzn_save_get_entry_type('''),
    ('''#endif''',
     '''uint32_t hzn_save_open_write(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                             const char *guest_path, FILE **stream);
uint32_t hzn_save_write_file(const char *root, uint64_t uid_hi, uint64_t uid_lo, FILE *stream,
                             uint64_t offset, const void *data, size_t bytes, bool allow_append, bool flush);
uint32_t hzn_save_flush(FILE *stream);
uint32_t hzn_save_commit(const char *root, uint64_t uid_hi, uint64_t uid_lo);

#endif''')
])

edit('horizon/save_store.c', [
    ('''    FS_INVALID_ARGUMENT = 0x2ee202,''',
     '''    FS_OUT_OF_RANGE = 0x177a02,
    FS_INVALID_ARGUMENT = 0x2ee202,
    FS_NO_APPEND = 0x307202,'''),
    ('''static uint32_t open_regular_read(const char *path, FILE **stream, uint32_t not_found)
{
#ifdef _WIN32
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);''',
     '''static uint32_t open_regular_file(const char *path, FILE **stream, uint32_t not_found, bool write)
{
#ifdef _WIN32
    const DWORD access = GENERIC_READ | (write ? GENERIC_WRITE : 0);
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                        (write ? FILE_FLAG_WRITE_THROUGH : 0);
    HANDLE file = CreateFileA(path, access, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);'''),
    ('''    int fd = _open_osfhandle((intptr_t)file, _O_RDONLY | _O_BINARY);''',
     '''    int fd = _open_osfhandle((intptr_t)file, (write ? _O_RDWR : _O_RDONLY) | _O_BINARY);'''),
    ('''    *stream = _fdopen(fd, "rb");''',
     '''    *stream = _fdopen(fd, write ? "r+b" : "rb");'''),
    ('''    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);''',
     '''    int fd = open(path, (write ? O_RDWR | O_SYNC : O_RDONLY) | O_NOFOLLOW | O_NONBLOCK);'''),
    ('''        *stream = fdopen(fd, "rb");''',
     '''        *stream = fdopen(fd, write ? "r+b" : "rb");'''),
    ('''    result = open_regular_read(meta, &stream, FS_TARGET_NOT_FOUND);''',
     '''    result = open_regular_file(meta, &stream, FS_TARGET_NOT_FOUND, false);'''),
    ('''uint32_t hzn_save_create_directory(''',
     '''#ifndef _WIN32
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
    *slash = '\\0';
    const bool synced = sync_directory(path);
    *slash = '/';
    return synced;
}
#endif

uint32_t hzn_save_create_directory('''),
    ('''    if (!mkdir(path, 0700))
        return 0;
    if (errno == EEXIST)''',
     '''    if (!mkdir(path, 0700)) {
        if (sync_parent(path))
            return 0;
        rmdir(path);
        return FS_GENERIC_FAILURE;
    }
    if (errno == EEXIST)'''),
    ('''uint32_t hzn_save_get_entry_type(''',
     '''uint32_t hzn_save_create_file(const char *root, uint64_t uid_hi, uint64_t uid_lo,
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

uint32_t hzn_save_get_entry_type('''),
    ('''uint32_t hzn_save_open_read(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                            const char *guest_path, FILE **stream)''',
     '''static uint32_t open_save_file(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               const char *guest_path, FILE **stream, bool write)'''),
    ('''    return open_regular_read(path, stream, FS_PATH_NOT_FOUND);
}''',
     '''    return open_regular_file(path, stream, FS_PATH_NOT_FOUND, write);
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
}''')
])
