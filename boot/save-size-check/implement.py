from pathlib import Path

root = Path(__file__).resolve().parents[2]
def write(name, before, after):
    if before != after:
        path = root/name
        assert path.read_text() == before, name
        path.write_text(after)

name = 'horizon/applet.h'
before = (root/name).read_text()
after = before.replace('typedef uint32_t (*HznAppletEnsureSave)(void *opaque, uint64_t uid_hi, uint64_t uid_lo);',
                       '''typedef uint32_t (*HznAppletEnsureSave)(void *opaque, uint64_t uid_hi, uint64_t uid_lo);
typedef uint32_t (*HznAppletGetSaveSize)(void *opaque, uint64_t uid_hi, uint64_t uid_lo,
                                        int64_t *normal, int64_t *journal);''')
after = after.replace('    HznAppletEnsureSave ensure_save;', '    HznAppletEnsureSave ensure_save;\n    HznAppletGetSaveSize get_save_size;')
write(name,before,after)

name = 'horizon/applet.c'
before = (root/name).read_text()
after = before.replace('    SET_TERMINATE_RESULT = 22,', '''    SET_TERMINATE_RESULT = 22,
    GET_SAVE_DATA_SIZE = 26,
    SAVE_DATA_ACCOUNT = 1,''')
anchor = '    if (object->type == application_functions && command == POP_LAUNCH_PARAMETER) {'
assert after.count(anchor) == 1
after = after.replace(anchor, '''    if (object->type == application_functions && command == GET_SAVE_DATA_SIZE) {
        int64_t quotas[2] = {0};
        if (in_size != 40 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            size < HZN_CMIF_DOMAIN_REPLY_BYTES + sizeof quotas)
            return svc_result(INVALID_COMBINATION);
        if (raw[32] != SAVE_DATA_ACCOUNT || !applet->get_save_size)
            return boundary(command, id, application_functions);
        const uint64_t uid_hi = hzn_cmif_read64(raw + 40);
        const uint64_t uid_lo = hzn_cmif_read64(raw + 48);
        const uint32_t result = applet->get_save_size(applet->save_opaque, uid_hi, uid_lo, &quotas[0], &quotas[1]);
        return svc_result(hzn_cmif_reply_data(tls, size, true, result, quotas, sizeof quotas)
                          ? 0 : INVALID_COMBINATION);
    }
''' + anchor)
write(name,before,after)

name = 'horizon/save_store.h'
before = (root/name).read_text()
anchor = 'uint32_t hzn_save_get_entry_type('
assert before.count(anchor) == 1
after = before.replace(anchor, '''uint32_t hzn_save_account_size(const char *root, uint64_t uid_hi, uint64_t uid_lo,
                               int64_t *normal, int64_t *journal);
''' + anchor)
write(name,before,after)

name = 'horizon/save_store.c'
before = (root/name).read_text()
after = before.replace('#include <stdio.h>\n#include <string.h>', '#include <errno.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>')
after = after.replace('#else\n#include <errno.h>\n#include <fcntl.h>', '#else\n#include <fcntl.h>')
start = after.index('#ifdef _WIN32\n    HANDLE file = CreateFileA(path, GENERIC_READ')
end = after.index('\n}',start)
helper = after[start:end]
after = after[:start] + '    return open_regular_read(path, stream, FS_PATH_NOT_FOUND);' + after[end:]
helper = helper.replace('? FS_PATH_NOT_FOUND : FS_GENERIC_FAILURE;', '? not_found : FS_GENERIC_FAILURE;')
anchor = 'static uint32_t resolve_entry('
assert after.count(anchor) == 1
after = after.replace(anchor, '''static uint32_t open_regular_read(const char *path, FILE **stream, uint32_t not_found)
{
''' + helper + '''
}

static bool parse_quota(const char *value, int64_t *quota)
{
    if (*value < '0' || *value > '9')
        return false;
    errno = 0;
    char *end;
    const long long parsed = strtoll(value, &end, 10);
    if (errno || parsed < 0 || (*end && strcmp(end, "\\n") && strcmp(end, "\\r\\n")))
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
    result = open_regular_read(meta, &stream, FS_TARGET_NOT_FOUND);
    if (result)
        return result;

    char line[224];
    int64_t quotas[2];
    unsigned seen = 0;
    while (fgets(line, sizeof line, stream)) {
        if (!strchr(line, '\\n') && !feof(stream)) {
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

''' + anchor)
assert '#include <stdbool.h>' not in after
after = after.replace('#include <errno.h>', '#include <errno.h>\n#include <stdbool.h>')
write(name,before,after)

name = 'loader-probe/loader_probe.c'
before = (root/name).read_text()
anchor = 'static bool init_account(ServiceSession *slot)'
assert before.count(anchor) == 1
after = before.replace(anchor, '''static uint32_t get_applet_save_size(void *opaque, uint64_t uid_hi, uint64_t uid_lo,
                                     int64_t *normal, int64_t *journal)
{
    (void)opaque;
    if (uid_hi != HZN_OFFLINE_UID_WORD0 || uid_lo != HZN_OFFLINE_UID_WORD1)
        return 0x7d402;
    return hzn_save_account_size(account_save_root, uid_hi, uid_lo, normal, journal);
}

''' + anchor)
anchor = '                slot->state.applet.ensure_save = ensure_applet_save;'
assert after.count(anchor) == 1
after = after.replace(anchor, anchor + '\n                slot->state.applet.get_save_size = get_applet_save_size;')
write(name,before,after)
