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
    ('uint32_t hzn_save_get_entry_type(',
     '''uint32_t hzn_save_create_directory(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                                   const char *guest_path);
uint32_t hzn_save_get_entry_type(''')
])

edit('horizon/save_store.c', [
    ('    FS_PATH_NOT_FOUND = 0x202,', '    FS_PATH_NOT_FOUND = 0x202,\n    FS_PATH_ALREADY_EXISTS = 0x402,'),
    ('''                              const char *guest_path, char path[SAVE_ENTRY_PATH_MAX], SaveNode *type)
{''',
     '''                              const char *guest_path, char path[SAVE_ENTRY_PATH_MAX],
                              SaveNode *type, bool allow_missing_leaf)
{'''),
    ('''        if (*type == NODE_MISSING || (separator && *type != NODE_DIRECTORY))
            return FS_PATH_NOT_FOUND;''',
     '''        if (separator ? *type != NODE_DIRECTORY : (*type == NODE_MISSING && !allow_missing_leaf))
            return FS_PATH_NOT_FOUND;'''),
    ('''uint32_t hzn_save_get_entry_type(''',
     '''uint32_t hzn_save_create_directory(const char *root, uint64_t uid_hi, uint64_t uid_lo,
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
    if (!mkdir(path, 0700))
        return 0;
    if (errno == EEXIST)
        return FS_PATH_ALREADY_EXISTS;
    if (errno == ENOENT || errno == ENOTDIR)
        return FS_PATH_NOT_FOUND;
#endif
    return FS_GENERIC_FAILURE;
}

uint32_t hzn_save_get_entry_type('''),
    ('''    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &node_type);
    if (!result)''',
     '''    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &node_type, false);
    if (!result)'''),
    ('''    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &node_type);
    if (result)''',
     '''    uint32_t result = resolve_entry(root, uid_hi, uid_lo, guest_path, path, &node_type, false);
    if (result)'''),
    ('''resolve_entry(root, uid_hi, uid_lo, guest_path, path, &kind);''',
     '''resolve_entry(root, uid_hi, uid_lo, guest_path, path, &kind, false);''')
])

edit('gfx-bridge/fsp.c', [
    ('''static HznFspReply get_save_entry_type(''',
     '''static HznFspReply create_save_directory(const HznFsp *fsp, uint8_t *tls, size_t size,
                                         const HznCmifRequest *request, size_t in_size)
{
    if (in_size != 16 || size < HZN_CMIF_DOMAIN_REPLY_BYTES || !is_save_path_request(fsp, tls, request))
        return svc_result(INVALID_COMBINATION);
    const char *path = read_save_path(fsp, request);
    if (!path)
        return svc_result(INVALID_ADDRESS);
    const uint32_t result = hzn_save_create_directory(fsp->save_root, HZN_OFFLINE_UID_WORD0,
                                                      HZN_OFFLINE_UID_WORD1, path);
    return reply_cmif(tls, size, true, result, NULL);
}

static HznFspReply get_save_entry_type('''),
    ('''    if (save) {
        if (command == 7)''',
     '''    if (save) {
        if (command == 2)
            return create_save_directory(fsp, tls, size, &request, in_size);
        if (command == 7)''')
])
