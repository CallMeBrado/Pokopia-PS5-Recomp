#define _CRT_SECURE_NO_WARNINGS
#include "../../horizon/save_store.h"
#include "../../horizon/account_identity.h"

#include <assert.h>
#include <direct.h>
#include <string.h>
#include <windows.h>

static const uint64_t hi = HZN_OFFLINE_UID_WORD0, lo = HZN_OFFLINE_UID_WORD1;

int main(void)
{
    char root[96];
    snprintf(root, sizeof root, "directory-%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    assert(!hzn_save_ensure_account(root, hi, lo));
    assert(hzn_save_create_directory(root, hi, lo, "/missing/child") == 0x202);
    assert(hzn_save_create_directory(root, hi, lo, "/") == 0x402);
    assert(!hzn_save_create_directory(root, hi, lo, "/profiles"));
    assert(!hzn_save_create_directory(root, hi, lo, "/profiles/first"));
    assert(hzn_save_create_directory(root, hi, lo, "/profiles") == 0x402);
    assert(hzn_save_create_directory(root, hi, lo, "/profiles/../escape") == 0x2ee202);
    assert(hzn_save_create_directory(root, hi, lo, "/profiles/win\\escape") == 0x2ee202);

    const char *game_path = "/profiles/first/state.bin";
    FILE *file = NULL;
    assert(hzn_save_open_write(root, hi, lo, game_path, &file) == 0x202 && !file);
    assert(hzn_save_create_file(root, hi, lo, game_path, UINT64_MAX) == 0x177a02);
    assert(hzn_save_create_file(root, hi, lo, "/missing/state.bin", 16) == 0x202);
    assert(!hzn_save_create_file(root, hi, lo, game_path, 16));
    assert(hzn_save_create_file(root, hi, lo, game_path, 0) == 0x402);
    assert(hzn_save_create_directory(root, hi, lo, game_path) == 0x402);
    assert(hzn_save_create_directory(root, hi, lo, "/profiles/first/state.bin/x") == 0x202);
    uint32_t type;
    assert(!hzn_save_get_entry_type(root, hi, lo, "/profiles/first", &type) && type == 0);
    assert(!hzn_save_get_entry_type(root, hi, lo, game_path, &type) && type == 1);
    assert(!hzn_save_open_read(root, hi, lo, game_path, &file));
    for (unsigned i = 0; i < 16; ++i)
        assert(fgetc(file) == 0);
    assert(fgetc(file) == EOF && !fclose(file));

    assert(!hzn_save_open_write(root, hi, lo, game_path, &file));
    const unsigned char payload[] = {0x21, 0x43, 0x65, 0x87};
    assert(!hzn_save_write_file(root, hi, lo, file, 4, payload, sizeof payload, false, true));
    assert(hzn_save_write_file(root, hi, lo, file, 15, payload, sizeof payload, false, false) == 0x307202);
    assert(hzn_save_write_file(root, hi, lo, file, INT64_MAX, payload, sizeof payload, true, false) == 0x177a02);

    char mounted[HZN_SAVE_PATH_MAX], meta[HZN_SAVE_PATH_MAX + 16], disk[HZN_SAVE_PATH_MAX + 32];
    assert(!hzn_save_open_account(root, hi, lo, mounted));
    snprintf(meta, sizeof meta, "%s.meta", mounted);
    FILE *metadata = fopen(meta, "wb");
    assert(metadata && fputs("size=24\njournal=24\n", metadata) >= 0 && !fclose(metadata));
    assert(hzn_save_write_file(root, hi, lo, file, 23, payload, sizeof payload, true, false) == 0x3e802);
    assert(hzn_save_create_file(root, hi, lo, "/quota.bin", 9) == 0x3e802);
    assert(hzn_save_get_entry_type(root, hi, lo, "/quota.bin", &type) == 0x202);
    assert(!hzn_save_write_file(root, hi, lo, file, 19, payload, sizeof payload, true, false));
    assert(!hzn_save_flush(file) && !fclose(file));
    assert(!hzn_save_commit(root, hi, lo));
    assert(!hzn_save_open_read(root, hi, lo, game_path, &file));
    unsigned char data[23], expected[23] = {0};
    memcpy(expected + 4, payload, sizeof payload);
    memcpy(expected + 19, payload, sizeof payload);
    assert(fread(data, 1, sizeof data, file) == sizeof data && !memcmp(data, expected, sizeof data));
    assert(fgetc(file) == EOF && !fclose(file));
    uint64_t free_bytes;
    assert(!hzn_save_free_space(root, hi, lo, "/", &free_bytes) && free_bytes == 1);

    snprintf(disk, sizeof disk, "%s%s", mounted, game_path);
    assert(!remove(disk));
    snprintf(disk, sizeof disk, "%s/profiles/first", mounted);
    assert(!_rmdir(disk));
    snprintf(disk, sizeof disk, "%s/profiles", mounted);
    assert(!_rmdir(disk));
    assert(!remove(meta) && !_rmdir(mounted));
    snprintf(disk, sizeof disk, "%s/account", root);
    assert(!_rmdir(disk) && !_rmdir(root));
    puts("PASS mounted CreateDirectory, exclusive CreateFile, zero bytes, overwrite/append, quota, flush and commit persistence");
    return 0;
}
