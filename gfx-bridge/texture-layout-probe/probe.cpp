#include "nvn_texture_layout.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include <cstdio>

void nxrt::logf(LogLevel, const char *tag, const char *format, ...)
{
    std::fprintf(stderr, "%s ", tag);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

int main()
{
    static const nxrt::game::Info game{.platform = nxrt::game::Platform::Switch2};
    nxrt::game::set_current(&game);
    nxrt::nvn::TextureBuilderInfo builder;
    builder.width = 1920;
    builder.height = 1080;
    builder.depth = 1;
    builder.target = NVN_TEXTURE_TARGET_2D;
    builder.format = NVN_FORMAT_RGBA8_SRGB;
    builder.flags = 0x0d;
    auto display = nxrt::nvn::be::compute_texture_layout(builder);
    if (!display.valid || !display.desc.gob_switch2 || !display.fmt ||
        display.fmt->vk != VK_FORMAT_R8G8B8A8_SRGB)
        return 1;
    std::printf("Display baseline %llx bytes / %llx alignment\n",
                (unsigned long long)display.storage_size, (unsigned long long)display.alignment);
    builder.flags = 0;
    builder.width = 36;
    builder.height = 36;
    builder.format = NVN_FORMAT_BPTC_UNORM;
    builder.has_packaged_layout = true;
    auto bc7 = nxrt::nvn::be::compute_texture_layout(builder);
    return bc7.valid && bc7.desc.gob_switch2 && bc7.alignment == 0x200 &&
        bc7.storage_size == 0xc00 ? 0 : 2;
}



