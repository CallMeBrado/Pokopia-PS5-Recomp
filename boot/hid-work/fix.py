from pathlib import Path
p=Path('hid.h')
s=p.read_text()
s=s.replace('typedef struct {\n    void *opaque;', '''typedef struct {
    uint64_t aruid;
    uint32_t styles, ids;
    bool npad_active, touch_active;
} HznHidState;

typedef struct {
    void *opaque;
    const void *(*read_guest)(void *opaque, uint64_t address, size_t bytes);''', 1)
p.write_text(s)
p=Path('hid.c')
s=p.read_text().replace('    NPAD_BATTERY = 0x419c, NPAD_LEFT_BATTERY = 0x41a0, NPAD_RIGHT_BATTERY = 0x41a4,\n', '')
s=s.replace('    if (!base)\n        return;\n    if (touch_enabled)',
            '    if (!base || !hid)\n        return;\n    if (hid->touch_active)')
p.write_text(s)
