from pathlib import Path
prior = Path('../r39-feedback-reuse-check/test-post-depth.c').read_text()
text = prior[:prior.index('static void upload_exposure')]
text = text.replace('#include "live_draw.h"', '#include "live_draw.c"')
text = text.replace('#include "texture_upload.h"', '#include <math.h>')
text = text.replace('enum { WIDTH = 1920, HEIGHT = 1080, PIXELS = WIDTH * HEIGHT, LAYERS = 5 };',
                    'enum { WIDTH = 1280, HEIGHT = 720 };')
text = text.replace('const uint8_t expected[4])', 'const uint16_t expected[4])')
text = text.replace('.size = count * 4,', '.size = count * 8,')
text = text.replace('''    const uint8_t *bytes = mapped;
    printf("GPU readback %ux%u [%u,%u,%u,%u]\\n", color->extent.width, color->extent.height,
           bytes[0], bytes[1], bytes[2], bytes[3]);
    for (size_t i = 0; i < count; ++i)
        assert(!memcmp(bytes + i * 4, expected, 4));''',
'''    const uint16_t *samples = mapped;
    printf("GPU readback %ux%u RGBA16 [%u,%u,%u,%u], expected [%u,%u,%u,%u]\\n",
           color->extent.width, color->extent.height, samples[0], samples[1], samples[2], samples[3],
           expected[0], expected[1], expected[2], expected[3]);
    for (size_t i = 0; i < count; ++i) {
        for (unsigned component = 0; component < 3; ++component)
            assert(abs((int)samples[4 * i + component] - expected[component]) <= 2);
        assert(samples[4 * i + 3] == expected[3]);
    }''')
open_texture = prior[prior.index('static void open_texture'):prior.index('static PFN_vkGetDeviceProcAddr')]
Path('test-additive.c').write_text(text + open_texture + Path('additive-fixture.inc').read_text())
