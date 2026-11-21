from pathlib import Path

prior = Path('../depth-layer-replay-work/test-depth.c').read_text()
head = prior[:prior.index('static void seed_color')].replace(
    'enum { WIDTH = 1920, HEIGHT = 1080, PIXELS = WIDTH * HEIGHT, LAYERS = 5 };',
    'enum { WIDTH = 80, HEIGHT = 45, PIXELS = WIDTH * HEIGHT };')
readback = prior[prior.index('static void readback'):prior.index('static VkFormat physical_depth_format')]
readback = readback.replace('PIXELS * 4 * LAYERS', 'PIXELS * 4').replace('VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS', 'VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1')
start = readback.index('    if (color) {', readback.index('const uint8_t *bytes = mapped;'))
end = readback.index('    const uint32_t *depth_words', start)
readback = readback[:start] + '''    if (color) {
        printf("Readback RGBA [%u,%u,%u,%u]\\n", bytes[0], bytes[1], bytes[2], bytes[3]);
        for (size_t i = 0; i < PIXELS; ++i) {
            const uint8_t *pixel = bytes + i * 4;
            assert(abs((int)pixel[0] - 64) <= 1 && abs((int)pixel[1] - 128) <= 1 &&
                   abs((int)pixel[2] - 191) <= 1 && pixel[3] == 255);
        }
    }
''' + readback[end:]
texture = Path('../r39-feedback-reuse-check/test-post-depth.c').read_text()
texture = texture[texture.index('static void open_texture'):texture.index('static PFN_vkGetDeviceProcAddr')]
Path('test-title-passes.c').write_text(head + readback + texture + Path('title-passes-fixture.inc').read_text())
