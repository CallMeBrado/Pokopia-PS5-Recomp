from pathlib import Path
import hashlib
import re
import struct

p = Path('.')
code = (p/'composite-missing/code_caa3cba58abf8cc9.bin').read_bytes()
control = (p/'composite-missing/ctrl_caa3cba58abf8cc9.bin').read_bytes()
size, offset = struct.unpack_from('<II', control, 0x3c)
assert (size, offset) == (48, 512)
assert hashlib.sha1(code).hexdigest().startswith('caa3cba58abf8cc9')
cb = code[offset:offset+size]
print('Vertex constants',struct.unpack('<12f',cb))
trace = (p/'composite-dump.log').read_text(encoding='utf-8')
header = []
for stage in ('vs', 'fs'):
    matches = re.findall(rf'{stage} ubo2 [0-9a-f]+/512: (44f00000 44870000 3a088889 3a72b9d6[^\r\n]+)', trace)
    values = [int(v, 16) for v in matches[-1].split()]
    assert len(values) == 128
    header.append(f'static const uint32_t {stage}_composite[128] = {{')
    for i in range(0, 128, 8):
        header.append('    ' + ', '.join(f'0x{x:08x}' for x in values[i:i+8]) + ',')
    header.append('};')
header.append('static const uint8_t c1_composite[48] = {')
for i in range(0, 48, 12):
    header.append('    ' + ', '.join(f'0x{x:02x}' for x in cb[i:i+12]) + ',')
header.append('};')
(p/'composite-data.h').write_text('\n'.join(header)+'\n',encoding='ascii')

prior = (p/'../r39-feedback-reuse-check/test-post-depth.c').read_text()
head = prior[:prior.index('static void upload_exposure')]
head = head.replace('#include "texture_upload.h"', '#include "composite-data.h"')
head = head.replace('const uint8_t expected[4])','uint8_t sample[4])')
head = head.replace('''    for (size_t i = 0; i < count; ++i)
        assert(!memcmp(bytes + i * 4, expected, 4));''','''    memcpy(sample, bytes + (count / 2 + color->extent.width / 2) * 4, 4);
    size_t matching = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *pixel = bytes + i * 4;
        assert(memcmp(pixel, (const uint8_t[4]){255, 0, 255, 255}, 4));
        assert(pixel[3] == 255);
        matching += !memcmp(pixel, sample, 4);
    }
    printf("center [%u,%u,%u,%u], matching pixels %zu/%zu\\n",
           sample[0], sample[1], sample[2], sample[3], matching, count);
    assert(matching > count / 100);''')
open_texture = prior[prior.index('static void open_texture'):prior.index('static PFN_vkGetDeviceProcAddr')]
format_hooks = prior[prior.index('VkFormat nvn_guest_texture_vk_format'):]
(p/'test-composite.c').write_text(head + open_texture + (p/'composite-fixture.inc').read_text() + format_hooks,encoding='ascii')
