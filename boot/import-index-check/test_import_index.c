#include "../boot.h"

#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char import_shared[] = "shared";
static const char export_shared[] = "shared";

static const DynSym main_symbols[] = {
    {"z-last", 0x1a0, 1},
    {export_shared, 0x1b0, 1},
    {"shared", 0x1c0, 1},
    {"zero-then-value", 0, 1},
    {"absolute-zero", 0, 1},
    {"undefined-with-value", 0xdead, 0},
    {"same-module-order", 0x1d0, 1},
    {"same-module-order", 0x1e0, 1},
};

static const DynSym sdk_symbols[] = {
    {"zero-then-value", 0x2a0, 1},
    {"a-first", 0x2b0, 1},
    {"shared", 0x2c0, 1},
    {"absolute-zero", 0x2d0, 1},
    {"zero-only", 0, 1},
};

static const DynSym subsdk_symbols[] = {
    {import_shared, 0, 0},
    {"same-module-order", 0, 0},
    {"z-last", 0, 0},
    {"a-first", 0, 0},
    {"zero-then-value", 0, 0},
    {"zero-only", 0, 0},
    {"missing-name", 0, 0},
    {"undefined-with-value", 0, 0},
};

static void entry(Ctx *cpu) { (void)cpu; }

static const FnEntry main_functions[] = {{0x100, entry}};
static const FnEntry sdk_functions[] = {{0x200, entry}};
static const FnEntry subsdk_functions[] = {{0x300, entry}};

const NxModule nxmod_main = {
    .name = "main", .index = 30, .base = 0x100, .text_end = 0x200,
    .fns = main_functions, .nfns = _countof(main_functions),
    .syms = main_symbols, .nsyms = _countof(main_symbols),
};
const NxModule nxmod_sdk = {
    .name = "sdk", .index = 8, .base = 0x200, .text_end = 0x300,
    .fns = sdk_functions, .nfns = _countof(sdk_functions),
    .syms = sdk_symbols, .nsyms = _countof(sdk_symbols),
};
const NxModule nxmod_subsdk0 = {
    .name = "subsdk0", .index = 3, .base = 0x300, .text_end = 0x400,
    .fns = subsdk_functions, .nfns = _countof(subsdk_functions),
    .syms = subsdk_symbols, .nsyms = _countof(subsdk_symbols),
};
const uint32_t nxmod_main_abi = NXRECOMP_ABI_VERSION;
const uint32_t nxmod_sdk_abi = NXRECOMP_ABI_VERSION;
const uint32_t nxmod_subsdk0_abi = NXRECOMP_ABI_VERSION;

_Noreturn void guest_trap(const char *message, uint64_t pc)
{
    fprintf(stderr, "Unexpected guest trap at %#" PRIx64 " (%s)\n", pc, message);
    abort();
}

static void check_import(unsigned module, unsigned symbol, const char *expected_name, uint64_t expected_value)
{
    const char *name = "not written";
    uint64_t value = pokopia_boot_import(NX_IMPORT_ID(module, symbol), &name);
    if (value != expected_value || (expected_name ? !name || strcmp(name, expected_name) : name != NULL)) {
        fprintf(stderr, "Module %u symbol %u resolved to %s %#" PRIx64 ", expected %s %#" PRIx64 "\n",
                module, symbol, name ? name : "(null)", value,
                expected_name ? expected_name : "(null)", expected_value);
        exit(EXIT_FAILURE);
    }
}

static void run_cases(void)
{
    check_import(3, 0, "shared", 0x1b0);
    check_import(3, 1, "same-module-order", 0x1d0);
    check_import(3, 2, "z-last", 0x1a0);
    check_import(3, 3, "a-first", 0x2b0);
    check_import(3, 4, "zero-then-value", 0x2a0);
    check_import(3, 5, "zero-only", 0);
    check_import(3, 6, "missing-name", 0);
    check_import(3, 7, "undefined-with-value", 0);
    check_import(30, 2, "shared", 0x1c0);
    check_import(8, 2, "shared", 0x2c0);
    check_import(30, 3, "zero-then-value", 0);
    check_import(30, 4, "absolute-zero", 0);
    check_import(8, 5, NULL, 0);
    check_import(99, 0, NULL, 0);
    const char *name = NULL;
    assert(pokopia_boot_import(NX_IMPORT_ID(3, 0), &name) == 0x1b0);
    assert(name == import_shared && name != export_shared);
}

int main(void)
{
    run_cases();
    puts("PASS 15 import cases before initialization");
    FILE *log = fopen("boot-index.log", "w+");
    assert(log);
    assert(pokopia_boot_init(log, 0));
    rewind(log);
    char line[256];
    bool index_built = false;
    while (fgets(line, sizeof line, log)) {
        fputs(line, stdout);
        if (strcmp(line, "Indexed 9 translated exports\n") == 0)
            index_built = true;
    }
    assert(index_built);
    run_cases();
    puts("PASS 15 import cases with verified index");
    assert(pokopia_boot_release_heap(log));
    run_cases();
    puts("PASS 15 import cases after index release");
    fclose(log);
    return 0;
}
