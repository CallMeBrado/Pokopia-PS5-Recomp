from pathlib import Path

root = Path(__file__).resolve().parents[2]
def edit(name, replacements):
    path = root/name
    source = path.read_text()
    for old, new in replacements:
        assert source.count(old) == 1, name
        source = source.replace(old, new)
    path.write_text(source)

edit('horizon/save_store.h', [
    ('#define HZN_SAVE_TITLE_ID UINT64_C(0x0400a3a01ee4c000)',
     '''#define HZN_SAVE_TITLE_ID UINT64_C(0x0400a3a01ee4c000)
/* Fixed account save quotas supported by this title's host backend. */
#define HZN_SAVE_ACCOUNT_DATA_LIMIT INT64_C(0x4000000)
#define HZN_SAVE_ACCOUNT_JOURNAL_LIMIT INT64_C(0x4000000)''')
])

edit('horizon/save_store.c', [
    ('#include <errno.h>', '#include <errno.h>\n#include <inttypes.h>'),
    ('''    /* This title's control metadata specifies 64 MiB each for data and journal. */
    int size = snprintf(contents, sizeof contents,
                        "size=67108864\\njournal=67108864\\nowner=400a3a01ee4c000\\nattr=%s\\n", attr_hex);''',
     '''    int size = snprintf(contents, sizeof contents,
                        "size=%" PRId64 "\\njournal=%" PRId64 "\\nowner=400a3a01ee4c000\\nattr=%s\\n",
                        HZN_SAVE_ACCOUNT_DATA_LIMIT, HZN_SAVE_ACCOUNT_JOURNAL_LIMIT, attr_hex);''')
])

edit('horizon/applet.h', [
    ('    void *save_opaque;',
     '    void *save_opaque;\n    int64_t save_data_limit, save_journal_limit;')
])

edit('horizon/applet.c', [
    ('    GET_SAVE_DATA_SIZE = 26,', '    GET_SAVE_DATA_SIZE = 26,\n    GET_SAVE_DATA_SIZE_MAX = 28,'),
    ('''    if (object->type == application_functions && command == POP_LAUNCH_PARAMETER) {''',
     '''    if (object->type == application_functions && command == GET_SAVE_DATA_SIZE_MAX) {
        const int64_t quotas[] = {applet->save_data_limit, applet->save_journal_limit};
        if (in_size != 16 || request.has_pid || request.has_copy_handle || request.has_buffer_descriptors ||
            size < HZN_CMIF_DOMAIN_REPLY_BYTES + sizeof quotas)
            return svc_result(INVALID_COMBINATION);
        if (quotas[0] <= 0 || quotas[1] <= 0)
            return boundary(command, id, application_functions);
        return svc_result(hzn_cmif_reply_data(tls, size, true, 0, quotas, sizeof quotas)
                          ? 0 : INVALID_COMBINATION);
    }
    if (object->type == application_functions && command == POP_LAUNCH_PARAMETER) {''')
])

edit('loader-probe/loader_probe.c', [
    ('                slot->state.applet.get_save_size = get_applet_save_size;',
     '''                slot->state.applet.get_save_size = get_applet_save_size;
                slot->state.applet.save_data_limit = HZN_SAVE_ACCOUNT_DATA_LIMIT;
                slot->state.applet.save_journal_limit = HZN_SAVE_ACCOUNT_JOURNAL_LIMIT;''')
])
